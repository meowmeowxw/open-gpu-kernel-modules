/*
 * NVIDIA Kernel Detective - RM Push Capture
 *
 * Captures RM internal CE pushbuffer submissions from channelFillGpFifo()
 * in nvidia.ko. Uses a separate relay channel under /sys/kernel/debug/nkd_rm/
 * independent from the UVM relay in nvidia-uvm.ko.
 *
 * Record format is struct nkd_push_record defined in nkd_common.h (shared with UVM).
 */

#include <linux/relay.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/percpu.h>

#include "../nkd_common.h"
#include "nkd_rm_capture.h"

/*
 * RM pushes are small: methodSizePerBlock = 0x68 (104 bytes).
 * Scratch buffer = header + max push data.
 */
#define NKD_RM_MAX_PUSH_SIZE  256  /* generous headroom beyond 104 */
#define NKD_RM_SCRATCH_SIZE   (NKD_RECORD_HDR_SIZE + NKD_RM_MAX_PUSH_SIZE)

/* Relay: 64 subbuffers x 16KB = 1MB per CPU (RM is infrequent) */
#define NKD_RM_SUBBUF_SIZE    (16 * 1024)
#define NKD_RM_N_SUBBUFS      64

/* Global enable flag */
int nkd_rm_enabled;

/* Statistics */
static atomic64_t nkd_rm_total_pushes = ATOMIC64_INIT(0);
static atomic64_t nkd_rm_total_bytes  = ATOMIC64_INIT(0);
static atomic64_t nkd_rm_dropped      = ATOMIC64_INIT(0);

/* Relay and debugfs */
static struct rchan  *nkd_rm_rchan;
static struct dentry *nkd_rm_debugfs_dir;

/* Per-CPU scratch buffers */
static DEFINE_PER_CPU(u8 *, nkd_rm_scratch_buf);

/* ---- Relay callbacks ---- */

static struct dentry *nkd_rm_create_buf_file(const char *filename,
                                              struct dentry *parent,
                                              umode_t mode,
                                              struct rchan_buf *buf,
                                              int *is_global)
{
    *is_global = 1;
    return debugfs_create_file(filename, mode, parent, buf,
                               &relay_file_operations);
}

static int nkd_rm_remove_buf_file(struct dentry *dentry)
{
    debugfs_remove(dentry);
    return 0;
}

static const struct rchan_callbacks nkd_rm_relay_cbs = {
    .create_buf_file = nkd_rm_create_buf_file,
    .remove_buf_file = nkd_rm_remove_buf_file,
};

/* ---- Debugfs: enabled ---- */

static ssize_t nkd_rm_enabled_read(struct file *file, char __user *buf,
                                    size_t count, loff_t *ppos)
{
    char tmp[4];
    int len = snprintf(tmp, sizeof(tmp), "%d\n", READ_ONCE(nkd_rm_enabled));
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t nkd_rm_enabled_write(struct file *file, const char __user *buf,
                                     size_t count, loff_t *ppos)
{
    int val, ret;
    ret = kstrtoint_from_user(buf, count, 10, &val);
    if (ret)
        return ret;
    WRITE_ONCE(nkd_rm_enabled, !!val);
    pr_info("nkd_rm: capture %s\n", nkd_rm_enabled ? "enabled" : "disabled");
    return count;
}

static const struct file_operations nkd_rm_enabled_fops = {
    .read  = nkd_rm_enabled_read,
    .write = nkd_rm_enabled_write,
};

/* ---- Debugfs: stats ---- */

static ssize_t nkd_rm_stats_read(struct file *file, char __user *buf,
                                  size_t count, loff_t *ppos)
{
    char tmp[256];
    int len = snprintf(tmp, sizeof(tmp),
                       "total_pushes: %lld\n"
                       "total_bytes:  %lld\n"
                       "dropped:      %lld\n"
                       "enabled:      %d\n",
                       atomic64_read(&nkd_rm_total_pushes),
                       atomic64_read(&nkd_rm_total_bytes),
                       atomic64_read(&nkd_rm_dropped),
                       READ_ONCE(nkd_rm_enabled));
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static const struct file_operations nkd_rm_stats_fops = {
    .read = nkd_rm_stats_read,
};

/* ---- Capture function ---- */

void nkd_rm_capture_push(const void *methods, u32 size,
                          u32 gpu_id, u32 channel_id, u32 class_id)
{
    struct nkd_push_record *rec;
    size_t record_size;
    u8 *scratch;

    if (size == 0 || size > NKD_RM_MAX_PUSH_SIZE) {
        atomic64_inc(&nkd_rm_dropped);
        return;
    }

    record_size = NKD_RECORD_HDR_SIZE + size;

    scratch = get_cpu_var(nkd_rm_scratch_buf);
    if (unlikely(!scratch)) {
        put_cpu_var(nkd_rm_scratch_buf);
        atomic64_inc(&nkd_rm_dropped);
        return;
    }

    rec = (struct nkd_push_record *)scratch;
    nkd_fill_record_header(rec, gpu_id, channel_id, size,
                           NKD_SOURCE_RM, class_id);
    memcpy(scratch + NKD_RECORD_HDR_SIZE, methods, size);

    if (likely(nkd_rm_rchan))
        relay_write(nkd_rm_rchan, scratch, record_size);

    put_cpu_var(nkd_rm_scratch_buf);

    atomic64_inc(&nkd_rm_total_pushes);
    atomic64_add(size, &nkd_rm_total_bytes);
}

/* ---- Init / Cleanup ---- */

int nkd_rm_init(void)
{
    int cpu;
    bool alloc_failed = false;

    /* Allocate per-CPU scratch buffers */
    for_each_possible_cpu(cpu) {
        u8 *buf = kmalloc(NKD_RM_SCRATCH_SIZE, GFP_KERNEL);
        if (!buf) {
            alloc_failed = true;
            break;
        }
        *per_cpu_ptr(&nkd_rm_scratch_buf, cpu) = buf;
    }

    if (alloc_failed) {
        pr_err("nkd_rm: failed to allocate per-CPU scratch buffers\n");
        for_each_possible_cpu(cpu) {
            u8 *buf = *per_cpu_ptr(&nkd_rm_scratch_buf, cpu);
            kfree(buf);
            *per_cpu_ptr(&nkd_rm_scratch_buf, cpu) = NULL;
        }
        return -ENOMEM;
    }

    /* Create debugfs directory */
    nkd_rm_debugfs_dir = debugfs_create_dir("nkd_rm", NULL);
    if (IS_ERR_OR_NULL(nkd_rm_debugfs_dir)) {
        pr_err("nkd_rm: failed to create debugfs dir\n");
        goto err_free_scratch;
    }

    /* Create relay channel */
    nkd_rm_rchan = relay_open("pushbuffer", nkd_rm_debugfs_dir,
                               NKD_RM_SUBBUF_SIZE, NKD_RM_N_SUBBUFS,
                               &nkd_rm_relay_cbs, NULL);
    if (!nkd_rm_rchan) {
        pr_err("nkd_rm: failed to open relay channel\n");
        goto err_remove_debugfs;
    }

    /* Create control files */
    debugfs_create_file("enabled", 0644, nkd_rm_debugfs_dir, NULL,
                        &nkd_rm_enabled_fops);
    debugfs_create_file("stats", 0444, nkd_rm_debugfs_dir, NULL,
                        &nkd_rm_stats_fops);

    WRITE_ONCE(nkd_rm_enabled, 0);

    pr_info("nkd_rm: initialized (capture disabled)\n");
    return 0;

err_remove_debugfs:
    debugfs_remove_recursive(nkd_rm_debugfs_dir);
    nkd_rm_debugfs_dir = NULL;
err_free_scratch:
    for_each_possible_cpu(cpu) {
        u8 *buf = *per_cpu_ptr(&nkd_rm_scratch_buf, cpu);
        kfree(buf);
        *per_cpu_ptr(&nkd_rm_scratch_buf, cpu) = NULL;
    }
    return -ENOMEM;
}

void nkd_rm_cleanup(void)
{
    int cpu;

    WRITE_ONCE(nkd_rm_enabled, 0);

    if (nkd_rm_rchan) {
        relay_close(nkd_rm_rchan);
        nkd_rm_rchan = NULL;
    }

    if (nkd_rm_debugfs_dir) {
        debugfs_remove_recursive(nkd_rm_debugfs_dir);
        nkd_rm_debugfs_dir = NULL;
    }

    for_each_possible_cpu(cpu) {
        u8 *buf = *per_cpu_ptr(&nkd_rm_scratch_buf, cpu);
        kfree(buf);
        *per_cpu_ptr(&nkd_rm_scratch_buf, cpu) = NULL;
    }

    pr_info("nkd_rm: cleaned up\n");
}

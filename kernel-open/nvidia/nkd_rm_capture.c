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
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/workqueue.h>

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
#define NKD_RM_FLUSH_DELAY_MS 20

/* Global enable flag */
int nkd_rm_enabled;

/* Statistics */
static atomic64_t nkd_rm_total_pushes = ATOMIC64_INIT(0);
static atomic64_t nkd_rm_total_bytes  = ATOMIC64_INIT(0);
static atomic64_t nkd_rm_dropped      = ATOMIC64_INIT(0);
static atomic64_t nkd_rm_flushes      = ATOMIC64_INIT(0);
static atomic64_t nkd_rm_resets       = ATOMIC64_INIT(0);
static atomic_t nkd_rm_flush_queued   = ATOMIC_INIT(0);
static atomic_t nkd_rm_flush_dirty    = ATOMIC_INIT(0);

/* Relay and debugfs */
static struct rchan  *nkd_rm_rchan;
static struct dentry *nkd_rm_debugfs_dir;
static struct dentry *nkd_rm_flush_file;
static struct dentry *nkd_rm_reset_file;
static struct delayed_work nkd_rm_flush_work;

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

static void nkd_rm_flush_worker(struct work_struct *work)
{
    atomic_set(&nkd_rm_flush_queued, 0);
    if (!atomic_xchg(&nkd_rm_flush_dirty, 0))
        return;

    if (!nkd_rm_rchan)
        return;

    relay_flush(nkd_rm_rchan);
    atomic64_inc(&nkd_rm_flushes);

    if (atomic_read(&nkd_rm_flush_dirty) &&
        atomic_xchg(&nkd_rm_flush_queued, 1) == 0) {
        queue_delayed_work(system_unbound_wq, &nkd_rm_flush_work,
                           msecs_to_jiffies(NKD_RM_FLUSH_DELAY_MS));
    }
}

static void nkd_rm_schedule_flush(void)
{
    atomic_set(&nkd_rm_flush_dirty, 1);
    if (atomic_xchg(&nkd_rm_flush_queued, 1) == 0) {
        queue_delayed_work(system_unbound_wq, &nkd_rm_flush_work,
                           msecs_to_jiffies(NKD_RM_FLUSH_DELAY_MS));
    }
}

static void nkd_rm_flush_now(void)
{
    if (!nkd_rm_rchan)
        return;

    relay_flush(nkd_rm_rchan);
    atomic_set(&nkd_rm_flush_dirty, 0);
    atomic64_inc(&nkd_rm_flushes);
}

static void nkd_rm_reset_now(void)
{
    if (!nkd_rm_rchan)
        return;

    cancel_delayed_work_sync(&nkd_rm_flush_work);
    atomic_set(&nkd_rm_flush_queued, 0);
    atomic_set(&nkd_rm_flush_dirty, 0);
    relay_reset(nkd_rm_rchan);
    atomic64_inc(&nkd_rm_resets);
}

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

static ssize_t nkd_rm_flush_write(struct file *file, const char __user *buf,
                                  size_t count, loff_t *ppos)
{
    int val;

    if (kstrtoint_from_user(buf, count, 10, &val))
        return -EINVAL;
    if (val)
        nkd_rm_flush_now();
    return count;
}

static ssize_t nkd_rm_reset_write(struct file *file, const char __user *buf,
                                  size_t count, loff_t *ppos)
{
    int val;

    if (kstrtoint_from_user(buf, count, 10, &val))
        return -EINVAL;
    if (val)
        nkd_rm_reset_now();
    return count;
}

static const struct file_operations nkd_rm_flush_fops = {
    .write = nkd_rm_flush_write,
};

static const struct file_operations nkd_rm_reset_fops = {
    .write = nkd_rm_reset_write,
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
                       "flushes:      %lld\n"
                       "resets:       %lld\n"
                       "enabled:      %d\n",
                       atomic64_read(&nkd_rm_total_pushes),
                       atomic64_read(&nkd_rm_total_bytes),
                       atomic64_read(&nkd_rm_dropped),
                       atomic64_read(&nkd_rm_flushes),
                       atomic64_read(&nkd_rm_resets),
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

    if (likely(nkd_rm_rchan)) {
        relay_write(nkd_rm_rchan, scratch, record_size);
        nkd_rm_schedule_flush();
    }

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

    INIT_DELAYED_WORK(&nkd_rm_flush_work, nkd_rm_flush_worker);

    /* Create control files */
    debugfs_create_file("enabled", 0644, nkd_rm_debugfs_dir, NULL,
                        &nkd_rm_enabled_fops);
    debugfs_create_file("stats", 0444, nkd_rm_debugfs_dir, NULL,
                        &nkd_rm_stats_fops);
    nkd_rm_flush_file = debugfs_create_file("flush", 0200, nkd_rm_debugfs_dir,
                                            NULL, &nkd_rm_flush_fops);
    nkd_rm_reset_file = debugfs_create_file("reset", 0200, nkd_rm_debugfs_dir,
                                            NULL, &nkd_rm_reset_fops);

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
    cancel_delayed_work_sync(&nkd_rm_flush_work);
    atomic_set(&nkd_rm_flush_queued, 0);
    atomic_set(&nkd_rm_flush_dirty, 0);
    nkd_rm_flush_file = NULL;
    nkd_rm_reset_file = NULL;

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

/*
 * NVIDIA Kernel Detective - Hook Implementation
 *
 * Captures pushbuffer data at uvm_channel_end_push() and writes
 * structured records to the relay channel. Provides debugfs controls
 * for enable/disable and statistics.
 */

#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/atomic.h>

#include "nkd_hook.h"
#include "nkd_relay.h"
#include "uvm_channel.h"
#include "uvm_gpu.h"
#include "uvm_processors.h"

/* Global enable flag — checked with READ_ONCE() in hot path */
int nkd_enabled;

/* Statistics */
static atomic64_t nkd_total_pushes  = ATOMIC64_INIT(0);
static atomic64_t nkd_total_bytes   = ATOMIC64_INIT(0);
static atomic64_t nkd_dropped       = ATOMIC64_INIT(0);

/* Debugfs entries */
extern struct dentry *nkd_debugfs_dir;  /* from nkd_relay.c */
static struct dentry *nkd_enabled_file;
static struct dentry *nkd_stats_file;

/*
 * Scratch buffer for assembling records before relay_write().
 * Max UVM push is UVM_MAX_PUSH_SIZE (currently ~16KB).
 * Header + max push ≈ 24 + 16384 = ~16.4KB.
 * We use a per-CPU buffer to avoid allocations in the hot path.
 */
#define NKD_SCRATCH_SIZE  (NKD_RECORD_HDR_SIZE + UVM_MAX_PUSH_SIZE)
static DEFINE_PER_CPU(u8 *, nkd_scratch_buf);

/* ---- Debugfs: enabled ---- */

static ssize_t nkd_enabled_read(struct file *file, char __user *buf,
                                size_t count, loff_t *ppos)
{
    char tmp[4];
    int len;

    len = snprintf(tmp, sizeof(tmp), "%d\n", READ_ONCE(nkd_enabled));
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t nkd_enabled_write(struct file *file, const char __user *buf,
                                 size_t count, loff_t *ppos)
{
    int val;
    int ret;

    ret = kstrtoint_from_user(buf, count, 10, &val);
    if (ret)
        return ret;

    WRITE_ONCE(nkd_enabled, !!val);
    pr_info("nkd: capture %s\n", nkd_enabled ? "enabled" : "disabled");
    return count;
}

static const struct file_operations nkd_enabled_fops = {
    .read  = nkd_enabled_read,
    .write = nkd_enabled_write,
};

/* ---- Debugfs: stats ---- */

static ssize_t nkd_stats_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    char tmp[256];
    int len;

    len = snprintf(tmp, sizeof(tmp),
                   "total_pushes: %lld\n"
                   "total_bytes:  %lld\n"
                   "dropped:      %lld\n"
                   "enabled:      %d\n",
                   atomic64_read(&nkd_total_pushes),
                   atomic64_read(&nkd_total_bytes),
                   atomic64_read(&nkd_dropped),
                   READ_ONCE(nkd_enabled));
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static const struct file_operations nkd_stats_fops = {
    .read = nkd_stats_read,
};

/* ---- Push capture ---- */

void nkd_capture_push(uvm_push_t *push)
{
    struct nkd_push_record *rec;
    uvm_channel_t *channel = push->channel;
    uvm_gpu_t *gpu = push->gpu;
    NvU32 push_size;
    size_t record_size;
    u8 *scratch;

    push_size = (NvU32)((push->next - push->begin) * sizeof(NvU32));
    if (push_size == 0 || push_size > UVM_MAX_PUSH_SIZE) {
        atomic64_inc(&nkd_dropped);
        return;
    }

    record_size = NKD_RECORD_HDR_SIZE + push_size;

    scratch = get_cpu_var(nkd_scratch_buf);
    if (unlikely(!scratch)) {
        put_cpu_var(nkd_scratch_buf);
        atomic64_inc(&nkd_dropped);
        return;
    }

    rec = (struct nkd_push_record *)scratch;
    rec->timestamp_ns = ktime_get_ns();
    rec->gpu_id       = uvm_id_value(gpu->id);
    rec->channel_id   = channel->channel_info.hwChannelId;
    rec->push_size    = push_size;
    rec->reserved     = 0;
    memcpy(rec->pushbuffer_data, push->begin, push_size);

    nkd_relay_write(scratch, record_size);

    put_cpu_var(nkd_scratch_buf);

    atomic64_inc(&nkd_total_pushes);
    atomic64_add(push_size, &nkd_total_bytes);
}

/* ---- Init / Cleanup ---- */

void nkd_init(void)
{
    int cpu;
    bool alloc_failed = false;

    /* Allocate per-CPU scratch buffers */
    for_each_possible_cpu(cpu) {
        u8 *buf = kmalloc(NKD_SCRATCH_SIZE, GFP_KERNEL);
        if (!buf) {
            alloc_failed = true;
            break;
        }
        *per_cpu_ptr(&nkd_scratch_buf, cpu) = buf;
    }

    if (alloc_failed) {
        pr_err("nkd: failed to allocate per-CPU scratch buffers\n");
        for_each_possible_cpu(cpu) {
            u8 *buf = *per_cpu_ptr(&nkd_scratch_buf, cpu);
            kfree(buf);
            *per_cpu_ptr(&nkd_scratch_buf, cpu) = NULL;
        }
        return;
    }

    if (nkd_relay_init() != 0)
        return;

    nkd_enabled_file = debugfs_create_file("enabled", 0644,
                                           nkd_debugfs_dir, NULL,
                                           &nkd_enabled_fops);
    nkd_stats_file = debugfs_create_file("stats", 0444,
                                         nkd_debugfs_dir, NULL,
                                         &nkd_stats_fops);

    WRITE_ONCE(nkd_enabled, 0);

    pr_info("nkd: initialized (capture disabled)\n");
}

void nkd_cleanup(void)
{
    int cpu;

    WRITE_ONCE(nkd_enabled, 0);

    nkd_relay_cleanup();

    for_each_possible_cpu(cpu) {
        u8 *buf = *per_cpu_ptr(&nkd_scratch_buf, cpu);
        kfree(buf);
        *per_cpu_ptr(&nkd_scratch_buf, cpu) = NULL;
    }

    pr_info("nkd: cleaned up\n");
}

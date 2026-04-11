/*
 * NVIDIA Kernel Detective - Relay Channel
 *
 * Creates a relay channel under /sys/kernel/debug/nkd/pushbuffer
 * for streaming push records to userspace.
 */

#include <linux/relay.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "nkd_relay.h"

/* 256 subbuffers × 64KB = 16MB total per CPU */
#define NKD_SUBBUF_SIZE     (64 * 1024)
#define NKD_N_SUBBUFS       256
#define NKD_FLUSH_DELAY_MS  20

static struct rchan *nkd_rchan;
struct dentry *nkd_debugfs_dir;     /* shared with nkd_hook.c */
static atomic64_t nkd_relay_flushes = ATOMIC64_INIT(0);
static atomic64_t nkd_relay_resets = ATOMIC64_INIT(0);
static atomic_t nkd_flush_queued = ATOMIC_INIT(0);
static atomic_t nkd_flush_dirty = ATOMIC_INIT(0);
static struct dentry *nkd_flush_file;
static struct dentry *nkd_reset_file;

static void nkd_relay_flush_worker(struct work_struct *work);
static DECLARE_DELAYED_WORK(nkd_flush_work, nkd_relay_flush_worker);

/* relay callbacks */
static struct dentry *nkd_create_buf_file(const char *filename,
                                          struct dentry *parent,
                                          umode_t mode,
                                          struct rchan_buf *buf,
                                          int *is_global)
{
    *is_global = 1;     /* single global buffer, simpler for userspace */
    return debugfs_create_file(filename, mode, parent, buf,
                               &relay_file_operations);
}

static int nkd_remove_buf_file(struct dentry *dentry)
{
    debugfs_remove(dentry);
    return 0;
}

static const struct rchan_callbacks nkd_relay_cbs = {
    .create_buf_file = nkd_create_buf_file,
    .remove_buf_file = nkd_remove_buf_file,
};

static void nkd_relay_schedule_flush(void)
{
    atomic_set(&nkd_flush_dirty, 1);
    if (atomic_xchg(&nkd_flush_queued, 1) == 0) {
        queue_delayed_work(system_unbound_wq, &nkd_flush_work,
                           msecs_to_jiffies(NKD_FLUSH_DELAY_MS));
    }
}

void nkd_relay_flush_now(void)
{
    if (!nkd_rchan)
        return;

    relay_flush(nkd_rchan);
    atomic_set(&nkd_flush_dirty, 0);
    atomic64_inc(&nkd_relay_flushes);
}

void nkd_relay_reset_now(void)
{
    if (!nkd_rchan)
        return;

    cancel_delayed_work_sync(&nkd_flush_work);
    atomic_set(&nkd_flush_queued, 0);
    atomic_set(&nkd_flush_dirty, 0);
    relay_reset(nkd_rchan);
    atomic64_inc(&nkd_relay_resets);
}

u64 nkd_relay_flush_count(void)
{
    return (u64)atomic64_read(&nkd_relay_flushes);
}

u64 nkd_relay_reset_count(void)
{
    return (u64)atomic64_read(&nkd_relay_resets);
}

static void nkd_relay_flush_worker(struct work_struct *work)
{
    atomic_set(&nkd_flush_queued, 0);
    if (!atomic_xchg(&nkd_flush_dirty, 0))
        return;

    nkd_relay_flush_now();

    if (atomic_read(&nkd_flush_dirty) &&
        atomic_xchg(&nkd_flush_queued, 1) == 0) {
        queue_delayed_work(system_unbound_wq, &nkd_flush_work,
                           msecs_to_jiffies(NKD_FLUSH_DELAY_MS));
    }
}

static ssize_t nkd_flush_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    int val;

    if (kstrtoint_from_user(buf, count, 10, &val))
        return -EINVAL;
    if (val)
        nkd_relay_flush_now();
    return count;
}

static ssize_t nkd_reset_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    int val;

    if (kstrtoint_from_user(buf, count, 10, &val))
        return -EINVAL;
    if (val)
        nkd_relay_reset_now();
    return count;
}

static const struct file_operations nkd_flush_fops = {
    .write = nkd_flush_write,
};

static const struct file_operations nkd_reset_fops = {
    .write = nkd_reset_write,
};

int nkd_relay_init(void)
{
    nkd_debugfs_dir = debugfs_create_dir("nkd", NULL);
    if (IS_ERR_OR_NULL(nkd_debugfs_dir)) {
        pr_err("nkd: failed to create debugfs dir\n");
        return -ENOMEM;
    }

    nkd_rchan = relay_open("pushbuffer", nkd_debugfs_dir,
                           NKD_SUBBUF_SIZE, NKD_N_SUBBUFS,
                           &nkd_relay_cbs, NULL);
    if (!nkd_rchan) {
        pr_err("nkd: failed to open relay channel\n");
        debugfs_remove_recursive(nkd_debugfs_dir);
        nkd_debugfs_dir = NULL;
        return -ENOMEM;
    }

    nkd_flush_file = debugfs_create_file("flush", 0200, nkd_debugfs_dir, NULL,
                                         &nkd_flush_fops);
    nkd_reset_file = debugfs_create_file("reset", 0200, nkd_debugfs_dir, NULL,
                                         &nkd_reset_fops);

    pr_info("nkd: relay channel created (%d × %dKB)\n",
            NKD_N_SUBBUFS, NKD_SUBBUF_SIZE / 1024);
    return 0;
}

void nkd_relay_cleanup(void)
{
    cancel_delayed_work_sync(&nkd_flush_work);
    atomic_set(&nkd_flush_queued, 0);
    atomic_set(&nkd_flush_dirty, 0);
    nkd_flush_file = NULL;
    nkd_reset_file = NULL;

    if (nkd_rchan) {
        relay_close(nkd_rchan);
        nkd_rchan = NULL;
    }
    if (nkd_debugfs_dir) {
        debugfs_remove_recursive(nkd_debugfs_dir);
        nkd_debugfs_dir = NULL;
    }
}

void nkd_relay_write(const void *data, size_t len)
{
    if (likely(nkd_rchan)) {
        relay_write(nkd_rchan, data, len);
        nkd_relay_schedule_flush();
    }
}

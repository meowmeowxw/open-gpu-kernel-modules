/*
 * NVIDIA Kernel Detective - Relay Channel
 *
 * Creates a relay channel under /sys/kernel/debug/nkd/pushbuffer
 * for streaming push records to userspace.
 */

#include <linux/relay.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "nkd_relay.h"

/* 256 subbuffers × 64KB = 16MB total per CPU */
#define NKD_SUBBUF_SIZE     (64 * 1024)
#define NKD_N_SUBBUFS       256

static struct rchan *nkd_rchan;
struct dentry *nkd_debugfs_dir;     /* shared with nkd_hook.c */

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

    pr_info("nkd: relay channel created (%d × %dKB)\n",
            NKD_N_SUBBUFS, NKD_SUBBUF_SIZE / 1024);
    return 0;
}

void nkd_relay_cleanup(void)
{
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
    if (likely(nkd_rchan))
        relay_write(nkd_rchan, data, len);
}

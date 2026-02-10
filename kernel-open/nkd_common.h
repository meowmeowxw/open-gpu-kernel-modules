/*
 * NVIDIA Kernel Detective - Common Definitions
 *
 * Shared header for both UVM and RM capture paths.
 * Defines the relay record structure and helper functions.
 */

#ifndef __NKD_COMMON_H__
#define __NKD_COMMON_H__

#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>

/* Source identifiers */
#define NKD_SOURCE_UVM  0
#define NKD_SOURCE_RM   1

/*
 * Per-push record written to relay channels (v2 format, 48-byte header).
 * Used by both UVM (/sys/kernel/debug/nkd/) and RM (/sys/kernel/debug/nkd_rm/).
 */
struct nkd_push_record {
    u64  timestamp_ns;       /* ktime_get_ns() when captured */
    u32  gpu_id;             /* GPU identifier */
    u32  channel_id;         /* Hardware channel ID */
    u32  push_size;          /* Pushbuffer data size in bytes */
    u32  pid;                /* Triggering process PID */
    char comm[16];           /* Triggering process name (TASK_COMM_LEN) */
    u8   source;             /* NKD_SOURCE_UVM or NKD_SOURCE_RM */
    u8   padding[3];         /* Alignment padding */
    u32  class_id;           /* GPU engine class (e.g. 0xC8B5, 0xC9B5) */
    u8   pushbuffer_data[];  /* Variable-length push data */
} __attribute__((packed));

#define NKD_RECORD_HDR_SIZE  (sizeof(struct nkd_push_record))  /* 48 bytes */

/*
 * Helper: Fill common record header fields.
 * Call this before copying pushbuffer_data to ensure consistent formatting.
 *
 * @rec: Pointer to record header
 * @gpu_id: GPU identifier
 * @channel_id: Hardware channel ID
 * @push_size: Size of pushbuffer data in bytes
 * @source: NKD_SOURCE_UVM or NKD_SOURCE_RM
 * @class_id: GPU engine class (0 if determined by subchannel)
 */
static inline void nkd_fill_record_header(
    struct nkd_push_record *rec,
    u32 gpu_id,
    u32 channel_id,
    u32 push_size,
    u8 source,
    u32 class_id)
{
    rec->timestamp_ns = ktime_get_ns();
    rec->gpu_id       = gpu_id;
    rec->channel_id   = channel_id;
    rec->push_size    = push_size;
    rec->pid          = current->pid;
    memcpy(rec->comm, current->comm, sizeof(rec->comm));
    rec->source       = source;
    rec->padding[0]   = 0;
    rec->padding[1]   = 0;
    rec->padding[2]   = 0;
    rec->class_id     = class_id;
}

#endif /* __NKD_COMMON_H__ */

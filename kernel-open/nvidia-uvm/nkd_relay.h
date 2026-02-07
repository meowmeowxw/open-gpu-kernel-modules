/*
 * NVIDIA Kernel Detective - Relay Channel Interface
 *
 * Manages a Linux relay channel under debugfs for streaming pushbuffer
 * records from kernel to userspace.
 */

#ifndef __NKD_RELAY_H__
#define __NKD_RELAY_H__

#include "nvtypes.h"

/* Per-push record written to the relay channel */
struct nkd_push_record {
    NvU64 timestamp_ns;
    NvU32 gpu_id;
    NvU32 channel_id;
    NvU32 push_size;        /* in bytes */
    NvU32 reserved;
    NvU8  pushbuffer_data[];
};

#define NKD_RECORD_HDR_SIZE  (sizeof(struct nkd_push_record))

int  nkd_relay_init(void);
void nkd_relay_cleanup(void);
void nkd_relay_write(const void *data, size_t len);

#endif /* __NKD_RELAY_H__ */

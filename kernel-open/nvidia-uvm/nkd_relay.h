/*
 * NVIDIA Kernel Detective - Relay Channel Interface
 *
 * Manages a Linux relay channel under debugfs for streaming pushbuffer
 * records from kernel to userspace.
 */

#ifndef __NKD_RELAY_H__
#define __NKD_RELAY_H__

#include "../nkd_common.h"

int  nkd_relay_init(void);
void nkd_relay_cleanup(void);
void nkd_relay_write(const void *data, size_t len);
void nkd_relay_flush_now(void);
void nkd_relay_reset_now(void);
u64  nkd_relay_flush_count(void);
u64  nkd_relay_reset_count(void);

#endif /* __NKD_RELAY_H__ */

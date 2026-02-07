/*
 * NVIDIA Kernel Detective - Hook Interface
 *
 * Called from uvm_channel_end_push() to capture pushbuffer data.
 */

#ifndef __NKD_HOOK_H__
#define __NKD_HOOK_H__

#include "uvm_push.h"

extern int nkd_enabled;

void nkd_init(void);
void nkd_cleanup(void);
void nkd_capture_push(uvm_push_t *push);

#endif /* __NKD_HOOK_H__ */

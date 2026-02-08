/*
 * NVIDIA Kernel Detective - RM Capture Interface
 *
 * Captures RM internal CE pushbuffer submissions in nvidia.ko
 * and streams them via a separate relay channel under
 * /sys/kernel/debug/nkd_rm/
 */

#ifndef __NKD_RM_CAPTURE_H__
#define __NKD_RM_CAPTURE_H__

#include <linux/types.h>

/* Global enable flag — checked with READ_ONCE() in channelFillGpFifo() */
extern int nkd_rm_enabled;

int  nkd_rm_init(void);
void nkd_rm_cleanup(void);

/*
 * Capture an RM pushbuffer submission.
 *
 * @methods:      Pointer to raw method words in the pushbuffer
 * @size:         Size of method data in bytes
 * @gpu_id:       GPU instance index
 * @channel_id:   RM channel handle
 * @class_id:     GPU engine class (e.g. 0xC8B5, 0xC9B5)
 */
void nkd_rm_capture_push(const void *methods, u32 size,
                          u32 gpu_id, u32 channel_id, u32 class_id);

#endif /* __NKD_RM_CAPTURE_H__ */

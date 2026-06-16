/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 *
 * Internal control layer (not installed). Public symbols: rkuvc.h
 */

#ifndef __UVC_CONTROL_H__
#define __UVC_CONTROL_H__

#include "rkuvc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USE_RK_MODULE
#define ISP_SEQ 1
#define ISP_FMT HAL_FRMAE_FMT_NV12
#define CIF_SEQ 0
#define CIF_FMT HAL_FRMAE_FMT_SBGGR10
#else
#define ISP_SEQ 0
#define ISP_FMT HAL_FRMAE_FMT_SBGGR8
#define CIF_SEQ 1
#define CIF_FMT HAL_FRMAE_FMT_NV12
#endif

#define UVC_CONTROL_LOOP_ONCE (1 << 0)
#define UVC_CONTROL_CHECK_STRAIGHT (1 << 1)

int uvc_control_module_open(void);
void uvc_control_module_close(void);

void add_uvc_video(void);
int uvc_control_video_id(unsigned seq);
int check_uvc_video_id(void);
void uvc_control_init(int width, int height, int fcc, int fps);
void uvc_control_exit(void);
bool uvc_control_host_streaming(void);
int get_uvc_streaming_intf(void);
int uvc_control_run(uint32_t flags);
void uvc_control_join(uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif

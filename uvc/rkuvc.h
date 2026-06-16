/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 *
 * Public API for librkuvc.so — external applications should include only:
 *   #include <rkuvc/rkuvc.h>
 */

#ifndef RKUVC_H
#define RKUVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RKUVC_VERSION_MAJOR 1
#define RKUVC_VERSION_MINOR 0
#define RKUVC_VERSION_PATCH 0

/** Skip extcon monitor; probe UVC node once at uvc_app_run(). */
#define UVC_APP_FLAG_CHECK_STRAIGHT (1u << 0)

struct uvc_app;

/**
 * Register camera callbacks before uvc_app_create().
 * open: start capture when host begins UVC stream (STREAMON).
 * close: stop capture on stream stop / USB teardown.
 */
typedef int (*uvc_open_camera_callback)(void *userdata, int width, int height,
                                        int fcc, int fps);
typedef void (*uvc_close_camera_callback)(void *userdata);

void register_uvc_open_camera(uvc_open_camera_callback cb, void *userdata);
void register_uvc_close_camera(uvc_close_camera_callback cb, void *userdata);

/** Call from the camera thread to feed frames into the UVC pipeline. */
void uvc_read_camera_buffer(void *cam_buf, int cam_fd, size_t cam_size,
                            void *extra_data, size_t extra_size);

struct uvc_app *uvc_app_create(void);
void uvc_app_destroy(struct uvc_app *app);

/** flags: 0 = extcon hot-plug (default), or UVC_APP_FLAG_CHECK_STRAIGHT. */
int uvc_app_run(struct uvc_app *app, uint32_t flags);
void uvc_app_stop(struct uvc_app *app);

#ifdef __cplusplus
}
#endif

#endif /* RKUVC_H */

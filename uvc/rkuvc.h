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
typedef void (*uvc_release_buffer_callback)(void *userdata, int dmabuf_fd);
typedef void (*uvc_log_callback)(void *userdata, const char *message);

void register_uvc_open_camera(uvc_open_camera_callback cb, void *userdata);
void register_uvc_close_camera(uvc_close_camera_callback cb, void *userdata);
void register_uvc_release_buffer(uvc_release_buffer_callback cb, void *userdata);

/** Route librkuvc log messages to the application. Pass NULL to restore stdio. */
void register_uvc_log_callback(uvc_log_callback cb, void *userdata);

/**
 * Call from the camera thread to feed frames into the UVC pipeline.
 * When cam_fd is a valid DMA-BUF fd, the library duplicates the fd and queues
 * it directly to the UVC V4L2 output device. The caller must keep the fd and
 * underlying buffer valid and must not reuse either until the registered
 * release callback receives that fd. If DMA-BUF import is
 * unavailable, cam_buf is used by the MMAP copy fallback and must point to
 * cam_size readable bytes; the release callback is then invoked immediately
 * after the synchronous copy completes.
 */
void uvc_read_camera_buffer(void *cam_buf, int cam_fd, size_t cam_size,
                            void *extra_data, size_t extra_size);

struct uvc_app *uvc_app_create(void);
void uvc_app_destroy(struct uvc_app *app);

/** flags: 0 = extcon hot-plug (default), or UVC_APP_FLAG_CHECK_STRAIGHT. */
int uvc_app_run(struct uvc_app *app, uint32_t flags);
void uvc_app_stop(struct uvc_app *app);

/** Upper bounds for the arrays reported by uvc_get_sys_config(). */
#define UVC_SYS_MAX_RESOLUTIONS 16
#define UVC_SYS_MAX_FPS 8

/** Pixel formats selectable via uvc_set_config(). */
enum uvc_pixel_format {
    UVC_FMT_YUYV  = 0,
    UVC_FMT_MJPEG = 1,
    UVC_FMT_H264  = 2,
};

typedef struct uvc_resolution {
    int width;
    int height;
} uvc_resolution_t;

/**
 * Supported resolutions + frame rates the device advertises to the USB host
 * (the union of all formats declared in the UVC gadget descriptor).
 */
typedef struct uvc_config {
    uvc_resolution_t resolutions[UVC_SYS_MAX_RESOLUTIONS];
    int resolution_count;
    int fps[UVC_SYS_MAX_FPS];
    int fps_count;
} uvc_config_t;

/**
 * Select the fixed format + resolution + frame rate used by the application.
 * Call before uvc_app_run(); the (fmt, w, h, fps) combination must be
 * advertised by the gadget, otherwise the call is rejected. The host's UVC
 * Probe/Commit values are adapted to this setting and do not change it.
 *
 * @param h   height  (e.g. 720)
 * @param w   width   (e.g. 1280)
 * @param fmt pixel format, one of enum uvc_pixel_format (e.g. UVC_FMT_MJPEG)
 * @param fps frame rate in frames-per-second (e.g. 30)
 *
 * @return 0 on success, -1 if the combination is unsupported or the module is
 *         not initialized (call after register_uvc_open_camera() /
 *         uvc_app_create()).
 *         If never called, the built-in fixed default is MJPEG 1280x720 @ 30fps.
 */
int uvc_set_config(int h, int w, int fmt, int fps);

/** Query the resolutions + frame rates the system advertises to the host. */
uvc_config_t uvc_get_sys_config(void);

#ifdef __cplusplus
}
#endif

#endif /* RKUVC_H */

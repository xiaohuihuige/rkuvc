/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#ifndef UVC_GADGET_INTERNAL_H
#define UVC_GADGET_INTERNAL_H

#include "uvc-gadget.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <string.h>

#define UVC_GADGET_CLEAR(x) memset(&(x), 0, sizeof(x))
#define UVC_GADGET_MAX(a, b) ((a) > (b) ? (a) : (b))

struct v4l2_device {
    int v4l2_fd;
    int is_streaming;
    char *v4l2_devname;
    enum io_method io;
    struct buffer *mem;
    unsigned int nbufs;
    unsigned long long int qbuf_count;
    unsigned long long int dqbuf_count;
    struct uvc_device *udev;
};

struct uvc_gadget_config {
    int default_format;
    int default_resolution;
    int nbufs;
    int bulk_mode;
    int dummy_data_gen_mode;
    int mult;
    int burst;
    enum usb_device_speed speed;
    enum io_method uvc_io_method;
    const char *v4l2_devname;
    const char *mjpeg_image;
};

struct uvc_gadget_runtime {
    int video_id;
    char uvc_devname[32];
    struct uvc_device *udev;
    struct v4l2_device *vdev;
    struct uvc_gadget_config cfg;
};

int uvc_gadget_v4l2_open(struct v4l2_device **vdev, char *devname,
                         struct v4l2_format *fmt);
void uvc_gadget_v4l2_close(struct v4l2_device *dev);
int uvc_gadget_v4l2_reqbufs(struct v4l2_device *dev, int nbufs);
void uvc_gadget_v4l2_process_data(struct v4l2_device *dev);
void uvc_gadget_v4l2_stop_stream(struct v4l2_device *dev);

int uvc_gadget_uvc_open(struct uvc_device **udev, char *devname);
void uvc_gadget_uvc_close(struct uvc_device *dev);
void uvc_gadget_events_init(struct uvc_device *dev);
void uvc_gadget_events_process(struct uvc_device *dev);
void uvc_gadget_video_process(struct uvc_device *dev);
int uvc_gadget_uvc_stop_stream(struct uvc_device *dev);
int uvc_gadget_uvc_release_buffers(struct uvc_device *dev);

/** STREAMOFF + REQBUFS(0) on /dev/videoN (recover from zombie gadget). */
int uvc_gadget_force_uvc_node_idle(int video_id);

void uvc_gadget_apply_config(struct uvc_gadget_runtime *rt);

int uvc_gadget_session_loop(struct uvc_gadget_runtime *rt);
int uvc_gadget_session_teardown(struct uvc_gadget_runtime *rt);

#endif

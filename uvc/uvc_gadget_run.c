/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uvc-gadget.h"
#include "uvc_gadget_internal.h"
#include "uvc_epoll.h"
#include "uvc_video.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

struct gadget_loop_ctx {
    struct uvc_gadget_runtime *rt;
    struct uvc_epoll *ep;
    int stop_pipe[2];
};

static void gadget_stop_wake_handler(int fd, uint32_t revents, void *userdata)
{
    char drain[16];

    (void)userdata;
    if (!uvc_epoll_readable(revents))
        return;
    while (read(fd, drain, sizeof(drain)) > 0)
        ;
}

static void gadget_default_config(struct uvc_gadget_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->default_format = 1;
    cfg->default_resolution = 1;
    cfg->nbufs = 4;
    cfg->dummy_data_gen_mode = 1;
    cfg->speed = USB_SPEED_SUPER;
    cfg->uvc_io_method = IO_METHOD_MMAP;
    cfg->v4l2_devname = "/dev/video1";
}

static void gadget_set_maxpkt(struct uvc_device *udev,
                              const struct uvc_gadget_config *cfg)
{
    switch (cfg->speed) {
    case USB_SPEED_FULL:
        udev->maxpkt = cfg->bulk_mode ? 64 : 1023;
        break;
    case USB_SPEED_HIGH:
        udev->maxpkt = cfg->bulk_mode ? 512 : 1024;
        break;
    case USB_SPEED_SUPER:
    default:
        udev->maxpkt = 1024;
        break;
    }
}

static unsigned int gadget_pick_fcc(const struct uvc_gadget_config *cfg)
{
    switch (cfg->default_format) {
    case 2:
        return V4L2_PIX_FMT_H264;
    case 0:
        return V4L2_PIX_FMT_YUYV;
    case 1:
    default:
        return V4L2_PIX_FMT_MJPEG;
    }
}

void uvc_gadget_apply_config(struct uvc_gadget_runtime *rt)
{
    struct uvc_device *udev = rt->udev;
    const struct uvc_gadget_config *cfg = &rt->cfg;
    unsigned w = cfg->default_resolution ? 1280 : 640;
    unsigned h = cfg->default_resolution ? 720 : 360;

    udev->width = w;
    udev->height = h;
    udev->imgsize = w * h * 2;
    udev->fcc = gadget_pick_fcc(cfg);
    uvc_set_user_fcc(udev->fcc, udev->video_id);
    udev->io = cfg->uvc_io_method;
    udev->bulk = cfg->bulk_mode;
    udev->nbufs = cfg->nbufs;
    udev->mult = cfg->mult;
    udev->burst = cfg->burst;
    udev->speed = cfg->speed;
    udev->control = 0;

    if (cfg->dummy_data_gen_mode || cfg->mjpeg_image)
        udev->run_standalone = 1;

    gadget_set_maxpkt(udev, cfg);
}

static bool gadget_use_v4l2(const struct uvc_gadget_runtime *rt)
{
    return !rt->cfg.dummy_data_gen_mode && !rt->cfg.mjpeg_image;
}

static int gadget_open_v4l2(struct uvc_gadget_runtime *rt)
{
    struct v4l2_format fmt;

    if (!gadget_use_v4l2(rt))
        return 0;

    UVC_GADGET_CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = rt->cfg.default_resolution ? 1280 : 640;
    fmt.fmt.pix.height = rt->cfg.default_resolution ? 720 : 360;
    fmt.fmt.pix.pixelformat = gadget_pick_fcc(&rt->cfg);
    fmt.fmt.pix.sizeimage = fmt.fmt.pix.width * fmt.fmt.pix.height * 2;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (uvc_gadget_v4l2_open(&rt->vdev, (char *)rt->cfg.v4l2_devname, &fmt) < 0)
        return -1;

    rt->vdev->nbufs = rt->cfg.nbufs;
    rt->vdev->io = (rt->cfg.uvc_io_method == IO_METHOD_MMAP) ?
                   IO_METHOD_USERPTR : IO_METHOD_MMAP;
    rt->udev->vdev = rt->vdev;
    rt->vdev->udev = rt->udev;
    rt->vdev->v4l2_devname = (char *)rt->cfg.v4l2_devname;

    if (rt->vdev->io == IO_METHOD_MMAP)
        uvc_gadget_v4l2_reqbufs(rt->vdev, rt->vdev->nbufs);

    return 0;
}

static int gadget_open_uvc(struct uvc_gadget_runtime *rt)
{
    snprintf(rt->uvc_devname, sizeof(rt->uvc_devname),
             "/dev/video%d", rt->video_id);

    if (uvc_gadget_uvc_open(&rt->udev, rt->uvc_devname) < 0)
        return -1;

    rt->udev->uvc_devname = rt->uvc_devname;
    rt->udev->video_id = rt->video_id;
    uvc_video_set_gadget_fd(rt->video_id, rt->udev->uvc_fd);
    uvc_gadget_apply_config(rt);
    return 0;
}

static void gadget_uvc_epoll_handler(int fd, uint32_t revents, void *userdata)
{
    struct gadget_loop_ctx *ctx = userdata;
    struct uvc_gadget_runtime *rt = ctx->rt;

    (void)fd;
    if (!uvc_get_user_run_state(rt->video_id))
        return;
    if (uvc_epoll_priority(revents))
        uvc_gadget_events_process(rt->udev);
    if (!uvc_get_user_run_state(rt->video_id))
        return;
    if (uvc_epoll_writable(revents))
        uvc_gadget_video_process(rt->udev);
}

static void gadget_v4l2_epoll_handler(int fd, uint32_t revents, void *userdata)
{
    struct gadget_loop_ctx *ctx = userdata;

    (void)fd;
    if (uvc_epoll_readable(revents))
        uvc_gadget_v4l2_process_data(ctx->rt->vdev);
}

static int gadget_loop_should_stop(void *ctx)
{
    struct gadget_loop_ctx *loop = ctx;

    return !uvc_get_user_run_state(loop->rt->video_id);
}

static void gadget_stop_pipe_close(struct gadget_loop_ctx *loop, int video_id)
{
    uvc_video_bind_stop_wake(video_id, -1);
    if (loop->stop_pipe[1] >= 0) {
        close(loop->stop_pipe[1]);
        loop->stop_pipe[1] = -1;
    }
    if (loop->stop_pipe[0] >= 0) {
        close(loop->stop_pipe[0]);
        loop->stop_pipe[0] = -1;
    }
}

static int gadget_stop_pipe_open(struct gadget_loop_ctx *loop)
{
    if (pipe(loop->stop_pipe) != 0)
        return -1;

    fcntl(loop->stop_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(loop->stop_pipe[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

static int gadget_run_epoll(struct uvc_gadget_runtime *rt)
{
    struct gadget_loop_ctx loop = { .rt = rt };
    int uvc_events = UVC_EPOLL_EVT_PRI | UVC_EPOLL_EVT_OUT;
    int ret;

    loop.stop_pipe[0] = loop.stop_pipe[1] = -1;

    loop.ep = uvc_epoll_create(8);
    if (!loop.ep)
        return -1;

    if (gadget_stop_pipe_open(&loop) != 0) {
        ret = -1;
        goto out;
    }

    uvc_video_bind_stop_wake(rt->video_id, loop.stop_pipe[1]);

    if (uvc_epoll_add(loop.ep, loop.stop_pipe[0], UVC_EPOLL_EVT_IN,
                      gadget_stop_wake_handler, &loop) != 0) {
        ret = -1;
        goto out_pipe;
    }

    if (uvc_epoll_add(loop.ep, rt->udev->uvc_fd, uvc_events,
                      gadget_uvc_epoll_handler, &loop) != 0) {
        ret = -1;
        goto out_pipe;
    }

    if (gadget_use_v4l2(rt) &&
        uvc_epoll_add(loop.ep, rt->vdev->v4l2_fd, UVC_EPOLL_EVT_IN,
                      gadget_v4l2_epoll_handler, &loop) != 0) {
        ret = -1;
        goto out_pipe;
    }

    ret = uvc_epoll_loop(loop.ep, NULL, NULL, gadget_loop_should_stop, &loop);

out_pipe:
    gadget_stop_pipe_close(&loop, rt->video_id);
out:
    uvc_epoll_destroy(loop.ep);
    return ret < 0 ? ret : 0;
}

int uvc_gadget_session_loop(struct uvc_gadget_runtime *rt)
{
    uvc_gadget_events_init(rt->udev);
    uvc_set_user_run_state(true, rt->video_id);
    return gadget_run_epoll(rt);
}

int uvc_gadget_session_teardown(struct uvc_gadget_runtime *rt)
{
    if (gadget_use_v4l2(rt))
        uvc_gadget_v4l2_stop_stream(rt->vdev);

    uvc_gadget_uvc_stop_stream(rt->udev);

    if (gadget_use_v4l2(rt))
        uvc_gadget_v4l2_close(rt->vdev);

    uvc_video_set_gadget_fd(rt->video_id, -1);
    uvc_gadget_uvc_close(rt->udev);
    uvc_buffer_deinit(rt->video_id);
    rt->udev = NULL;
    rt->vdev = NULL;
    return 0;
}

static int gadget_session_start(struct uvc_gadget_runtime *rt)
{
    if (gadget_open_uvc(rt) != 0)
        return -1;

    if (gadget_open_v4l2(rt) != 0) {
        uvc_gadget_uvc_close(rt->udev);
        rt->udev = NULL;
        return -1;
    }

    return uvc_gadget_session_loop(rt);
}

int uvc_gadget_run(int video_id)
{
    struct uvc_gadget_runtime rt;

    memset(&rt, 0, sizeof(rt));
    rt.video_id = video_id;
    gadget_default_config(&rt.cfg);

    if (gadget_session_start(&rt) != 0)
        return -1;

    return uvc_gadget_session_teardown(&rt);
}

int uvc_gadget_main(int video_id)
{
    return uvc_gadget_run(video_id);
}

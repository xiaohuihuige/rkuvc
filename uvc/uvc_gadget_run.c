/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uvc-gadget.h"
#include "uvc_gadget_internal.h"
#include "uvc_epoll.h"
#include "uvc_video.h"
#include "uvc_control.h"
#include "uvc_log.h"

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
    cfg->uvc_io_method = IO_METHOD_DMABUF;
    cfg->v4l2_devname = "/dev/video45";
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
    int uw = 0, uh = 0, ufcc = 0, ufps = 0;
    unsigned w, h;

    /* Use the application-owned fixed format selected by uvc_set_config()
     * (or the built-in fixed default initialized by the control module). */
    bool app_configured = uvc_control_get_default_config(&uw, &uh, &ufcc, &ufps);
    if (app_configured) {
        w = (unsigned)uw;
        h = (unsigned)uh;
    } else {
        w = cfg->default_resolution ? 1280 : 640;
        h = cfg->default_resolution ? 720 : 360;
    }

    udev->width = w;
    udev->height = h;
    udev->imgsize = w * h * 2;
    udev->fcc = ufcc ? (unsigned int)ufcc : gadget_pick_fcc(cfg);
    if (ufps > 0)
        udev->fps = ufps;
    uvc_set_user_resolution((int)udev->width, (int)udev->height,
                            udev->video_id);
    uvc_set_user_fcc(udev->fcc, udev->video_id);
    udev->io = cfg->uvc_io_method;
    /* Application-owned frames arrive as virtual memory from the camera
     * callback. DMABUF mode cannot consume fd=-1 fallback frames, so use the
     * standalone MMAP path whenever set_config() is active. */
    if (app_configured) {
        udev->io = IO_METHOD_MMAP;
        udev->run_standalone = 1;
    }
    /* Static demo frames are copied into the library's output buffers.  MMAP
     * is required here because the DMABUF event loop does not consume that
     * fallback queue after a compressed-frame DMA-BUF is rejected. */
    if (cfg->dummy_data_gen_mode || cfg->mjpeg_image)
        udev->io = IO_METHOD_MMAP;
    udev->bulk = cfg->bulk_mode;
    udev->nbufs = cfg->nbufs;
    udev->mult = cfg->mult;
    udev->burst = cfg->burst;
    udev->speed = cfg->speed;
    udev->control = 0;

    if (cfg->dummy_data_gen_mode || cfg->mjpeg_image)
        udev->run_standalone = 1;

    gadget_set_maxpkt(udev, cfg);
    uvc_log_printf("uvc gadget: config video%d %ux%u fcc=0x%08x io=%d buffers=%u bulk=%d speed=%d maxpkt=%u standalone=%d\n",
                   udev->video_id, udev->width, udev->height, udev->fcc,
                   udev->io, udev->nbufs, udev->bulk, udev->speed,
                   udev->maxpkt, udev->run_standalone);
}

static bool gadget_use_v4l2(const struct uvc_gadget_runtime *rt)
{
    (void)rt;
    // Frames are supplied by the embedding application through
    // uvc_read_camera_buffer(); never open a camera capture node here.
    return false;
}

static int gadget_open_v4l2(struct uvc_gadget_runtime *rt)
{
    struct v4l2_format fmt;

    if (!gadget_use_v4l2(rt))
        return 0;

    UVC_GADGET_CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    /* The camera/input side must use the same fixed format as the UVC side. */
    fmt.fmt.pix.width = rt->udev->width;
    fmt.fmt.pix.height = rt->udev->height;
    fmt.fmt.pix.pixelformat = rt->udev->fcc;
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

    uvc_log_printf("uvc gadget: opening %s\n", rt->uvc_devname);

    if (uvc_gadget_uvc_open(&rt->udev, rt->uvc_devname) < 0)
        return -1;

    rt->udev->uvc_devname = rt->uvc_devname;
    rt->udev->video_id = rt->video_id;
    uvc_video_set_gadget_fd(rt->video_id, rt->udev->uvc_fd);
    uvc_video_bind_gadget(rt->video_id, rt->udev);
    uvc_gadget_apply_config(rt);
    uvc_log_printf("uvc gadget: opened %s fd=%d\n",
                   rt->uvc_devname, rt->udev->uvc_fd);
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
    int flags;

    if (pipe(loop->stop_pipe) != 0)
        return -1;

    fcntl(loop->stop_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(loop->stop_pipe[1], F_SETFD, FD_CLOEXEC);

    flags = fcntl(loop->stop_pipe[0], F_GETFL, 0);
    if (flags < 0 || fcntl(loop->stop_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0)
        goto err;
    flags = fcntl(loop->stop_pipe[1], F_GETFL, 0);
    if (flags < 0 || fcntl(loop->stop_pipe[1], F_SETFL, flags | O_NONBLOCK) < 0)
        goto err;
    return 0;

err:
    close(loop->stop_pipe[0]);
    close(loop->stop_pipe[1]);
    loop->stop_pipe[0] = loop->stop_pipe[1] = -1;
    return -1;
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

    uvc_log_printf("uvc gadget: event loop start video%d uvc_fd=%d\n",
                   rt->video_id, rt->udev->uvc_fd);
    ret = uvc_epoll_loop(loop.ep, NULL, NULL, gadget_loop_should_stop, &loop);
    uvc_log_printf("uvc gadget: event loop end video%d ret=%d\n", rt->video_id, ret);

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
    uvc_log_printf("uvc gadget: teardown begin video%d\n", rt->video_id);
    if (gadget_use_v4l2(rt))
        uvc_gadget_v4l2_stop_stream(rt->vdev);

    uvc_gadget_uvc_stop_stream(rt->udev);

    if (gadget_use_v4l2(rt))
        uvc_gadget_v4l2_close(rt->vdev);

    uvc_video_set_gadget_fd(rt->video_id, -1);
    uvc_video_bind_gadget(rt->video_id, NULL);
    uvc_gadget_uvc_close(rt->udev);
    uvc_buffer_deinit(rt->video_id);
    rt->udev = NULL;
    rt->vdev = NULL;
    uvc_log_printf("uvc gadget: teardown complete video%d\n", rt->video_id);
    return 0;
}

static int gadget_session_start(struct uvc_gadget_runtime *rt)
{
    if (gadget_open_uvc(rt) != 0)
        return -1;

    if (gadget_open_v4l2(rt) != 0) {
        uvc_video_set_gadget_fd(rt->video_id, -1);
        uvc_video_bind_gadget(rt->video_id, NULL);
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

    uvc_log_printf("uvc gadget: run begin video%d\n", video_id);

    if (gadget_session_start(&rt) != 0) {
        uvc_log_printf("uvc gadget: session start failed video%d\n", video_id);
        return -1;
    }

    return uvc_gadget_session_teardown(&rt);
}

int uvc_gadget_main(int video_id)
{
    return uvc_gadget_run(video_id);
}

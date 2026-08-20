/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL), available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include "uvc_log.h"
#include <string.h>
#include <errno.h>
#include <pthread.h>

//#include "process/video.h"
#include "uvc-gadget.h"
#include "uvc_gadget_internal.h"
#include "uvc_control.h"
#include "uvc_video.h"
//#include "uvc_iq_tool.h"

/* Enable debug prints. */
#undef ENABLE_BUFFER_DEBUG
#undef ENABLE_USB_REQUEST_DEBUG

#define CLEAR(x)    memset (&(x), 0, sizeof (x))
#define max(a, b)   (((a) > (b)) ? (a) : (b))

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define clamp(val, min, max) ({                 \
        typeof(val) __val = (val);              \
        typeof(min) __min = (min);              \
        typeof(max) __max = (max);              \
        (void) (&__val == &__min);              \
        (void) (&__val == &__max);              \
        __val = __val < __min ? __min: __val;   \
        __val > __max ? __max: __val; })

#define pixfmtstr(x)    (x) & 0xff, ((x) >> 8) & 0xff, ((x) >> 16) & 0xff, \
            ((x) >> 24) & 0xff

/*
 * The UVC webcam gadget kernel driver (g_webcam.ko) supports changing
 * the Brightness attribute of the Processing Unit (PU). by default. If
 * the underlying video capture device supports changing the Brightness
 * attribute of the image being acquired (like the Virtual Video, VIVI
 * driver), then we should route this UVC request to the respective
 * video capture device.
 *
 * Incase, there is no actual video capture device associated with the
 * UVC gadget and we wish to use this application as the final
 * destination of the UVC specific requests then we should return
 * pre-cooked (static) responses to GET_CUR(BRIGHTNESS) and
 * SET_CUR(BRIGHTNESS) commands to keep command verifier test tools like
 * UVC class specific test suite of USBCV, happy.
 *
 * Note that the values taken below are in sync with the VIVI driver and
 * must be changed for your specific video capture device. These values
 * also work well in case there in no actual video capture device.
 */
#define PU_BRIGHTNESS_MIN_VAL       0
#define PU_BRIGHTNESS_MAX_VAL       255
#define PU_BRIGHTNESS_STEP_SIZE     1
#define PU_BRIGHTNESS_DEFAULT_VAL   127

#define PU_CONTRAST_MIN_VAL         0
#define PU_CONTRAST_MAX_VAL         65535
#define PU_CONTRAST_STEP_SIZE       1
#define PU_CONTRAST_DEFAULT_VAL     127

#define PU_HUE_MIN_VAL              0
#define PU_HUE_MAX_VAL              255
#define PU_HUE_STEP_SIZE            1
#define PU_HUE_DEFAULT_VAL          127

#define PU_SATURATION_MIN_VAL       0
#define PU_SATURATION_MAX_VAL       255
#define PU_SATURATION_STEP_SIZE     1
#define PU_SATURATION_DEFAULT_VAL   127

#define PU_SHARPNESS_MIN_VAL        0
#define PU_SHARPNESS_MAX_VAL        255
#define PU_SHARPNESS_STEP_SIZE      1
#define PU_SHARPNESS_DEFAULT_VAL    127

#define PU_GAMMA_MIN_VAL            0
#define PU_GAMMA_MAX_VAL            255
#define PU_GAMMA_STEP_SIZE          1
#define PU_GAMMA_DEFAULT_VAL        127

#define PU_WHITE_BALANCE_TEMPERATURE_MIN_VAL        0
#define PU_WHITE_BALANCE_TEMPERATURE_MAX_VAL        255
#define PU_WHITE_BALANCE_TEMPERATURE_STEP_SIZE      1
#define PU_WHITE_BALANCE_TEMPERATURE_DEFAULT_VAL    127

#define PU_GAIN_MIN_VAL             0
#define PU_GAIN_MAX_VAL             255
#define PU_GAIN_STEP_SIZE           1
#define PU_GAIN_DEFAULT_VAL         127

#define PU_HUE_AUTO_MIN_VAL         0
#define PU_HUE_AUTO_MAX_VAL         255
#define PU_HUE_AUTO_STEP_SIZE       1
#define PU_HUE_AUTO_DEFAULT_VAL     127

/* ---------------------------------------------------------------------------
 * UVC specific stuff
 */

struct uvc_frame_info {
    unsigned int width;
    unsigned int height;
    unsigned int intervals[8];
};

struct uvc_format_info {
    unsigned int fcc;
    const struct uvc_frame_info *frames;
};

static const struct uvc_frame_info uvc_frames_yuyv[] = {
    {  640, 480, { 333333, 666666, 1000000, 2000000, 0 }, },
    { 1280, 720, { 333333, 1000000, 2000000, 0 }, },
    { 0, 0, { 0, }, },
};

static const struct uvc_frame_info uvc_frames_mjpeg[] = {
    {  640, 480, { 333333, 666666, 1000000, 2000000, 0 }, },
    { 1280, 720, { 333333, 666666, 1000000, 2000000, 0 }, },
    { 1920, 1080, { 333333, 666666, 1000000, 2000000, 0 }, },
    { 2560, 1440, { 333333, 666666, 1000000, 2000000, 0 }, },
    { 2592, 1944, { 333333, 666666, 1000000, 2000000, 0 }, },
    { 0, 0, { 0, }, },
};

static const struct uvc_frame_info uvc_frames_h264[] = {
    {  640, 480, { 333333, 400000, 500000, 666666, 1000000, 2000000, 0 }, },
    { 1280, 720, { 333333, 400000, 500000, 666666, 1000000, 2000000, 0 }, },
    { 1920, 1080, { 333333, 400000, 500000, 666666, 1000000, 2000000, 0 }, },
    { 3840, 2160, { 333333, 400000, 500000, 666666, 1000000, 2000000, 0 }, },
    { 0, 0, { 0, }, },
};

static const struct uvc_format_info uvc_formats[] = {
    { V4L2_PIX_FMT_YUYV, uvc_frames_yuyv },
    { V4L2_PIX_FMT_MJPEG, uvc_frames_mjpeg },
    { V4L2_PIX_FMT_H264, uvc_frames_h264 },
};

/*
 * Resolve @w x @h + @fcc to format/frame indices in uvc_formats[].
 * Returns true and fills @iformat/@iframe on match.
 */
static bool
uvc_find_frame(unsigned int w, unsigned int h, unsigned int fcc,
               unsigned int *iformat, unsigned int *iframe)
{
    unsigned int fi, fr;

    for (fi = 0; fi < ARRAY_SIZE(uvc_formats); ++fi) {
        const struct uvc_format_info *format = &uvc_formats[fi];
        if (format->fcc != fcc)
            continue;
        for (fr = 0; format->frames[fr].width != 0; ++fr) {
            if (format->frames[fr].width == w &&
                format->frames[fr].height == h) {
                *iformat = fi;
                *iframe = fr;
                return true;
            }
        }
    }
    return false;
}

/*
 * Pick the frame interval (100ns units) matching @fps from @frame's declared
 * intervals; falls back to the first interval when @fps is 0 or not advertised.
 */
static unsigned int
uvc_pick_interval(const struct uvc_frame_info *frame, int fps)
{
    unsigned int i;

    if (fps > 0) {
        unsigned int want = 10000000u / (unsigned int)fps;
        for (i = 0; i < ARRAY_SIZE(frame->intervals) && frame->intervals[i]; ++i)
            if (frame->intervals[i] == want)
                return frame->intervals[i];
    }
    return frame->intervals[0];
}

/*
 * Populate @cfg with the union of resolutions + frame rates advertised by the
 * UVC gadget descriptor (mirrors the uvc_formats[] table). Used by the public
 * uvc_get_sys_config() entry point.
 */
void
uvc_gadget_fill_sys_config(struct uvc_config *cfg)
{
    unsigned int fi, fr, i;
    int rcount = 0, fcount = 0;

    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));

    for (fi = 0; fi < ARRAY_SIZE(uvc_formats); ++fi) {
        const struct uvc_format_info *format = &uvc_formats[fi];
        for (fr = 0; format->frames[fr].width != 0; ++fr) {
            const struct uvc_frame_info *frame = &format->frames[fr];
            int r;

            for (r = 0; r < rcount; ++r)
                if (cfg->resolutions[r].width == (int)frame->width &&
                    cfg->resolutions[r].height == (int)frame->height)
                    break;
            if (r == rcount && rcount < UVC_SYS_MAX_RESOLUTIONS) {
                cfg->resolutions[rcount].width = (int)frame->width;
                cfg->resolutions[rcount].height = (int)frame->height;
                rcount++;
            }

            for (i = 0; i < ARRAY_SIZE(frame->intervals) && frame->intervals[i]; ++i) {
                int fps = (int)(10000000u / frame->intervals[i]);
                int f;
                for (f = 0; f < fcount; ++f)
                    if (cfg->fps[f] == fps)
                        break;
                if (f == fcount && fcount < UVC_SYS_MAX_FPS) {
                    cfg->fps[fcount] = fps;
                    fcount++;
                }
            }
        }
    }
    cfg->resolution_count = rcount;
    cfg->fps_count = fcount;
}

/*
 * Check whether the (fcc, w, h, fps) combination is advertised by the UVC
 * gadget descriptor. Used by uvc_set_config() to validate user input.
 */
bool
uvc_gadget_config_supported(unsigned int fcc, unsigned int w, unsigned int h,
                            int fps)
{
    unsigned int iformat, iframe, i;
    const struct uvc_frame_info *frame;

    if (fps <= 0)
        return false;
    if (!uvc_find_frame(w, h, fcc, &iformat, &iframe))
        return false;

    frame = &uvc_formats[iformat].frames[iframe];
    for (i = 0; i < ARRAY_SIZE(frame->intervals) && frame->intervals[i]; ++i)
        if (frame->intervals[i] == 10000000u / (unsigned int)fps)
            return true;
    return false;
}

/* ---------------------------------------------------------------------------
 * V4L2 and UVC device instances (see uvc_gadget_internal.h)
 */

/* forward declarations */
static int uvc_video_stream(struct uvc_device *dev, int enable);

/* ---------------------------------------------------------------------------
 * V4L2 streaming related
 */

static int
v4l2_uninit_device(struct v4l2_device *dev)
{
    unsigned int i;
    int ret;

    switch (dev->io) {
    case IO_METHOD_MMAP:
        for (i = 0; i < dev->nbufs; ++i) {
            ret = munmap (dev->mem[i].start, dev->mem[i].length);
            if (ret < 0) {
                uvc_log_printf("V4L2: munmap failed\n");
                return ret;
            }
        }

        free(dev->mem);
        break;

    case IO_METHOD_USERPTR:
    default:
        break;
    }

    return 0;
}

static int
v4l2_reqbufs_mmap(struct v4l2_device *dev, int nbufs)
{
    struct v4l2_requestbuffers req;
    unsigned int i = 0;
    int ret;

    CLEAR(req);

    req.count = nbufs;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(dev->v4l2_fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        if (ret == -EINVAL)
            uvc_log_printf("V4L2: does not support memory mapping\n");
        else
            uvc_log_printf("V4L2: VIDIOC_REQBUFS error %s (%d).\n",
                   strerror(errno), errno);
        goto err;
    }

    if (!req.count)
        return 0;

    if (req.count < 2) {
        uvc_log_printf("V4L2: Insufficient buffer memory.\n");
        ret = -EINVAL;
        goto err;
    }

    /* Map the buffers. */
    dev->mem = calloc(req.count, sizeof dev->mem[0]);
    if (!dev->mem) {
        uvc_log_printf("V4L2: Out of memory\n");
        ret = -ENOMEM;
        goto err;
    }

    for (i = 0; i < req.count; ++i) {
        memset(&dev->mem[i].buf, 0, sizeof(dev->mem[i].buf));

        dev->mem[i].buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        dev->mem[i].buf.memory = V4L2_MEMORY_MMAP;
        dev->mem[i].buf.index = i;

        ret = ioctl(dev->v4l2_fd, VIDIOC_QUERYBUF, &(dev->mem[i].buf));
        if (ret < 0) {
            uvc_log_printf("V4L2: VIDIOC_QUERYBUF failed for buf %d: "
                   "%s (%d).\n", i, strerror(errno), errno);
            ret = -EINVAL;
            goto err_free;
        }

        dev->mem[i].start = mmap (NULL /* start anywhere */,
                                  dev->mem[i].buf.length,
                                  PROT_READ | PROT_WRITE /* required */,
                                  MAP_SHARED /* recommended */,
                                  dev->v4l2_fd, dev->mem[i].buf.m.offset);

        if (MAP_FAILED == dev->mem[i].start) {
            uvc_log_printf("V4L2: Unable to map buffer %u: %s (%d).\n", i,
                   strerror(errno), errno);
            dev->mem[i].length = 0;
            ret = -EINVAL;
            goto err_free;
        }

        dev->mem[i].length = dev->mem[i].buf.length;
        uvc_log_printf("V4L2: Buffer %u mapped at address %p.\n", i,
               dev->mem[i].start);
    }

    dev->nbufs = req.count;
    uvc_log_printf("V4L2: %u buffers allocated.\n", req.count);

    return 0;

err_free:
    free(dev->mem);
err:
    return ret;
}

static int
v4l2_reqbufs_userptr(struct v4l2_device *dev, int nbufs)
{
    struct v4l2_requestbuffers req;
    int ret;

    CLEAR(req);

    req.count = nbufs;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;

    ret = ioctl(dev->v4l2_fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        if (ret == -EINVAL)
            uvc_log_printf("V4L2: does not support user pointer i/o\n");
        else
            uvc_log_printf("V4L2: VIDIOC_REQBUFS error %s (%d).\n",
                   strerror(errno), errno);
        return ret;
    }

    dev->nbufs = req.count;
    uvc_log_printf("V4L2: %u buffers allocated.\n", req.count);

    return 0;
}

static int
v4l2_reqbufs(struct v4l2_device *dev, int nbufs)
{
    int ret = 0;

    switch (dev->io) {
    case IO_METHOD_MMAP:
        ret = v4l2_reqbufs_mmap(dev, nbufs);
        break;

    case IO_METHOD_USERPTR:
        ret = v4l2_reqbufs_userptr(dev, nbufs);
        break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static int
v4l2_qbuf_mmap(struct v4l2_device *dev)
{
    unsigned int i;
    int ret;

    for (i = 0; i < dev->nbufs; ++i) {
        memset(&dev->mem[i].buf, 0, sizeof(dev->mem[i].buf));

        dev->mem[i].buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        dev->mem[i].buf.memory = V4L2_MEMORY_MMAP;
        dev->mem[i].buf.index = i;

        ret = ioctl(dev->v4l2_fd, VIDIOC_QBUF, &(dev->mem[i].buf));
        if (ret < 0) {
            uvc_log_printf("V4L2: VIDIOC_QBUF failed : %s (%d).\n",
                   strerror(errno), errno);
            return ret;
        }

        dev->qbuf_count++;
    }

    return 0;
}

static int
v4l2_qbuf(struct v4l2_device *dev)
{
    int ret = 0;

    switch (dev->io) {
    case IO_METHOD_MMAP:
        ret = v4l2_qbuf_mmap(dev);
        break;

    case IO_METHOD_USERPTR:
        /* Empty. */
        ret = 0;
        break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static int
v4l2_process_data(struct v4l2_device *dev)
{
    int ret;
    struct v4l2_buffer vbuf;
    struct v4l2_buffer ubuf;

    /* Return immediately if V4l2 streaming has not yet started. */
    if (!dev->is_streaming)
        return 0;

    if (dev->udev->first_buffer_queued)
        if (dev->dqbuf_count >= dev->qbuf_count)
            return 0;

    /* Dequeue spent buffer rom V4L2 domain. */
    CLEAR(vbuf);

    vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    switch (dev->io) {
    case IO_METHOD_USERPTR:
        vbuf.memory = V4L2_MEMORY_USERPTR;
        break;

    case IO_METHOD_MMAP:
    default:
        vbuf.memory = V4L2_MEMORY_MMAP;
        break;
    }

    ret = ioctl(dev->v4l2_fd, VIDIOC_DQBUF, &vbuf);
    if (ret < 0)
        return ret;

    dev->dqbuf_count++;

#ifdef ENABLE_BUFFER_DEBUG
    uvc_log_printf("Dequeueing buffer at V4L2 side = %d\n", vbuf.index);
#endif

    /* Queue video buffer to UVC domain. */
    CLEAR(ubuf);

    ubuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    switch (dev->udev->io) {
    case IO_METHOD_MMAP:
        ubuf.memory = V4L2_MEMORY_MMAP;
        ubuf.length = vbuf.length;
        ubuf.index = vbuf.index;
        ubuf.bytesused = vbuf.bytesused;
        break;

    case IO_METHOD_USERPTR:
    default:
        ubuf.memory = V4L2_MEMORY_USERPTR;
        ubuf.m.userptr = (unsigned long) dev->mem[vbuf.index].start;
        ubuf.length = dev->mem[vbuf.index].length;
        ubuf.index = vbuf.index;
        ubuf.bytesused = vbuf.bytesused;
        break;
    }

    ret = ioctl(dev->udev->uvc_fd, VIDIOC_QBUF, &ubuf);
    if (ret < 0) {
        uvc_log_printf("UVC: Unable to queue buffer %d: %s (%d).\n",
               ubuf.index, strerror(errno), errno);
        /* Check for a USB disconnect/shutdown event. */
        if (errno == ENODEV) {
            dev->udev->uvc_shutdown_requested = 1;
            uvc_log_printf("UVC: Possible USB shutdown requested from "
                   "Host, seen during VIDIOC_QBUF\n");
            return 0;
        } else {
            return ret;
        }
    }

    dev->udev->qbuf_count++;

#ifdef ENABLE_BUFFER_DEBUG
    uvc_log_printf("Queueing buffer at UVC side = %d\n", ubuf.index);
#endif

    if (!dev->udev->first_buffer_queued && !dev->udev->run_standalone) {
        uvc_video_stream(dev->udev, 1);
        dev->udev->first_buffer_queued = 1;
        dev->udev->is_streaming = 1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * V4L2 generic stuff
 */

static int
v4l2_get_format(struct v4l2_device *dev)
{
    struct v4l2_format fmt;
    int ret;

    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(dev->v4l2_fd, VIDIOC_G_FMT, &fmt);
    if (ret < 0) {
        uvc_log_printf("V4L2: Unable to get format: %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }

    uvc_log_printf("V4L2: Getting current format: %c%c%c%c %ux%u\n",
           pixfmtstr(fmt.fmt.pix.pixelformat),
           fmt.fmt.pix.width, fmt.fmt.pix.height);

    return 0;
}

static int
v4l2_set_format(struct v4l2_device *dev, struct v4l2_format *fmt)
{
    int ret;

    ret = ioctl(dev->v4l2_fd, VIDIOC_S_FMT, fmt);
    if (ret < 0) {
        uvc_log_printf("V4L2: Unable to set format %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }

    uvc_log_printf("V4L2: Setting format to: %c%c%c%c %ux%u\n",
           pixfmtstr(fmt->fmt.pix.pixelformat),
           fmt->fmt.pix.width, fmt->fmt.pix.height);

    return 0;
}

static int
v4l2_set_ctrl(struct v4l2_device *dev, int new_val, int ctrl)
{
    struct v4l2_queryctrl queryctrl;
    struct v4l2_control control;
    int ret;

    CLEAR(queryctrl);

    switch (ctrl) {
    case V4L2_CID_BRIGHTNESS:
        queryctrl.id = V4L2_CID_BRIGHTNESS;
        ret = ioctl(dev->v4l2_fd, VIDIOC_QUERYCTRL, &queryctrl);
        if (-1 == ret) {
            if (errno != EINVAL)
                uvc_log_printf("V4L2: VIDIOC_QUERYCTRL"
                       " failed: %s (%d).\n",
                       strerror(errno), errno);
            else
                uvc_log_printf("V4L2_CID_BRIGHTNESS is not"
                        " supported: %s (%d).\n",
                        strerror(errno), errno);

            return ret;
        } else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
            uvc_log_printf("V4L2_CID_BRIGHTNESS is not supported.\n");
            ret = -EINVAL;
            return ret;
        } else {
            CLEAR(control);
            control.id = V4L2_CID_BRIGHTNESS;
            control.value = new_val;

            ret = ioctl(dev->v4l2_fd, VIDIOC_S_CTRL, &control);
            if (-1 == ret) {
                uvc_log_printf("V4L2: VIDIOC_S_CTRL failed: %s (%d).\n",
                       strerror(errno), errno);
                return ret;
            }
        }
        uvc_log_printf("V4L2: Brightness control changed to value = 0x%x\n",
                new_val);
        break;

    default:
        /* TODO: We don't support any other controls. */
        return -EINVAL;
    }

    return 0;
}

static int
v4l2_start_capturing(struct v4l2_device *dev)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int ret;

    ret = ioctl(dev->v4l2_fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        uvc_log_printf("V4L2: Unable to start streaming: %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }

    uvc_log_printf("V4L2: Starting video stream.\n");

    return 0;
}

static int
v4l2_stop_capturing(struct v4l2_device *dev)
{
    enum v4l2_buf_type type;
    int ret;

    switch (dev->io) {
    case IO_METHOD_MMAP:
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        ret = ioctl(dev->v4l2_fd, VIDIOC_STREAMOFF, &type);
        if (ret < 0) {
            uvc_log_printf("V4L2: VIDIOC_STREAMOFF failed: %s (%d).\n",
                   strerror(errno), errno);
            return ret;
        }

        break;
    default:
        /* Nothing to do. */
        break;
    }

    return 0;
}

static int
v4l2_open(struct v4l2_device **v4l2, char *devname, struct v4l2_format *s_fmt)
{
    struct v4l2_device *dev;
    struct v4l2_capability cap;
    int fd;
    int ret = -EINVAL;

    fd = open(devname, O_RDWR | O_NONBLOCK, 0);
    if (fd == -1) {
        uvc_log_printf("V4L2: device open failed: %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }

    ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0) {
        uvc_log_printf("V4L2: VIDIOC_QUERYCAP failed: %s (%d).\n",
                strerror(errno), errno);
        goto err;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        uvc_log_printf("V4L2: %s is no video capture device\n", devname);
        goto err;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        uvc_log_printf("V4L2: %s does not support streaming i/o\n",
               devname);
        goto err;
    }

    dev = calloc(1, sizeof * dev);
    if (dev == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    uvc_log_printf("V4L2 device is %s on bus %s\n", cap.card, cap.bus_info);

    dev->v4l2_fd = fd;

    /* Get the default image format supported. */
    ret = v4l2_get_format(dev);
    if (ret < 0)
        goto err_free;

    /*
     * Set the desired image format.
     * Note: VIDIOC_S_FMT may change width and height.
     */
    ret = v4l2_set_format(dev, s_fmt);
    if (ret < 0)
        goto err_free;

    /* Get the changed image format. */
    ret = v4l2_get_format(dev);
    if (ret < 0)
        goto err_free;

    uvc_log_printf("v4l2 open succeeded, file descriptor = %d\n", fd);

    *v4l2 = dev;

    return 0;

err_free:
    free(dev);
err:
    close (fd);

    return ret;
}

static void
v4l2_close(struct v4l2_device *dev)
{
    close(dev->v4l2_fd);
    free(dev);
}

/* ---------------------------------------------------------------------------
 * UVC generic stuff
 */

static int
uvc_video_set_format(struct uvc_device *dev)
{
    struct v4l2_format fmt;
    int ret;

    CLEAR(fmt);

    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = dev->width;
    fmt.fmt.pix.height = dev->height;
    fmt.fmt.pix.pixelformat = dev->fcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (dev->fcc == V4L2_PIX_FMT_MJPEG)
        fmt.fmt.pix.sizeimage = dev->imgsize * 2/*1.5*/;
    if (dev->fcc == V4L2_PIX_FMT_H264)
        fmt.fmt.pix.sizeimage = dev->width * dev->height * 2;

    ret = ioctl(dev->uvc_fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        uvc_log_printf("UVC: Unable to set format %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }

    uvc_log_printf("UVC: Setting format to: %c%c%c%c %ux%u\n",
           pixfmtstr(dev->fcc), dev->width, dev->height);

    return 0;
}

static int
uvc_video_stream(struct uvc_device *dev, int enable)
{
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    int ret;

    if (!enable) {
        ret = ioctl(dev->uvc_fd, VIDIOC_STREAMOFF, &type);
        if (ret < 0) {
            uvc_log_printf("UVC: VIDIOC_STREAMOFF failed: %s (%d).\n",
                   strerror(errno), errno);
            return ret;
        }

        uvc_log_printf("UVC: Stopping video stream.\n");

        return 0;
    }

    ret = ioctl(dev->uvc_fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        uvc_log_printf("UVC: Unable to start streaming %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }

    uvc_log_printf("UVC: Starting video stream.\n");

    dev->uvc_shutdown_requested = 0;

    return 0;
}

static int
uvc_uninit_device(struct uvc_device *dev)
{
    unsigned int i;
    int ret;

    switch (dev->io) {
    case IO_METHOD_DMABUF:
        break;

    case IO_METHOD_MMAP:
        for (i = 0; i < dev->nbufs; ++i) {
            ret = munmap(dev->mem[i].start, dev->mem[i].length);
            if (ret < 0) {
                uvc_log_printf("UVC: munmap failed\n");
                return ret;
            }
        }

        free(dev->mem);
        break;

    case IO_METHOD_USERPTR:
    default:
        if (dev->run_standalone) {
            for (i = 0; i < dev->nbufs; ++i)
                free(dev->dummy_buf[i].start);

            free(dev->dummy_buf);
        }
        break;
    }

    return 0;
}

static int
uvc_open(struct uvc_device **uvc, char *devname)
{
    struct uvc_device *dev;
    struct v4l2_capability cap;
    int fd;
    int ret = -EINVAL;

    fd = open(devname, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        uvc_log_printf("UVC: device open failed: %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }

    ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0) {
        uvc_log_printf("UVC: unable to query uvc device: %s (%d)\n",
               strerror(errno), errno);
        goto err;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        uvc_log_printf("UVC: %s is no video output device\n", devname);
        goto err;
    }

    dev = calloc(1, sizeof * dev);
    if (dev == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    uvc_log_printf("uvc device is %s on bus %s\n", cap.card, cap.bus_info);
    uvc_log_printf("uvc open succeeded, file descriptor = %d\n", fd);

    dev->uvc_fd = fd;
    pthread_mutex_init(&dev->dmabuf_lock, NULL);

    dev->brightness_val = PU_BRIGHTNESS_DEFAULT_VAL;
    dev->contrast_val = PU_CONTRAST_DEFAULT_VAL;
    dev->hue_val = PU_HUE_DEFAULT_VAL;
    dev->saturation_val = PU_SATURATION_DEFAULT_VAL;
    dev->sharpness_val = PU_SHARPNESS_DEFAULT_VAL;
    dev->gamma_val = PU_GAMMA_DEFAULT_VAL;
    dev->white_balance_temperature_val = PU_WHITE_BALANCE_TEMPERATURE_DEFAULT_VAL;
    dev->gain_val = PU_GAIN_DEFAULT_VAL;
    dev->hue_auto_val = PU_HUE_AUTO_DEFAULT_VAL;
    dev->power_line_frequency_val = V4L2_CID_POWER_LINE_FREQUENCY_50HZ;

    *uvc = dev;

    return 0;

err:
    close(fd);
    return ret;
}

static void
uvc_close(struct uvc_device *dev)
{
    if (dev->uvc_fd >= 0) {
        close(dev->uvc_fd);
        dev->uvc_fd = -1;
    }
    pthread_mutex_destroy(&dev->dmabuf_lock);
    free(dev->imgdata);
    free(dev);
}

/* ---------------------------------------------------------------------------
 * UVC streaming related
 */

static void
uvc_video_fill_buffer(struct uvc_device *dev, struct v4l2_buffer *buf)
{
#if 0
    unsigned int bpl;
    unsigned int i;

    switch (dev->fcc) {
    case V4L2_PIX_FMT_YUYV:
        /* Fill the buffer with video data. */
        bpl = dev->width * 2;
        for (i = 0; i < dev->height; ++i)
            memset(dev->mem[buf->index].start + i * bpl,
                   dev->color++, bpl);

        buf->bytesused = bpl * dev->height;
        break;

    case V4L2_PIX_FMT_MJPEG:
    case V4L2_PIX_FMT_H264:
        memcpy(dev->mem[buf->index].start, dev->imgdata, dev->imgsize);
        buf->bytesused = dev->imgsize;
        break;

    }
#else
    uvc_user_fill_buffer(dev, buf, dev->video_id);
#endif
}

static bool uvc_video_is_link_error(int error)
{
    return error == ENODEV || error == ENXIO || error == ESHUTDOWN ||
           error == EIO;
}

static void uvc_video_usb_link_lost(struct uvc_device *dev,
                                    const char *where, int error)
{
    if (!uvc_video_is_link_error(error))
        return;

    if (dev->uvc_shutdown_requested)
        return;

    if (dev->io == IO_METHOD_DMABUF)
        pthread_mutex_lock(&dev->dmabuf_lock);
    dev->uvc_shutdown_requested = 1;
    dev->is_streaming = 0;
    dev->dmabuf_ready = false;
    if (dev->io == IO_METHOD_DMABUF)
        pthread_mutex_unlock(&dev->dmabuf_lock);
    uvc_log_printf("%d: UVC: USB link lost during %s: %s (%d)\n",
           dev->video_id, where, strerror(error), error);
    uvc_control_exit();
    uvc_set_user_run_state(false, dev->video_id);
}

static int
uvc_video_process(struct uvc_device *dev)
{
    struct v4l2_buffer vbuf;
    unsigned int i;
    int ret;
    bool dmabuf_locked = false;

    if (dev->io == IO_METHOD_DMABUF && dev->run_standalone) {
        pthread_mutex_lock(&dev->dmabuf_lock);
        dmabuf_locked = true;
    }

    /*
     * Return immediately if UVC video output device has not started
     * streaming yet.
     */
    if (!dev->is_streaming) {
        if (dmabuf_locked)
            pthread_mutex_unlock(&dev->dmabuf_lock);
        return 0;
    }

    /* Prepare a v4l2 buffer to be dequeued from UVC domain. */
    CLEAR(dev->ubuf);

    dev->ubuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    switch (dev->io) {
    case IO_METHOD_DMABUF:
        dev->ubuf.memory = V4L2_MEMORY_DMABUF;
        break;

    case IO_METHOD_MMAP:
        dev->ubuf.memory = V4L2_MEMORY_MMAP;
        break;

    case IO_METHOD_USERPTR:
    default:
        dev->ubuf.memory = V4L2_MEMORY_USERPTR;
        break;
    }

    if (dev->run_standalone) {
        /* UVC stanalone setup. */
        ret = ioctl(dev->uvc_fd, VIDIOC_DQBUF, &dev->ubuf);
        if (ret < 0) {
            int error = errno;

            if (dmabuf_locked)
                pthread_mutex_unlock(&dev->dmabuf_lock);
            if (error != EAGAIN)
                uvc_video_usb_link_lost(dev, "VIDIOC_DQBUF", error);
            if (dev->uvc_shutdown_requested)
                return 0;
            return -error;
        }

        dev->dqbuf_count++;

        if (dev->io == IO_METHOD_DMABUF) {
            int source_fd = -1;

            if (dev->ubuf.index < dev->nbufs) {
                source_fd = dev->dmabuf_source_fds[dev->ubuf.index];
                close(dev->dmabuf_fds[dev->ubuf.index]);
                dev->dmabuf_fds[dev->ubuf.index] = -1;
                dev->dmabuf_source_fds[dev->ubuf.index] = -1;
                dev->dmabuf_queued[dev->ubuf.index] = false;
            }
            pthread_mutex_unlock(&dev->dmabuf_lock);
            if (source_fd >= 0)
                uvc_video_release_dmabuf(dev->video_id, source_fd);
            return 0;
        }

#ifdef ENABLE_BUFFER_DEBUG
        uvc_log_printf("%d: DeQueued buffer at UVC side = %d\n", dev->video_id, dev->ubuf.index);
#endif
        uvc_video_fill_buffer(dev, &dev->ubuf);

        ret = ioctl(dev->uvc_fd, VIDIOC_QBUF, &dev->ubuf);
        if (ret < 0) {
            int error = errno;

            uvc_log_printf("%d: UVC: Unable to queue buffer: %s (%d).\n",
                   dev->video_id, strerror(error), error);
            uvc_video_usb_link_lost(dev, "VIDIOC_QBUF", error);
            if (dev->uvc_shutdown_requested)
                return 0;
            return -error;
        }

        dev->qbuf_count++;

#ifdef ENABLE_BUFFER_DEBUG
        uvc_log_printf("%d: ReQueueing buffer at UVC side = %d\n", dev->video_id, dev->ubuf.index);
#endif
    } else {
        /* UVC - V4L2 integrated path. */

        /*
         * Return immediately if V4L2 video capture device has not
         * started streaming yet or if QBUF was not called even once on
         * the UVC side.
         */
        if (!dev->vdev->is_streaming || !dev->first_buffer_queued)
            return 0;

        /*
         * Do not dequeue buffers from UVC side until there are atleast
         * 2 buffers available at UVC domain.
         */
        if (!dev->uvc_shutdown_requested)
            if ((dev->dqbuf_count + 1) >= dev->qbuf_count)
                return 0;

        /* Dequeue the spent buffer from UVC domain */
        ret = ioctl(dev->uvc_fd, VIDIOC_DQBUF, &dev->ubuf);
        if (ret < 0)
            return ret;

        if (dev->io == IO_METHOD_USERPTR)
            for (i = 0; i < dev->nbufs; ++i)
                if (dev->ubuf.m.userptr ==
                    (unsigned long) dev->vdev->mem[i].start
                    && dev->ubuf.length == dev->vdev->mem[i].length)
                    break;

        dev->dqbuf_count++;

#ifdef ENABLE_BUFFER_DEBUG
        uvc_log_printf("DeQueued buffer at UVC side=%d\n", dev->ubuf.index);
#endif

        /*
         * If the dequeued buffer was marked with state ERROR by the
         * underlying UVC driver gadget, do not queue the same to V4l2
         * and wait for a STREAMOFF event on UVC side corresponding to
         * set_alt(0). So, now all buffers pending at UVC end will be
         * dequeued one-by-one and we will enter a state where we once
         * again wait for a set_alt(1) command from the USB host side.
         */
        if (dev->ubuf.flags & V4L2_BUF_FLAG_ERROR) {
            dev->uvc_shutdown_requested = 1;
            uvc_log_printf("UVC: Possible USB shutdown requested from "
                   "Host, seen during VIDIOC_DQBUF\n");
            return 0;
        }

        /* Queue the buffer to V4L2 domain */
        CLEAR(vbuf);

        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.index = dev->ubuf.index;

        ret = ioctl(dev->vdev->v4l2_fd, VIDIOC_QBUF, &vbuf);
        if (ret < 0) {
            uvc_log_printf("V4L2: Unable to queue buffer: %s (%d).\n",
                   strerror(errno), errno);
            return ret;
        }

        dev->vdev->qbuf_count++;

#ifdef ENABLE_BUFFER_DEBUG
        uvc_log_printf("ReQueueing buffer at V4L2 side = %d\n", vbuf.index);
#endif
    }

    return 0;
}

int uvc_gadget_queue_dmabuf(struct uvc_device *dev, int fd, size_t size)
{
    struct v4l2_buffer buf;
    unsigned int index;
    int owned_fd;
    int ret;

    if (!dev || fd < 0 || !size)
        return -EINVAL;

    pthread_mutex_lock(&dev->dmabuf_lock);
    if (dev->io != IO_METHOD_DMABUF || !dev->dmabuf_ready ||
        !dev->dmabuf_fds || !dev->dmabuf_queued) {
        pthread_mutex_unlock(&dev->dmabuf_lock);
        return -EOPNOTSUPP;
    }

    for (index = 0; index < dev->nbufs; ++index) {
        if (dev->dmabuf_queued[index] && dev->dmabuf_source_fds[index] == fd) {
            pthread_mutex_unlock(&dev->dmabuf_lock);
            return -EBUSY;
        }
    }

    for (index = 0; index < dev->nbufs; ++index) {
        if (!dev->dmabuf_queued[index])
            break;
    }
    if (index == dev->nbufs) {
        pthread_mutex_unlock(&dev->dmabuf_lock);
        return -EAGAIN;
    }

    owned_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned_fd < 0) {
        ret = -errno;
        pthread_mutex_unlock(&dev->dmabuf_lock);
        return ret;
    }

    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = index;
    buf.m.fd = owned_fd;
    buf.length = size;
    buf.bytesused = size;

    ret = ioctl(dev->uvc_fd, VIDIOC_QBUF, &buf);
    if (ret < 0) {
        int error = errno;

        ret = -error;
        close(owned_fd);
        if (uvc_video_is_link_error(error)) {
            dev->dmabuf_ready = false;
            dev->uvc_shutdown_requested = 1;
        }
        pthread_mutex_unlock(&dev->dmabuf_lock);
        if (uvc_video_is_link_error(error))
            uvc_set_user_run_state(false, dev->video_id);
        return ret;
    }

    dev->dmabuf_fds[index] = owned_fd;
    dev->dmabuf_source_fds[index] = fd;
    dev->dmabuf_queued[index] = true;
    dev->qbuf_count++;

    if (!dev->is_streaming) {
        ret = uvc_video_stream(dev, 1);
        if (ret < 0) {
            dev->dmabuf_ready = false;
            dev->uvc_shutdown_requested = 1;
            pthread_mutex_unlock(&dev->dmabuf_lock);
            uvc_set_user_run_state(false, dev->video_id);
            return 0;
        }
        dev->first_buffer_queued = 1;
        dev->is_streaming = 1;
    }

    pthread_mutex_unlock(&dev->dmabuf_lock);
    return 0;
}

static int
uvc_video_qbuf_mmap(struct uvc_device *dev)
{
    unsigned int i;
    int ret;

    for (i = 0; i < dev->nbufs; ++i) {
        memset(&dev->mem[i].buf, 0, sizeof(dev->mem[i].buf));

        dev->mem[i].buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        dev->mem[i].buf.memory = V4L2_MEMORY_MMAP;
        dev->mem[i].buf.index = i;

        ret = ioctl(dev->uvc_fd, VIDIOC_QBUF, &(dev->mem[i].buf));
        if (ret < 0) {
            uvc_log_printf("UVC: VIDIOC_QBUF failed : %s (%d).\n",
                   strerror(errno), errno);
            return ret;
        }

        dev->qbuf_count++;
    }

    return 0;
}

static int
uvc_video_qbuf_userptr(struct uvc_device *dev)
{
    unsigned int i;
    int ret;

    /* UVC standalone setup. */
    if (dev->run_standalone) {
        for (i = 0; i < dev->nbufs; ++i) {
            struct v4l2_buffer buf;

            CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            buf.memory = V4L2_MEMORY_USERPTR;
            buf.m.userptr = (unsigned long)dev->dummy_buf[i].start;
            buf.length = dev->dummy_buf[i].length;
            buf.index = i;

            ret = ioctl(dev->uvc_fd, VIDIOC_QBUF, &buf);
            if (ret < 0) {
                uvc_log_printf("UVC: VIDIOC_QBUF failed : %s (%d).\n",
                       strerror(errno), errno);
                return ret;
            }

            dev->qbuf_count++;
        }
    }

    return 0;
}

static int
uvc_video_qbuf(struct uvc_device *dev)
{
    int ret = 0;

    switch (dev->io) {
    case IO_METHOD_DMABUF:
        ret = 0;
        break;

    case IO_METHOD_MMAP:
        ret = uvc_video_qbuf_mmap(dev);
        break;

    case IO_METHOD_USERPTR:
        ret = uvc_video_qbuf_userptr(dev);
        break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static int
uvc_video_reqbufs_mmap(struct uvc_device *dev, int nbufs)
{
    struct v4l2_requestbuffers rb;
    unsigned int i;
    int ret;

    CLEAR(rb);

    rb.count = nbufs;
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(dev->uvc_fd, VIDIOC_REQBUFS, &rb);
    if (ret < 0) {
        if (ret == -EINVAL)
            uvc_log_printf("UVC: does not support memory mapping\n");
        else
            uvc_log_printf("UVC: Unable to allocate buffers: %s (%d).\n",
                   strerror(errno), errno);
        goto err;
    }

    if (!rb.count)
        return 0;

    if (rb.count < 2) {
        uvc_log_printf("UVC: Insufficient buffer memory.\n");
        ret = -EINVAL;
        goto err;
    }

    /* Map the buffers. */
    dev->mem = calloc(rb.count, sizeof dev->mem[0]);
    if (!dev->mem) {
        uvc_log_printf("UVC: Out of memory\n");
        ret = -ENOMEM;
        goto err;
    }

    for (i = 0; i < rb.count; ++i) {
        memset(&dev->mem[i].buf, 0, sizeof(dev->mem[i].buf));

        dev->mem[i].buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        dev->mem[i].buf.memory = V4L2_MEMORY_MMAP;
        dev->mem[i].buf.index = i;

        ret = ioctl(dev->uvc_fd, VIDIOC_QUERYBUF, &(dev->mem[i].buf));
        if (ret < 0) {
            uvc_log_printf("UVC: VIDIOC_QUERYBUF failed for buf %d: "
                   "%s (%d).\n", i, strerror(errno), errno);
            ret = -EINVAL;
            goto err_free;
        }

        dev->mem[i].start = mmap(NULL /* start anywhere */,
                                 dev->mem[i].buf.length,
                                 PROT_READ | PROT_WRITE /* required */,
                                 MAP_SHARED /* recommended */,
                                 dev->uvc_fd, dev->mem[i].buf.m.offset);

        if (MAP_FAILED == dev->mem[i].start) {
            uvc_log_printf("UVC: Unable to map buffer %u: %s (%d).\n", i,
                   strerror(errno), errno);
            dev->mem[i].length = 0;
            ret = -EINVAL;
            goto err_free;
        }

        dev->mem[i].length = dev->mem[i].buf.length;
        uvc_log_printf("UVC: Buffer %u mapped at address %p.\n", i,
               dev->mem[i].start);
    }

    dev->nbufs = rb.count;
    uvc_log_printf("UVC: %u buffers allocated.\n", rb.count);

    return 0;

err_free:
    free(dev->mem);
err:
    return ret;
}

static int
uvc_video_reqbufs_userptr(struct uvc_device *dev, int nbufs)
{
    struct v4l2_requestbuffers rb;
    unsigned int i, j, bpl = 0, payload_size;
    int ret;

    CLEAR(rb);

    rb.count = nbufs;
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_USERPTR;

    ret = ioctl(dev->uvc_fd, VIDIOC_REQBUFS, &rb);
    if (ret < 0) {
        if (ret == -EINVAL)
            uvc_log_printf("UVC: does not support user pointer i/o\n");
        else
            uvc_log_printf("UVC: VIDIOC_REQBUFS error %s (%d).\n",
                   strerror(errno), errno);
        goto err;
    }

    if (!rb.count)
        return 0;

    dev->nbufs = rb.count;
    uvc_log_printf("UVC: %u buffers allocated.\n", rb.count);

    if (dev->run_standalone) {
        /* Allocate buffers to hold dummy data pattern. */
        dev->dummy_buf = calloc(rb.count, sizeof dev->dummy_buf[0]);
        if (!dev->dummy_buf) {
            uvc_log_printf("UVC: Out of memory\n");
            ret = -ENOMEM;
            goto err;
        }

        switch (dev->fcc) {
        case V4L2_PIX_FMT_YUYV:
            bpl = dev->width * 2;
            payload_size = dev->width * dev->height * 2;
            break;
        case V4L2_PIX_FMT_MJPEG:
        case V4L2_PIX_FMT_H264:
            payload_size = dev->imgsize;
            break;
        default:
            return -1;
        }

        for (i = 0; i < rb.count; ++i) {
            dev->dummy_buf[i].length = payload_size;
            dev->dummy_buf[i].start = malloc(payload_size);
            if (!dev->dummy_buf[i].start) {
                uvc_log_printf("UVC: Out of memory\n");
                ret = -ENOMEM;
                goto err;
            }

            if (V4L2_PIX_FMT_YUYV == dev->fcc)
                for (j = 0; j < dev->height; ++j)
                    memset(dev->dummy_buf[i].start + j * bpl,
                           dev->color++, bpl);

            if (V4L2_PIX_FMT_MJPEG == dev->fcc)
                memcpy(dev->dummy_buf[i].start, dev->imgdata,
                       dev->imgsize);
        }
    }

    return 0;

err:
    return ret;

}

static int
uvc_video_reqbufs_dmabuf(struct uvc_device *dev, int nbufs)
{
    struct v4l2_requestbuffers rb;
    unsigned int i;
    int ret;

    CLEAR(rb);
    rb.count = nbufs;
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_DMABUF;

    ret = ioctl(dev->uvc_fd, VIDIOC_REQBUFS, &rb);
    if (ret < 0) {
        uvc_log_printf("UVC: DMABUF request failed: %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }
    if (!rb.count)
        return 0;

    dev->dmabuf_fds = malloc(rb.count * sizeof(dev->dmabuf_fds[0]));
    dev->dmabuf_source_fds = malloc(rb.count * sizeof(dev->dmabuf_source_fds[0]));
    dev->dmabuf_queued = calloc(rb.count, sizeof(dev->dmabuf_queued[0]));
    if (!dev->dmabuf_fds || !dev->dmabuf_source_fds || !dev->dmabuf_queued) {
        free(dev->dmabuf_fds);
        free(dev->dmabuf_source_fds);
        free(dev->dmabuf_queued);
        dev->dmabuf_fds = NULL;
        dev->dmabuf_source_fds = NULL;
        dev->dmabuf_queued = NULL;
        return -ENOMEM;
    }

    for (i = 0; i < rb.count; ++i) {
        dev->dmabuf_fds[i] = -1;
        dev->dmabuf_source_fds[i] = -1;
    }
    dev->nbufs = rb.count;
    dev->dmabuf_ready = true;
    uvc_log_printf("UVC: %u DMABUF slots allocated.\n", rb.count);
    return 0;
}

static int
uvc_video_reqbufs(struct uvc_device *dev, int nbufs)
{
    int ret = 0;

    switch (dev->io) {
    case IO_METHOD_DMABUF:
        ret = uvc_video_reqbufs_dmabuf(dev, nbufs);
        if (ret < 0 && nbufs > 0) {
            struct v4l2_requestbuffers rb;

            uvc_log_printf("UVC: falling back from DMABUF to MMAP.\n");
            CLEAR(rb);
            rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            rb.memory = V4L2_MEMORY_DMABUF;
            ioctl(dev->uvc_fd, VIDIOC_REQBUFS, &rb);
            dev->io = IO_METHOD_MMAP;
            ret = uvc_video_reqbufs_mmap(dev, nbufs);
        }
        break;

    case IO_METHOD_MMAP:
        ret = uvc_video_reqbufs_mmap(dev, nbufs);
        break;

    case IO_METHOD_USERPTR:
        ret = uvc_video_reqbufs_userptr(dev, nbufs);
        break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static void uvc_video_release_buffers(struct uvc_device *dev)
{
    unsigned int i;
    int *dmabuf_fds;
    int *source_fds;
    bool *dmabuf_queued;
    unsigned int dmabuf_count;

    if (!dev)
        return;

    pthread_mutex_lock(&dev->dmabuf_lock);
    dev->dmabuf_ready = false;
    pthread_mutex_unlock(&dev->dmabuf_lock);

    uvc_control_exit();

    pthread_mutex_lock(&dev->dmabuf_lock);

    if (dev->is_streaming) {
        uvc_video_stream(dev, 0);
        dev->is_streaming = 0;
    }

    dmabuf_fds = dev->dmabuf_fds;
    source_fds = dev->dmabuf_source_fds;
    dmabuf_queued = dev->dmabuf_queued;
    dmabuf_count = dev->nbufs;
    dev->dmabuf_fds = NULL;
    dev->dmabuf_source_fds = NULL;
    dev->dmabuf_queued = NULL;

    pthread_mutex_unlock(&dev->dmabuf_lock);

    if (dev->mem || dev->dummy_buf) {
        uvc_uninit_device(dev);
        dev->mem = NULL;
        dev->dummy_buf = NULL;
    }

    uvc_video_reqbufs(dev, 0);
    dev->first_buffer_queued = 0;

    for (i = 0; dmabuf_fds && i < dmabuf_count; ++i) {
        if (dmabuf_fds[i] >= 0)
            close(dmabuf_fds[i]);
        if (source_fds[i] >= 0)
            uvc_video_release_dmabuf(dev->video_id, source_fds[i]);
    }
    free(dmabuf_fds);
    free(source_fds);
    free(dmabuf_queued);
}

/*
 *  - A SET_ALT(interface 1, alt setting 1) command from USB host,
 *    if the UVC gadget supports an ISOCHRONOUS video streaming endpoint
 *    or,
 *
 *  - A UVC_VS_COMMIT_CONTROL command from USB host, if the UVC gadget
 *    supports a BULK type video streaming endpoint.
 */
static int
uvc_handle_streamon_event(struct uvc_device *dev)
{
    int ret;

    if (!uvc_get_user_run_state(dev->video_id))
        return -EINTR;

    uvc_video_release_buffers(dev);

    if (!uvc_get_user_run_state(dev->video_id))
        return -EINTR;

    ret = uvc_video_reqbufs(dev, dev->nbufs);
    if (ret < 0)
        goto err;

    if (!dev->run_standalone) {
        /* UVC - V4L2 integrated path. */
        if (IO_METHOD_USERPTR == dev->vdev->io) {
            /*
             * Ensure that the V4L2 video capture device has already
             * some buffers queued.
             */
            ret = v4l2_reqbufs(dev->vdev, dev->vdev->nbufs);
            if (ret < 0)
                goto err;
        }
        ret = v4l2_qbuf(dev->vdev);
        if (ret < 0)
            goto err;


        /* Start V4L2 capturing now. */
        ret = v4l2_start_capturing(dev->vdev);
        if (ret < 0)
            goto err;

        dev->vdev->is_streaming = 1;
    }

    /* Common setup. */

    /* Queue buffers to UVC domain and start streaming. */
    ret = uvc_video_qbuf(dev);
    if (ret < 0)
        goto err;

    if (dev->run_standalone && dev->io != IO_METHOD_DMABUF) {
        uvc_video_stream(dev, 1);
        dev->first_buffer_queued = 1;
        dev->is_streaming = 1;
    }

    /* dev->fcc/width/height/fps come from the UVC commit. Do not replace
     * them here with the application default after buffers are configured;
     * that would make the USB negotiation and encoder disagree. */
    uvc_control_init(dev->width, dev->height, dev->fcc, dev->fps);
    return 0;

err:
    uvc_video_release_buffers(dev);
    return ret;
}

/* ---------------------------------------------------------------------------
 * UVC Request processing
 */

static void
uvc_fill_streaming_control(struct uvc_device *dev,
                           struct uvc_streaming_control *ctrl,
                           int iframe, int iformat, bool use_user_default)
{
    const struct uvc_format_info *format;
    const struct uvc_frame_info *frame;
    unsigned int nframes;
    unsigned int interval = 0;

    /* For the initial probe/commit and GET_DEF, use the application-owned
     * format selected by uvc_set_config (or the built-in fixed default).
     * GET_MIN/MAX retain their descriptor boundary semantics. */
    if (use_user_default) {
        int w, h, fps;
        unsigned int uf, ui;
        int configured_fcc;

        if (uvc_control_get_default_config(&w, &h, &configured_fcc, &fps)) {
            if (!uvc_find_frame((unsigned int)w, (unsigned int)h,
                                (unsigned int)configured_fcc, &uf, &ui)) {
                uvc_log_printf("UVC: fixed config is not advertised\n");
                memset(ctrl, 0, sizeof(*ctrl));
                return;
            }
            iformat = (int)uf;
            iframe = (int)ui;
            frame = &uvc_formats[iformat].frames[iframe];
            interval = uvc_pick_interval(frame, fps);
        }
    }

    if (iformat < 0)
        iformat = ARRAY_SIZE(uvc_formats) + iformat;
    if (iformat < 0 || iformat >= (int)ARRAY_SIZE(uvc_formats))
        return;
    format = &uvc_formats[iformat];

    nframes = 0;
    while (format->frames[nframes].width != 0)
        ++nframes;

    if (iframe < 0)
        iframe = nframes + iframe;
    if (iframe < 0 || iframe >= (int)nframes)
        return;
    frame = &format->frames[iframe];

    memset(ctrl, 0, sizeof * ctrl);

    ctrl->bmHint = 1;
    ctrl->bFormatIndex = iformat + 1;
    ctrl->bFrameIndex = iframe + 1;
    ctrl->dwFrameInterval = interval ? interval : frame->intervals[0];
    switch (format->fcc) {
    case V4L2_PIX_FMT_YUYV:
        ctrl->dwMaxVideoFrameSize = frame->width * frame->height * 2;
        break;
    case V4L2_PIX_FMT_MJPEG:
    case V4L2_PIX_FMT_H264:
        /* Only report the max frame size; do not mutate dev->width/height/
         * imgsize here. GET_DEF/MIN/MAX must not clobber the negotiated
         * state (which is owned by apply_config + uvc_events_process_data). */
        ctrl->dwMaxVideoFrameSize = frame->width * frame->height * 2/*1.5*/;
        break;
    }

    /* TODO: the UVC maxpayload transfer size should be filled
     * by the driver.
     */
    if (!dev->bulk)
        ctrl->dwMaxPayloadTransferSize = (dev->maxpkt) *
                                         (dev->mult + 1) * (dev->burst + 1);
    else
        ctrl->dwMaxPayloadTransferSize = ctrl->dwMaxVideoFrameSize;

    ctrl->bmFramingInfo = 3;
    ctrl->bPreferedVersion = 1;
    ctrl->bMaxVersion = 1;
}

static void
uvc_events_process_standard(struct uvc_device *dev,
                            struct usb_ctrlrequest *ctrl,
                            struct uvc_request_data *resp)
{
    uvc_log_printf("standard request\n");
    (void)dev;
    (void)ctrl;
    (void)resp;
}

static void
uvc_events_process_control(struct uvc_device *dev, uint8_t req,
                           uint8_t cs, uint8_t entity_id,
                           uint8_t len, struct uvc_request_data *resp)
{
    uvc_log_printf("req = %d cs = %d entity_id =%d len = %d \n", req, cs, entity_id, len);
    dev->cs = cs;
    dev->entity_id = entity_id;

    switch (entity_id) {
    case 0:
        switch (cs) {
        case UVC_VC_REQUEST_ERROR_CODE_CONTROL:
            /* Send the request error code last prepared. */
            resp->data[0] = dev->request_error_code.data[0];
            resp->length = dev->request_error_code.length;
            break;

        default:
            /*
             * If we were not supposed to handle this
             * 'cs', prepare an error code response.
             */
            dev->request_error_code.data[0] = 0x06;
            dev->request_error_code.length = 1;
            break;
        }
        break;

        /* Camera terminal unit 'UVC_VC_INPUT_TERMINAL'. */
    case 1:
        switch (cs) {
            /*
             * We support only 'UVC_CT_AE_MODE_CONTROL' for CAMERA
             * terminal, as our bmControls[0] = 2 for CT. Also we
             * support only auto exposure.
             */
        case UVC_CT_AE_MODE_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                /* Incase of auto exposure, attempts to
                 * programmatically set the auto-adjusted
                 * controls are ignored.
                 */
                resp->data[0] = 0x01;
                resp->length = 1;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;

            case UVC_GET_INFO:
                /*
                 * TODO: We support Set and Get requests, but
                 * don't support async updates on an video
                 * status (interrupt) endpoint as of
                 * now.
                 */
                resp->data[0] = 0x03;
                resp->length = 1;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;

            case UVC_GET_CUR:
            case UVC_GET_DEF:
            case UVC_GET_RES:
                /* Auto Mode Ã¢?? auto Exposure Time, auto Iris. */
                resp->data[0] = 0x02;
                resp->length = 1;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                /*
                 * We don't support this control, so STALL the
                 * control ep.
                 */
                resp->length = -EL2HLT;
                /*
                 * For every unsupported control request
                 * set the request error code to appropriate
                 * value.
                 */
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL:
            switch (req) {
            case UVC_GET_INFO:
            case UVC_GET_MIN:
            case UVC_GET_MAX:
            case UVC_GET_CUR:
            case UVC_GET_DEF:
            case UVC_GET_RES:
                resp->data[0] = 100;
                resp->length = len;

                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                /*
                 * We don't support this control, so STALL the
                 * control ep.
                 */
                resp->length = -EL2HLT;
                /*
                 * For every unsupported control request
                 * set the request error code to appropriate
                 * value.
                 */
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
            }
            break;
        case UVC_CT_IRIS_ABSOLUTE_CONTROL:
            switch (req) {
            case UVC_GET_INFO:
            case UVC_GET_CUR:
            case UVC_GET_MIN:
            case UVC_GET_MAX:
            case UVC_GET_DEF:
            case UVC_GET_RES:
                resp->data[0] = 10;
                resp->length = len;

                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                /*
                 * We don't support this control, so STALL the
                 * control ep.
                 */
                resp->length = -EL2HLT;
                /*
                 * For every unsupported control request
                 * set the request error code to appropriate
                 * value.
                 */
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
            }
            break;

        default:
            /*
             * We don't support this control, so STALL the control
             * ep.
             */
            resp->length = -EL2HLT;
            /*
             * If we were not supposed to handle this
             * 'cs', prepare a Request Error Code response.
             */
            dev->request_error_code.data[0] = 0x06;
            dev->request_error_code.length = 1;
            break;
        }
        break;

        /* processing unit 'UVC_VC_PROCESSING_UNIT' */
    case 2:
        switch (cs) {
        case UVC_PU_BRIGHTNESS_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                uvc_log_printf("set brightness\n");
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_BRIGHTNESS_MIN_VAL;
                resp->length = 2;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_BRIGHTNESS_MAX_VAL;
                resp->length = 2;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->brightness_val,
                       resp->length);
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                /*
                 * We support Set and Get requests and don't
                 * support async updates on an interrupt endpt
                 */
                resp->data[0] = 0x03;
                resp->length = 1;
                /*
                 * For every successfully handled control
                 * request, set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_BRIGHTNESS_DEFAULT_VAL;
                resp->length = 2;
                /*
                 * For every successfully handled control
                 * request, set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_BRIGHTNESS_STEP_SIZE;
                resp->length = 2;
                /*
                 * For every successfully handled control
                 * request, set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                /*
                 * We don't support this control, so STALL the
                 * default control ep.
                 */
                resp->length = -EL2HLT;
                /*
                 * For every unsupported control request
                 * set the request error code to appropriate
                 * code.
                 */
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_CONTRAST_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_CONTRAST_MIN_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_CONTRAST_MAX_VAL % 256;
                resp->data[1] = PU_CONTRAST_MAX_VAL / 256;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->contrast_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_CONTRAST_DEFAULT_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_CONTRAST_STEP_SIZE;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_HUE_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_HUE_MIN_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_HUE_MAX_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->hue_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_HUE_DEFAULT_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_HUE_STEP_SIZE;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_SATURATION_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_SATURATION_MIN_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_SATURATION_MAX_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->saturation_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_SATURATION_DEFAULT_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_SATURATION_STEP_SIZE;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_SHARPNESS_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_SHARPNESS_MIN_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_SHARPNESS_MAX_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->sharpness_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_SHARPNESS_DEFAULT_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_SHARPNESS_STEP_SIZE;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_GAMMA_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_GAMMA_MIN_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_GAMMA_MAX_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->gamma_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_GAMMA_DEFAULT_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_GAMMA_STEP_SIZE;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_WHITE_BALANCE_TEMPERATURE_MIN_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_WHITE_BALANCE_TEMPERATURE_MAX_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->white_balance_temperature_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_WHITE_BALANCE_TEMPERATURE_DEFAULT_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_WHITE_BALANCE_TEMPERATURE_STEP_SIZE;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_GAIN_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_GAIN_MIN_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_GAIN_MAX_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->gain_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_GAIN_DEFAULT_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_GAIN_STEP_SIZE;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_HUE_AUTO_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = PU_HUE_AUTO_MIN_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = PU_HUE_AUTO_MAX_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 2;
                memcpy(&resp->data[0], &dev->hue_auto_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = PU_HUE_AUTO_DEFAULT_VAL;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = PU_HUE_AUTO_STEP_SIZE;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        case UVC_PU_POWER_LINE_FREQUENCY_CONTROL:
            switch (req) {
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = 1;
                memcpy(&resp->data[0], &dev->power_line_frequency_val,
                       resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = 1;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;
        default:
            /*
             * We don't support this control, so STALL the control
             * ep.
             */
            resp->length = -EL2HLT;
            /*
             * If we were not supposed to handle this
             * 'cs', prepare a Request Error Code response.
             */
            dev->request_error_code.data[0] = 0x06;
            dev->request_error_code.length = 1;
            break;
        }

        break;

    case 6:
        switch (cs) {
        case 1:
            switch (req) {
            case UVC_GET_LEN:
                resp->data[0] = 0x4;
                resp->length = len;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = 0;
                resp->length = 4;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = 0xFF;
                resp->data[1] = 0xFF;
                resp->data[2] = 0xFF;
                resp->data[3] = 0xFF;
                resp->length = 4;
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = len < sizeof(dev->extension_io_data) ? len : sizeof(dev->extension_io_data);
                memcpy(resp->data, dev->extension_io_data, resp->length);
                /*
                 * For every successfully handled control
                 * request set the request error code to no
                 * error
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                /*
                 * We support Set and Get requests and don't
                 * support async updates on an interrupt endpt
                 */
                resp->data[0] = 0x03;
                resp->length = 1;
                /*
                 * For every successfully handled control
                 * request, set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = 0;
                resp->length = 4;
                /*
                 * For every successfully handled control
                 * request, set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = 1;
                resp->length = 4;
                /*
                 * For every successfully handled control
                 * request, set the request error code to no
                 * error.
                 */
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                /*
                 * We don't support this control, so STALL the
                 * default control ep.
                 */
                resp->length = -EL2HLT;
                /*
                 * For every unsupported control request
                 * set the request error code to appropriate
                 * code.
                 */
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;

        case 2:
            switch (req) {
            case UVC_GET_LEN:
                resp->data[0] = 0x10;
                resp->data[1] = 0x00;
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = 0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = 0xFF;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = len;
                if (sizeof(dev->ex_ctrl) >= resp->length)
                    memcpy(resp->data, dev->ex_ctrl, resp->length);
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = 0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = 1;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;

        case 3:
            switch (req) {
            case UVC_GET_LEN:
                if (!dev->ex_ctrl[1] && !dev->ex_ctrl[2]) {
                    resp->data[0] = 0x02;
                    resp->data[1] = 0x00;
                } else {
                    resp->data[0] = dev->ex_ctrl[1];
                    resp->data[1] = dev->ex_ctrl[2];
                }
                resp->length = 2;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_SET_CUR:
                resp->data[0] = 0x0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MIN:
                resp->data[0] = 0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_MAX:
                resp->data[0] = 0xFF;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_CUR:
                resp->length = len;
                if (sizeof(dev->ex_data) >= resp->length) {
                    //memcpy(resp->data, dev->ex_data, resp->length);
                    //uvc_iq_tool_get_data(dev->ex_ctrl, resp->data, resp->length);
                }
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_INFO:
                resp->data[0] = 0x03;
                resp->length = 1;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_DEF:
                resp->data[0] = 0;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            case UVC_GET_RES:
                resp->data[0] = 1;
                resp->length = len;
                dev->request_error_code.data[0] = 0x00;
                dev->request_error_code.length = 1;
                break;
            default:
                resp->length = -EL2HLT;
                dev->request_error_code.data[0] = 0x07;
                dev->request_error_code.length = 1;
                break;
            }
            break;

        default:
            /*
             * We don't support this control, so STALL the control
             * ep.
             */
            resp->length = -EL2HLT;
            /*
             * If we were not supposed to handle this
             * 'cs', prepare a Request Error Code response.
             */
            dev->request_error_code.data[0] = 0x06;
            dev->request_error_code.length = 1;
            break;
        }
        break;

    default:
        /*
         * If we were not supposed to handle this
         * 'cs', prepare a Request Error Code response.
         */
        dev->request_error_code.data[0] = 0x06;
        dev->request_error_code.length = 1;
        break;

    }
    if (resp->length == -EL2HLT) {
        uvc_log_printf("unsupported: req=%02x, cs=%d, entity_id=%d, len=%d\n",
               req, cs, entity_id, len);
    }
    uvc_log_printf("control request (req %02x cs %02x)\n", req, cs);
}


static void
uvc_events_process_streaming(struct uvc_device *dev, uint8_t req, uint8_t cs,
                             struct uvc_request_data *resp)
{
    struct uvc_streaming_control *ctrl;

    uvc_log_printf("streaming request (req %02x cs %02x)\n", req, cs);

    if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)
        return;

    ctrl = (struct uvc_streaming_control *)&resp->data;
    resp->length = sizeof * ctrl;

    switch (req) {
    case UVC_SET_CUR:
        dev->control = cs;
        resp->length = 34;
        break;

    case UVC_GET_CUR:
        if (cs == UVC_VS_PROBE_CONTROL)
            memcpy(ctrl, &dev->probe, sizeof * ctrl);
        else
            memcpy(ctrl, &dev->commit, sizeof * ctrl);
#if 0
        uvc_log_printf("bmHint: %u\n", ctrl->bmHint);
        uvc_log_printf("bFormatIndex: %u\n", ctrl->bFormatIndex);
        uvc_log_printf("bFrameIndex: %u\n", ctrl->bFrameIndex);
        uvc_log_printf("dwFrameInterval: %u\n", ctrl->dwFrameInterval);
        uvc_log_printf("wKeyFrameRate: %u\n", ctrl->wKeyFrameRate);
        uvc_log_printf("wPFrameRate: %u\n", ctrl->wPFrameRate);
        uvc_log_printf("wCompQuality: %u\n", ctrl->wCompQuality);
        uvc_log_printf("wCompWindowSize: %u\n", ctrl->wCompWindowSize);
        uvc_log_printf("wDelay: %u\n", ctrl->wDelay);
        uvc_log_printf("dwMaxVideoFrameSize: %u\n", ctrl->dwMaxVideoFrameSize);
        uvc_log_printf("dwMaxPayloadTransferSize: %u\n", ctrl->dwMaxPayloadTransferSize);
        uvc_log_printf("dwClockFrequency: %u\n", ctrl->dwClockFrequency);
        uvc_log_printf("bmFramingInfo: %u\n", ctrl->bmFramingInfo);
        uvc_log_printf("bPreferedVersion: %u\n", ctrl->bPreferedVersion);
        uvc_log_printf("bMinVersion: %u\n", ctrl->bMinVersion);
        uvc_log_printf("bMaxVersion: %u\n", ctrl->bMaxVersion);
#endif
        break;

    case UVC_GET_MIN:
    case UVC_GET_MAX:
        uvc_fill_streaming_control(dev, ctrl, req == UVC_GET_MAX ? -1 : 0,
                                   req == UVC_GET_MAX ? -1 : 0, false);
        break;
    case UVC_GET_DEF:
        uvc_fill_streaming_control(dev, ctrl, 0, 0, true);
        break;

    case UVC_GET_RES:
        CLEAR(ctrl);
        break;

    case UVC_GET_LEN:
        resp->data[0] = 0x00;
        resp->data[1] = 0x22;
        resp->length = 2;
        break;

    case UVC_GET_INFO:
        resp->data[0] = 0x03;
        resp->length = 1;
        break;
    }
}

static void
uvc_events_process_class(struct uvc_device *dev, struct usb_ctrlrequest *ctrl,
                         struct uvc_request_data *resp)
{
    if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE)
        return;

    if ((ctrl->wIndex & 0xff) % 2 != get_uvc_streaming_intf() % 2) {
        uvc_events_process_control(dev, ctrl->bRequest,
                                   ctrl->wValue >> 8,
                                   ctrl->wIndex >> 8,
                                   ctrl->wLength, resp);
    } else {
        uvc_events_process_streaming(dev, ctrl->bRequest,
                                     ctrl->wValue >> 8, resp);
    }
}

static void
uvc_events_process_setup(struct uvc_device *dev, struct usb_ctrlrequest *ctrl,
                         struct uvc_request_data *resp)
{
    dev->control = 0;

#ifdef ENABLE_USB_REQUEST_DEBUG
    uvc_log_printf("\nbRequestType %02x bRequest %02x wValue %04x wIndex %04x "
           "wLength %04x\n", ctrl->bRequestType, ctrl->bRequest,
           ctrl->wValue, ctrl->wIndex, ctrl->wLength);
#endif

    switch (ctrl->bRequestType & USB_TYPE_MASK) {
    case USB_TYPE_STANDARD:
        uvc_events_process_standard(dev, ctrl, resp);
        break;

    case USB_TYPE_CLASS:
        uvc_events_process_class(dev, ctrl, resp);
        break;

    default:
        break;
    }
}

static int
uvc_events_process_control_data(struct uvc_device *dev,
                                uint8_t cs, uint8_t entity_id,
                                struct uvc_request_data *data)
{

    unsigned int *val = (unsigned int *)data->data;
    uvc_log_printf(" data = %d, length = %d  , current_cs = %d\n", *val , data->length, dev->cs);
    switch (entity_id) {
        /* Processing unit 'UVC_VC_PROCESSING_UNIT'. */
    case 2:
        switch (cs) {
        case UVC_PU_BRIGHTNESS_CONTROL:
            if (sizeof(dev->brightness_val) >= data->length) {
                memcpy(&dev->brightness_val, data->data, data->length);
                //video_record_set_brightness(*val);
            }
            if (!dev->run_standalone)
                /*
                 * Try to change the Brightness attribute on
                 * Video capture device. Note that this try may
                 * succeed or end up with some error on the
                 * video capture side. By default to keep tools
                 * like USBCV's UVC test suite happy, we are
                 * maintaining a local copy of the current
                 * brightness value in 'dev->brightness_val'
                 * variable and we return the same value to the
                 * Host on receiving a GET_CUR(BRIGHTNESS)
                 * control request.
                 *
                 * FIXME: Keeping in view the point discussed
                 * above, notice that we ignore the return value
                 * from the function call below. To be strictly
                 * compliant, we should return the same value
                 * accordingly.
                 */
                v4l2_set_ctrl(dev->vdev, dev->brightness_val,
                              V4L2_CID_BRIGHTNESS);

            break;
        case UVC_PU_CONTRAST_CONTROL:
            uvc_log_printf("UVC_PU_CONTRAST_CONTROL receive\n");
            if (sizeof(dev->contrast_val) >= data->length) {
                memcpy(&dev->contrast_val, data->data, data->length);
                //video_record_set_time(dev->contrast_val);
                uvc_log_printf("UVC_PU_CONTRAST_CONTROL: 0x%02x 0x%02x\n",
                        data->data[0], data->data[1]);
                //video_record_set_contrast(*val);
            }
            break;
        case UVC_PU_HUE_CONTROL:
            if (sizeof(dev->hue_val) >= data->length) {
                memcpy(&dev->hue_val, data->data, data->length);
                //video_record_set_hue(*val);
            }
            break;
        case UVC_PU_SATURATION_CONTROL:
            if (sizeof(dev->saturation_val) >= data->length) {
                memcpy(&dev->saturation_val, data->data, data->length);
                //video_record_set_saturation(*val);
            }
            break;
        case UVC_PU_SHARPNESS_CONTROL:
            if (sizeof(dev->sharpness_val) >= data->length)
                memcpy(&dev->sharpness_val, data->data, data->length);
            break;
        case UVC_PU_GAMMA_CONTROL:
            if (sizeof(dev->gamma_val) >= data->length)
                memcpy(&dev->gamma_val, data->data, data->length);
            break;
        case UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL:
            /* 0:auto, 1:Daylight 2:fluocrescence 3:cloudysky 4:tungsten */
            if (sizeof(dev->white_balance_temperature_val) >= data->length) {
                memcpy(&dev->white_balance_temperature_val, data->data, data->length);
                //api_set_white_balance(*val / 51);
            }
            break;
        case UVC_PU_GAIN_CONTROL:
            if (sizeof(dev->gain_val) >= data->length)
                memcpy(&dev->gain_val, data->data, data->length);
            break;
        case UVC_PU_HUE_AUTO_CONTROL:
            if (sizeof(dev->hue_auto_val) >= data->length)
                memcpy(&dev->hue_auto_val, data->data, data->length);
            break;
        case UVC_PU_POWER_LINE_FREQUENCY_CONTROL:
            if (sizeof(dev->power_line_frequency_val) >= data->length) {
                memcpy(&dev->power_line_frequency_val, data->data, data->length);
                //video_record_set_power_line_frequency(*val);
            }
            break;
        default:
            break;
        }

        break;

    case 6:
        switch (cs) {
        case 1:
            if (sizeof(dev->extension_io_data) >= data->length) {
                memcpy(dev->extension_io_data, data->data, data->length);
                uvc_log_printf("extension ctrl 1 set cur data: 0x%02x\n", dev->extension_io_data[0]);
            }
            break;

        case 2:
            if (sizeof(dev->ex_ctrl) >= data->length) {
                memcpy(dev->ex_ctrl, data->data, data->length);
                uvc_log_printf("extension control: 0x%02x 0x%02x 0x%02x\n",
                       dev->ex_ctrl[0], dev->ex_ctrl[1], dev->ex_ctrl[2]);
                //if (dev->ex_ctrl[0] == 0xc5)
                //    video_record_get_flt_parameter(dev->ex_ctrl[3], dev->ex_ctrl[4]);
            }
            break;

        case 3:
            if (sizeof(dev->ex_data) >= data->length) {
                memcpy(dev->ex_data, data->data, data->length);
                //uvc_iq_tool_set_data(data->data, data->length);
                uvc_log_printf("extension data: 0x%02x 0x%02x\n", dev->ex_data[0], dev->ex_data[1]);
            }
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }
    uvc_log_printf("Control Request data phase (cs %02x  data %d entity %02x)\n", cs, *val, entity_id);
    return 0;
}

static int
uvc_events_process_data(struct uvc_device *dev, struct uvc_request_data *data)
{
    struct uvc_streaming_control *target;
    struct uvc_streaming_control *ctrl;
    struct v4l2_format fmt;
    const struct uvc_format_info *format;
    const struct uvc_frame_info *frame;
    const unsigned int *interval;
    unsigned int iformat, iframe;
    unsigned int nframes;
    //unsigned int *val = (unsigned int *)data->data;
    int ret = 0;

    switch (dev->control) {
    case UVC_VS_PROBE_CONTROL:
        uvc_log_printf("setting probe control, length = %d\n", data->length);
        target = &dev->probe;
        break;

    case UVC_VS_COMMIT_CONTROL:
        uvc_log_printf("setting commit control, length = %d\n", data->length);
        target = &dev->commit;
        break;

    default:
        uvc_log_printf("setting unknown control, length = %d\n", data->length);

        uvc_log_printf("cs: %u, entity_id: %u\n", dev->cs, dev->entity_id);
        ret = uvc_events_process_control_data(dev,
                                              dev->cs,
                                              dev->entity_id, data);
        if (ret < 0)
            goto err;

        return 0;
    }

    ctrl = (struct uvc_streaming_control *)&data->data;

    /* The application configuration is authoritative. Rewrite the host's
     * probe/commit request before selecting the format and before any buffers
     * or encoder are initialized, so USB descriptors and payloads agree. */
    {
        int cfg_w, cfg_h, cfg_fcc, cfg_fps;
        unsigned int uf, ui;

        if (uvc_control_get_default_config(&cfg_w, &cfg_h, &cfg_fcc, &cfg_fps)) {
            if (!uvc_find_frame((unsigned int)cfg_w, (unsigned int)cfg_h,
                                (unsigned int)cfg_fcc, &uf, &ui)) {
                uvc_log_printf("UVC: configured format is not advertised\n");
                return -EINVAL;
            }
            uvc_log_printf("UVC: overriding host request with configured format %c%c%c%c %dx%d @ %dfps\n",
                           pixfmtstr(cfg_fcc), cfg_w, cfg_h, cfg_fps);
            ctrl->bFormatIndex = uf + 1;
            ctrl->bFrameIndex = ui + 1;
            ctrl->dwFrameInterval =
                uvc_pick_interval(&uvc_formats[uf].frames[ui], cfg_fps);
        }
    }

    iformat = clamp((unsigned int)ctrl->bFormatIndex, 1U,
                    (unsigned int)ARRAY_SIZE(uvc_formats));
    format = &uvc_formats[iformat - 1];

    nframes = 0;
    while (format->frames[nframes].width != 0)
        ++nframes;

    iframe = clamp((unsigned int)ctrl->bFrameIndex, 1U, nframes);
    frame = &format->frames[iframe - 1];
    interval = frame->intervals;

    while (interval[0] < ctrl->dwFrameInterval && interval[1])
        ++interval;

    target->bFormatIndex = iformat;
    target->bFrameIndex = iframe;
    switch (format->fcc) {
    case V4L2_PIX_FMT_YUYV:
        target->dwMaxVideoFrameSize = frame->width * frame->height * 2;
        break;
    case V4L2_PIX_FMT_MJPEG:
    case V4L2_PIX_FMT_H264:
        if (dev->imgsize == 0)
            uvc_log_printf("WARNING: MJPEG/h.264 requested and no image loaded.\n");
        dev->width = frame->width;
        dev->height = frame->height;
        dev->imgsize = frame->width * frame->height * 2/*1.5*/;
        uvc_log_printf("uvc_events_process_data:format->fcc:%d,dev->width:%d,dev->imgsize:%d\n",
                format->fcc,dev->width,dev->imgsize);
        target->dwMaxVideoFrameSize = dev->imgsize;
        break;
    }
    target->dwFrameInterval = *interval;

    if (dev->control == UVC_VS_COMMIT_CONTROL) {
        if (!uvc_get_user_run_state(dev->video_id))
            return -EINTR;

        if (uvc_video_get_uvc_process(dev->video_id) && dev->is_streaming)
            return 0;
        if (uvc_video_get_uvc_process(dev->video_id)) {
            uvc_buffer_deinit(dev->video_id);
            uvc_video_release_buffers(dev);
        }
        /* uvc_set_config() has already selected format/frame above. Keep the
         * committed device state tied to that selection instead of allowing a
         * host request to change the producer configuration. */
        dev->fcc = format->fcc;
        dev->width = frame->width;
        dev->height = frame->height;
        dev->fps = 10000000 / target->dwFrameInterval;
        uvc_log_printf("UVC: committed format %c%c%c%c %ux%u @ %dfps\n",
                       pixfmtstr(dev->fcc), dev->width, dev->height, dev->fps);

        /*
         * Try to set the default format at the V4L2 video capture
         * device as requested by the user.
         */
        CLEAR(fmt);

        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        fmt.fmt.pix.width = frame->width;
        fmt.fmt.pix.height = frame->height;
        fmt.fmt.pix.pixelformat = format->fcc;

        switch (format->fcc) {
        case V4L2_PIX_FMT_YUYV:
            fmt.fmt.pix.sizeimage = (fmt.fmt.pix.width * fmt.fmt.pix.height * 2);
            break;
        case V4L2_PIX_FMT_MJPEG:
        case V4L2_PIX_FMT_H264:
            fmt.fmt.pix.sizeimage = (fmt.fmt.pix.width * fmt.fmt.pix.height * 2/*1.5*/);//dev->imgsize;
            break;
        }

        uvc_set_user_resolution(fmt.fmt.pix.width, fmt.fmt.pix.height, dev->video_id);
        uvc_set_user_fcc(fmt.fmt.pix.pixelformat, dev->video_id);
        if (uvc_buffer_init(dev->video_id))
            goto err;

        /*
         * As per the new commit command received from the UVC host
         * change the current format selection at both UVC and V4L2
         * sides.
         */
        ret = uvc_video_set_format(dev);
        if (ret < 0)
            goto err;

        if (!dev->run_standalone) {
            /* UVC - V4L2 integrated path. */
            ret = v4l2_set_format(dev->vdev, &fmt);
            if (ret < 0)
                goto err;
        }

        if (dev->bulk) {
            if (!uvc_get_user_run_state(dev->video_id))
                goto err;
            ret = uvc_handle_streamon_event(dev);
            if (ret < 0)
                goto err;
        }
    }

    return 0;

err:
    uvc_buffer_deinit(dev->video_id);
    uvc_video_release_buffers(dev);
    return ret;
}

static void uvc_events_handle_connect(struct uvc_device *dev)
{
    pthread_mutex_lock(&dev->dmabuf_lock);
    dev->uvc_shutdown_requested = 0;
    pthread_mutex_unlock(&dev->dmabuf_lock);
    uvc_log_printf("%d: UVC: host connected\n", dev->video_id);
}

static void uvc_events_handle_disconnect(struct uvc_device *dev)
{
    pthread_mutex_lock(&dev->dmabuf_lock);
    dev->uvc_shutdown_requested = 1;
    dev->dmabuf_ready = false;
    pthread_mutex_unlock(&dev->dmabuf_lock);

    uvc_log_printf("%d: UVC: host disconnected\n", dev->video_id);
    uvc_video_release_buffers(dev);
    uvc_buffer_deinit(dev->video_id);
}

static void uvc_events_handle_streamoff(struct uvc_device *dev)
{
    if (!dev->run_standalone && dev->vdev && dev->vdev->is_streaming) {
        v4l2_stop_capturing(dev->vdev);
        dev->vdev->is_streaming = 0;
    }

    uvc_video_release_buffers(dev);
    uvc_buffer_deinit(dev->video_id);
}

static void
uvc_events_process(struct uvc_device *dev)
{
    struct v4l2_event v4l2_event;
    struct uvc_event *uvc_event = (void *)&v4l2_event.u.data;
    struct uvc_request_data resp;
    int ret = 0;

    ret = ioctl(dev->uvc_fd, VIDIOC_DQEVENT, &v4l2_event);
    if (ret < 0) {
        int error = errno;

        if (uvc_video_is_link_error(error)) {
            uvc_video_usb_link_lost(dev, "VIDIOC_DQEVENT", error);
            return;
        }
        if (error != EAGAIN)
            uvc_log_printf("VIDIOC_DQEVENT failed: %s (%d)\n", strerror(error),
                   error);
        return;
    }

    memset(&resp, 0, sizeof resp);
    resp.length = -EL2HLT;

    switch (v4l2_event.type) {
    case UVC_EVENT_CONNECT:
        uvc_events_handle_connect(dev);
        return;

    case UVC_EVENT_DISCONNECT:
        uvc_events_handle_disconnect(dev);
        return;

    case UVC_EVENT_SETUP:
        uvc_events_process_setup(dev, &uvc_event->req, &resp);
        break;

    case UVC_EVENT_DATA:
        ret = uvc_events_process_data(dev, &uvc_event->data);
        if (ret < 0)
            break;

        return;

    case UVC_EVENT_STREAMON:
        if (!dev->bulk)
            uvc_handle_streamon_event(dev);
        return;

    case UVC_EVENT_STREAMOFF:
        uvc_events_handle_streamoff(dev);
        return;
    }

    ret = ioctl(dev->uvc_fd, UVCIOC_SEND_RESPONSE, &resp);
    if (ret < 0) {
        uvc_log_printf("UVCIOC_S_EVENT failed: %s (%d)\n", strerror(errno),
               errno);
        return;
    }
}

static void
uvc_events_init(struct uvc_device *dev)
{
    struct v4l2_event_subscription sub;
    unsigned int payload_size;

    switch (dev->fcc) {
    case V4L2_PIX_FMT_YUYV:
        payload_size = dev->width * dev->height * 2;
        break;
    case V4L2_PIX_FMT_MJPEG:
    case V4L2_PIX_FMT_H264:
        payload_size = dev->imgsize;
        break;
    default:
        return;
    }

    uvc_fill_streaming_control(dev, &dev->probe, 0, 0, true);
    uvc_fill_streaming_control(dev, &dev->commit, 0, 0, true);

    if (dev->bulk)
        /* FIXME Crude hack, must be negotiated with the driver. */
        dev->probe.dwMaxPayloadTransferSize =
            dev->commit.dwMaxPayloadTransferSize = payload_size;

    memset(&sub, 0, sizeof sub);
    sub.type = UVC_EVENT_SETUP;
    ioctl(dev->uvc_fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_DATA;
    ioctl(dev->uvc_fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_STREAMON;
    ioctl(dev->uvc_fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_STREAMOFF;
    ioctl(dev->uvc_fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
}

/* ---------------------------------------------------------------------------
 * main
 */

static void
image_load(struct uvc_device *dev, const char *img)
{
    int fd = -1;

    if (img == NULL)
        return;

    fd = open(img, O_RDONLY);
    if (fd == -1) {
        uvc_log_printf("Unable to open MJPEG image '%s'\n", img);
        return;
    }

    dev->imgsize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    dev->imgdata = malloc(dev->imgsize);
    if (dev->imgdata == NULL) {
        uvc_log_printf("Unable to allocate memory for MJPEG image\n");
        dev->imgsize = 0;
        return;
    }

    read(fd, dev->imgdata, dev->imgsize);
    close(fd);
}

/*
static void usage(const char *argv0)
{
    uvc_log_fprintf(stderr, "Usage: %s [options]\n", argv0);
    uvc_log_fprintf(stderr, "Available options are\n");
    uvc_log_fprintf(stderr, " -b		Use bulk mode\n");
    uvc_log_fprintf(stderr, " -d		Do not use any real V4L2 capture device\n");
    uvc_log_fprintf(stderr, " -f <format>    Select frame format\n\t"
            "0 = V4L2_PIX_FMT_YUYV\n\t"
            "1 = V4L2_PIX_FMT_MJPEG\n\t"
            "2 = V4L2_PIX_FMT_H264\n");
    uvc_log_fprintf(stderr, " -h		Print this help screen and exit\n");
    uvc_log_fprintf(stderr, " -i image	MJPEG image\n");
    uvc_log_fprintf(stderr, " -m		Streaming mult for ISOC (b/w 0 and 2)\n");
    uvc_log_fprintf(stderr, " -n		Number of Video buffers (b/w 2 and 32)\n");
    uvc_log_fprintf(stderr, " -o <IO method> Select UVC IO method:\n\t"
            "0 = MMAP\n\t"
            "1 = USER_PTR\n");
    uvc_log_fprintf(stderr, " -r <resolution> Select frame resolution:\n\t"
            "0 = 360p, VGA (640x360)\n\t"
            "1 = 720p, WXGA (1280x720)\n");
    uvc_log_fprintf(stderr, " -s <speed>	Select USB bus speed (b/w 0 and 2)\n\t"
            "0 = Full Speed (FS)\n\t"
            "1 = High Speed (HS)\n\t"
            "2 = Super Speed (SS)\n");
    uvc_log_fprintf(stderr, " -t		Streaming burst (b/w 0 and 15)\n");
    uvc_log_fprintf(stderr, " -u device	UVC Video Output device\n");
    uvc_log_fprintf(stderr, " -v device	V4L2 Video Capture device\n");

}
*/

int uvc_gadget_v4l2_open(struct v4l2_device **vdev, char *devname,
                         struct v4l2_format *fmt)
{
    return v4l2_open(vdev, devname, fmt);
}

void uvc_gadget_v4l2_close(struct v4l2_device *dev)
{
    v4l2_close(dev);
}

int uvc_gadget_v4l2_reqbufs(struct v4l2_device *dev, int nbufs)
{
    return v4l2_reqbufs(dev, nbufs);
}

void uvc_gadget_v4l2_process_data(struct v4l2_device *dev)
{
    v4l2_process_data(dev);
}

void uvc_gadget_v4l2_stop_stream(struct v4l2_device *dev)
{
    if (!dev || !dev->is_streaming)
        return;
    v4l2_stop_capturing(dev);
    v4l2_uninit_device(dev);
    v4l2_reqbufs(dev, 0);
    dev->is_streaming = 0;
}

int uvc_gadget_uvc_open(struct uvc_device **udev, char *devname)
{
    return uvc_open(udev, devname);
}

void uvc_gadget_uvc_close(struct uvc_device *dev)
{
    uvc_close(dev);
}

void uvc_gadget_events_init(struct uvc_device *dev)
{
    uvc_events_init(dev);
}

void uvc_gadget_events_process(struct uvc_device *dev)
{
    uvc_events_process(dev);
}

void uvc_gadget_video_process(struct uvc_device *dev)
{
    uvc_video_process(dev);
}

int uvc_gadget_uvc_stop_stream(struct uvc_device *dev)
{
    if (!dev)
        return 0;

    uvc_video_release_buffers(dev);
    return 0;
}

int uvc_gadget_uvc_release_buffers(struct uvc_device *dev)
{
    uvc_video_release_buffers(dev);
    return 0;
}

int uvc_gadget_force_uvc_node_idle(int video_id)
{
    char devname[32];
    int fd;
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    struct v4l2_requestbuffers rb;
    int ret = 0;

    snprintf(devname, sizeof(devname), "/dev/video%d", video_id);
    fd = open(devname, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return -errno;

    ioctl(fd, VIDIOC_STREAMOFF, &type);

    CLEAR(rb);
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_MMAP;
    rb.count = 0;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0)
        ret = -errno;

    close(fd);
    return ret;
}

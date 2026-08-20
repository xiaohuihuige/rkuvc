/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>

#include "uvc_control.h"
#include "uvc_log.h"
#include "uvc_encode.h"
#include "uvc_video.h"
#include "uvc_app.h"
#include "uvc_gadget_internal.h"

#define UVC_STREAMING_INTF_PATH \
    "/sys/kernel/config/usb_gadget/rockchip/functions/uvc.gs6/streaming/bInterfaceNumber"

struct uvc_ctrl {
    int id;
    int width;
    int height;
    int fps;
};

struct uvc_control_module {
    struct uvc_ctrl ctrl[2];
    struct uvc_encode enc;
    pthread_mutex_t lock;
    int streaming_intf;
    uvc_open_camera_callback open_cb;
    void *open_ud;
    uvc_close_camera_callback close_cb;
    void *close_ud;
    uvc_release_buffer_callback release_cb;
    void *release_ud;
    bool streaming_active;
    /** open_cb was invoked and close_cb not yet run (host was streaming). */
    bool camera_user_open;
    bool camera_closing;
    /** Default fmt/w/h/fps selected via uvc_set_config(); .configured=false => defaults. */
    struct {
        int width;
        int height;
        unsigned int fcc;
        int fps;
        bool configured;
    } user_cfg;
};

static struct uvc_control_module *ctl_mod;
static struct uvc_app *ctl_app;

static struct uvc_control_module *control_require(void)
{
    return ctl_mod;
}

int uvc_control_module_open(void)
{
    struct uvc_control_module *m;

    if (ctl_mod)
        return 0;

    m = calloc(1, sizeof(*m));
    if (!m)
        return -1;

    pthread_mutex_init(&m->lock, NULL);
    m->streaming_intf = -1;

    /* Always start with an application-owned format. uvc_set_config() can
     * replace it, but a missing call must never make the host request the
     * producer configuration. */
    m->user_cfg.width = 1280;
    m->user_cfg.height = 720;
    m->user_cfg.fcc = V4L2_PIX_FMT_MJPEG;
    m->user_cfg.fps = 30;
    m->user_cfg.configured = true;
    ctl_mod = m;
    return 0;
}

void uvc_control_module_close(void)
{
    if (!ctl_mod)
        return;

    uvc_control_exit();
    pthread_mutex_destroy(&ctl_mod->lock);
    free(ctl_mod);
    ctl_mod = NULL;
}

void register_uvc_open_camera(uvc_open_camera_callback cb, void *userdata)
{
    if (uvc_control_module_open() != 0)
        return;

    pthread_mutex_lock(&ctl_mod->lock);
    ctl_mod->open_cb = cb;
    ctl_mod->open_ud = userdata;
    pthread_mutex_unlock(&ctl_mod->lock);
}

void register_uvc_close_camera(uvc_close_camera_callback cb, void *userdata)
{
    if (uvc_control_module_open() != 0)
        return;

    pthread_mutex_lock(&ctl_mod->lock);
    ctl_mod->close_cb = cb;
    ctl_mod->close_ud = userdata;
    pthread_mutex_unlock(&ctl_mod->lock);
}

void register_uvc_release_buffer(uvc_release_buffer_callback cb, void *userdata)
{
    if (uvc_control_module_open() != 0)
        return;

    pthread_mutex_lock(&ctl_mod->lock);
    ctl_mod->release_cb = cb;
    ctl_mod->release_ud = userdata;
    pthread_mutex_unlock(&ctl_mod->lock);
}

void uvc_control_release_buffer(int dmabuf_fd)
{
    uvc_release_buffer_callback cb;
    void *userdata;

    if (!ctl_mod)
        return;

    pthread_mutex_lock(&ctl_mod->lock);
    cb = ctl_mod->release_cb;
    userdata = ctl_mod->release_ud;
    pthread_mutex_unlock(&ctl_mod->lock);

    if (cb)
        cb(userdata, dmabuf_fd);
}

static unsigned int uvc_fmt_to_fcc(int fmt)
{
    switch (fmt) {
    case UVC_FMT_YUYV:  return V4L2_PIX_FMT_YUYV;
    case UVC_FMT_MJPEG: return V4L2_PIX_FMT_MJPEG;
    case UVC_FMT_H264:  return V4L2_PIX_FMT_H264;
    default:            return 0;
    }
}

int uvc_set_config(int h, int w, int fmt, int fps)
{
    struct uvc_control_module *m;
    unsigned int fcc;

    if (uvc_control_module_open() != 0)
        return -1;
    m = control_require();

    fcc = uvc_fmt_to_fcc(fmt);
    if (fcc == 0 || w <= 0 || h <= 0 || fps <= 0)
        return -1;

    if (!uvc_gadget_config_supported(fcc, (unsigned int)w, (unsigned int)h, fps)) {
        uvc_log_printf("uvc_set_config: fmt=%d %dx%d@%dfps not supported\n",
               fmt, w, h, fps);
        return -1;
    }

    pthread_mutex_lock(&m->lock);
    if (m->streaming_active || m->camera_user_open || m->camera_closing) {
        pthread_mutex_unlock(&m->lock);
        uvc_log_printf("uvc_set_config: cannot change while streaming\n");
        return -1;
    }
    m->user_cfg.width = w;
    m->user_cfg.height = h;
    m->user_cfg.fcc = fcc;
    m->user_cfg.fps = fps;
    m->user_cfg.configured = true;
    pthread_mutex_unlock(&m->lock);

    uvc_log_printf("uvc_set_config: default set to fmt=%d %dx%d@%dfps\n",
           fmt, w, h, fps);
    return 0;
}

uvc_config_t uvc_get_sys_config(void)
{
    uvc_config_t cfg;

    uvc_gadget_fill_sys_config(&cfg);
    return cfg;
}

bool uvc_control_get_default_config(int *w, int *h, int *fcc, int *fps)
{
    struct uvc_control_module *m = control_require();
    bool configured;

    if (!m)
        return false;

    pthread_mutex_lock(&m->lock);
    configured = m->user_cfg.configured;
    if (configured) {
        if (w) *w = m->user_cfg.width;
        if (h) *h = m->user_cfg.height;
        if (fcc) *fcc = (int)m->user_cfg.fcc;
        if (fps) *fps = m->user_cfg.fps;
    }
    pthread_mutex_unlock(&m->lock);
    return configured;
}

static bool is_uvc_video_name(const char *buf)
{
    return strstr(buf, "usb") || strstr(buf, "gadget");
}

static void query_streaming_intf(struct uvc_control_module *m)
{
    int fd;

    fd = open(UVC_STREAMING_INTF_PATH, O_RDONLY);
    if (fd < 0)
        return;

    {
        char intf[32] = {0};
        ssize_t n;

        n = read(fd, intf, sizeof(intf) - 1);
        if (n > 0) {
            intf[n] = '\0';
            m->streaming_intf = atoi(intf);
            uvc_log_printf("uvc_streaming_intf = %d\n", m->streaming_intf);
        }
    }
    close(fd);
}

int get_uvc_streaming_intf(void)
{
    struct uvc_control_module *m = control_require();

    return m ? m->streaming_intf : -1;
}

static int scan_max_video_node(void)
{
    const char *dir_path = "/sys/class/video4linux/";
    struct dirent *entry;
    int max_video = -1;
    DIR *dir = opendir(dir_path);

    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "video", 5) == 0) {
            int n = atoi(entry->d_name + 5);

            if (n > max_video)
                max_video = n;
        }
    }
    closedir(dir);
    return max_video;
}

static int probe_video_node(int id, struct uvc_control_module *m, int *found)
{
    char path[128];
    char name[128];
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), "/sys/class/video4linux/video%d/name", id);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    n = read(fd, name, sizeof(name) - 1);
    close(fd);
    if (n <= 0)
        return 0;

    name[n] = '\0';
    if (is_uvc_video_name(name)) {
        (*found)++;
        if (m->ctrl[1].id < 0)
            m->ctrl[1].id = id;
        else if (m->ctrl[0].id < 0)
            m->ctrl[0].id = id;
    }
    return 0;
}

int check_uvc_video_id(void)
{
    struct uvc_control_module *m = control_require();
    int max;
    int uvc_cnt = 1;
    int find_cnt = 0;
    int i;

    if (!m)
        return -1;

    if (getenv("UVC_CNT"))
        uvc_cnt = atoi(getenv("UVC_CNT"));

    memset(m->ctrl, 0, sizeof(m->ctrl));
    m->ctrl[0].id = m->ctrl[1].id = -1;

    max = scan_max_video_node();
    if (max < 0)
        return -1;

    for (i = max; i >= 0; i--) {
        probe_video_node(i, m, &find_cnt);
        if (find_cnt >= uvc_cnt)
            break;
    }

    if (m->ctrl[0].id < 0 && m->ctrl[1].id < 0)
        return -1;

    if (m->ctrl[0].id < 0 && m->ctrl[1].id >= 0) {
        m->ctrl[0].id = m->ctrl[1].id;
        m->ctrl[1].id = -1;
    }

    query_streaming_intf(m);
    return 0;
}

int uvc_control_video_id(unsigned seq)
{
    struct uvc_control_module *m = control_require();

    if (!m || seq > 1)
        return -1;
    return m->ctrl[seq].id;
}

void add_uvc_video(void)
{
    struct uvc_control_module *m = control_require();

    if (!m)
        return;

    if (m->ctrl[0].id >= 0)
        uvc_video_id_add(m->ctrl[0].id);
    if (m->ctrl[1].id >= 0)
        uvc_video_id_add(m->ctrl[1].id);
}

void uvc_control_init(int width, int height, int fcc, int fps)
{
    struct uvc_control_module *m = control_require();
    uvc_open_camera_callback open_cb;
    uvc_close_camera_callback close_cb;
    void *open_ud;
    void *close_ud;
    bool active;
    bool closing;
    int ret;

    if (!m)
        return;

    pthread_mutex_lock(&m->lock);
    active = m->streaming_active || m->camera_user_open;
    closing = m->camera_closing;
    pthread_mutex_unlock(&m->lock);

    if (active)
        uvc_control_exit();
    if (closing)
        return;

    pthread_mutex_lock(&m->lock);
    memset(&m->enc, 0, sizeof(m->enc));
    /* The application configuration is authoritative. The gadget commit
     * handler rewrites the host request to this setting before stream-on. */
    if (m->user_cfg.configured) {
        width = m->user_cfg.width;
        height = m->user_cfg.height;
        fcc = m->user_cfg.fcc;
        fps = m->user_cfg.fps;
    }
    if (uvc_encode_init(&m->enc, width, height, fcc)) {
        uvc_log_printf("%s: encoder init failed\n", __func__);
        pthread_mutex_unlock(&m->lock);
        uvc_set_user_run_state(false, uvc_video_id_get(0));
        return;
    }
    m->streaming_active = true;
    open_cb = m->open_cb;
    open_ud = m->open_ud;
    close_cb = m->close_cb;
    close_ud = m->close_ud;
    pthread_mutex_unlock(&m->lock);

    if (open_cb) {
        ret = open_cb(open_ud, width, height, fcc, fps);
        if (ret == 0) {
            pthread_mutex_lock(&m->lock);
            m->camera_user_open = true;
            pthread_mutex_unlock(&m->lock);
        } else {
            uvc_log_printf("%s: camera open failed: %d\n", __func__, ret);
            if (close_cb)
                close_cb(close_ud);
            pthread_mutex_lock(&m->lock);
            uvc_encode_exit(&m->enc);
            memset(&m->enc, 0, sizeof(m->enc));
            m->streaming_active = false;
            pthread_mutex_unlock(&m->lock);
            uvc_set_user_run_state(false, uvc_video_id_get(0));
        }
    }
}

void uvc_control_exit(void)
{
    struct uvc_control_module *m = control_require();
    uvc_close_camera_callback close_cb = NULL;
    void *close_ud = NULL;
    bool close_camera = false;

    if (!m)
        return;

    pthread_mutex_lock(&m->lock);
    if (m->camera_closing || (!m->streaming_active && !m->camera_user_open)) {
        pthread_mutex_unlock(&m->lock);
        return;
    }
    m->camera_closing = true;
    close_camera = m->camera_user_open;
    m->camera_user_open = false;
    close_cb = m->close_cb;
    close_ud = m->close_ud;
    pthread_mutex_unlock(&m->lock);

    if (close_camera && close_cb)
        close_cb(close_ud);

    pthread_mutex_lock(&m->lock);
    if (m->streaming_active) {
        uvc_encode_exit(&m->enc);
        memset(&m->enc, 0, sizeof(m->enc));
        m->streaming_active = false;
    }
    m->camera_closing = false;
    pthread_mutex_unlock(&m->lock);
}

bool uvc_control_host_streaming(void)
{
    struct uvc_control_module *m = control_require();
    bool streaming;

    if (!m)
        return false;

    pthread_mutex_lock(&m->lock);
    streaming = m->streaming_active || m->camera_user_open || m->camera_closing;
    pthread_mutex_unlock(&m->lock);
    return streaming;
}

void uvc_read_camera_buffer(void *cam_buf, int cam_fd, size_t cam_size,
                            void *extra_data, size_t extra_size)
{
    struct uvc_control_module *m = control_require();
    struct uvc_encode enc;
    bool process = false;

    if (!m)
        return;

    pthread_mutex_lock(&m->lock);
    /* VENC allocates compressed MJPEG/H264 buffers up to 4 bytes/pixel;
     * the old 2-byte limit silently discarded valid encoded frames. */
    size_t max_size = 0;
    if (m->enc.fcc == V4L2_PIX_FMT_YUYV)
        max_size = (size_t)m->enc.width * m->enc.height * 2;
    else
        max_size = (size_t)m->enc.width * m->enc.height * 4;
    if (m->streaming_active && m->enc.width > 0 && m->enc.height > 0 &&
        cam_size <= max_size) {
        enc = m->enc;
        enc.video_id = uvc_video_id_get(0);
        enc.extra_data = extra_data;
        enc.extra_size = extra_size;
        process = true;
    }
    pthread_mutex_unlock(&m->lock);

    if (process)
        uvc_encode_process(&enc, cam_buf, cam_fd, cam_size);
    else
        uvc_log_printf("uvc_read_camera_buffer: drop frame size=%zu (max=%zu, streaming=%d)\n",
                       cam_size, max_size, m->streaming_active);
}

int uvc_control_run(uint32_t flags)
{
    uint32_t app_flags = 0;

    if (uvc_control_module_open() != 0)
        return -1;

    ctl_app = uvc_app_create();
    if (!ctl_app)
        return -1;

    if (flags & UVC_CONTROL_CHECK_STRAIGHT)
        app_flags |= UVC_APP_FLAG_CHECK_STRAIGHT;

    return uvc_app_run(ctl_app, app_flags);
}

void uvc_control_join(uint32_t flags)
{
    (void)flags;

    if (ctl_app) {
        uvc_app_destroy(ctl_app);
        ctl_app = NULL;
    }
    uvc_control_module_close();
}

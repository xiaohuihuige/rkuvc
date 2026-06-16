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
#include "uvc_encode.h"
#include "uvc_video.h"
#include "uvc_app.h"

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
    bool streaming_active;
    /** open_cb was invoked and close_cb not yet run (host was streaming). */
    bool camera_user_open;
};

static struct uvc_control_module *ctl_mod;
static struct uvc_app *ctl_app;

static struct uvc_control_module *control_require(void)
{
    return ctl_mod;
}

int uvc_control_module_open(void)
{
    if (ctl_mod)
        return 0;

    ctl_mod = calloc(1, sizeof(*ctl_mod));
    if (!ctl_mod)
        return -1;

    pthread_mutex_init(&ctl_mod->lock, NULL);
    ctl_mod->streaming_intf = -1;
    return 0;
}

void uvc_control_module_close(void)
{
    if (!ctl_mod)
        return;

    pthread_mutex_destroy(&ctl_mod->lock);
    free(ctl_mod);
    ctl_mod = NULL;
}

void register_uvc_open_camera(uvc_open_camera_callback cb, void *userdata)
{
    if (uvc_control_module_open() != 0)
        return;

    ctl_mod->open_cb = cb;
    ctl_mod->open_ud = userdata;
}

void register_uvc_close_camera(uvc_close_camera_callback cb, void *userdata)
{
    if (uvc_control_module_open() != 0)
        return;

    ctl_mod->close_cb = cb;
    ctl_mod->close_ud = userdata;
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

        read(fd, intf, sizeof(intf) - 1);
        m->streaming_intf = atoi(intf);
        printf("uvc_streaming_intf = %d\n", m->streaming_intf);
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

    if (!m)
        return;

    if (m->streaming_active)
        uvc_control_exit();

    pthread_mutex_lock(&m->lock);
    memset(&m->enc, 0, sizeof(m->enc));
    if (uvc_encode_init(&m->enc, width, height, fcc)) {
        printf("%s fail!\n", __func__);
        abort();
    }
    m->streaming_active = true;
    pthread_mutex_unlock(&m->lock);

    if (m->open_cb) {
        m->camera_user_open = true;
        m->open_cb(m->open_ud, width, height, fcc, fps);
    }
}

void uvc_control_exit(void)
{
    struct uvc_control_module *m = control_require();

    if (!m || (!m->streaming_active && !m->camera_user_open))
        return;

    if (m->close_cb)
        m->close_cb(m->close_ud);
    m->camera_user_open = false;

    pthread_mutex_lock(&m->lock);
    if (m->streaming_active) {
        uvc_encode_exit(&m->enc);
        memset(&m->enc, 0, sizeof(m->enc));
        m->streaming_active = false;
    }
    pthread_mutex_unlock(&m->lock);
}

bool uvc_control_host_streaming(void)
{
    struct uvc_control_module *m = control_require();

    if (!m)
        return false;

    return m->streaming_active || m->camera_user_open;
}

void uvc_read_camera_buffer(void *cam_buf, int cam_fd, size_t cam_size,
                            void *extra_data, size_t extra_size)
{
    struct uvc_control_module *m = control_require();

    if (!m)
        return;

    pthread_mutex_lock(&m->lock);
    if (cam_size <= (size_t)m->enc.width * m->enc.height * 2) {
        m->enc.video_id = uvc_video_id_get(0);
        m->enc.extra_data = extra_data;
        m->enc.extra_size = extra_size;
        uvc_encode_process(&m->enc, cam_buf, cam_fd, cam_size);
    }
    pthread_mutex_unlock(&m->lock);
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

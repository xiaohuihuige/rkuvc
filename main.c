#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <pthread.h>
#include "uvc_control.h"
#include "uvc_video.h"
#include "drm.h"

#define ALIGN(x, a)         (((x)+(a)-1)&~((a)-1))

static void fill_nv12(uint8_t *buf, uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride) {
    uint8_t *buf_y = buf;
    uint32_t x, y;
    const uint32_t frame_count = 0;

    uint8_t *p = buf_y;

    for (y = 0; y < height; y++, p += hor_stride) {
        for (x = 0; x < width; x++) {
            p[x] = x + y + frame_count * 3;
        }
    }

    p = buf + hor_stride * ver_stride;
    for (y = 0; y < height / 2; y++, p += hor_stride) {
        for (x = 0; x < width / 2; x++) {
            p[x * 2 + 0] = 128 + y + frame_count * 2;
            p[x * 2 + 1] = 64  + x + frame_count * 5;
        }
    }
}

static void fill_yuyv(uint8_t *buf, uint32_t width, uint32_t height)
{
    uint32_t x, y;

    for (y = 0; y < height; ++y) {
        uint8_t *line = buf + y * width * 2;
        for (x = 0; x < width; x += 2) {
            line[x * 2 + 0] = (uint8_t)(x + y);
            line[x * 2 + 1] = 128;
            line[x * 2 + 2] = (uint8_t)(x + y + 1);
            line[x * 2 + 3] = 128;
        }
    }
}

struct camera_param {
    int width;
    int height;
    int fcc;
    int fps;
};

static struct camera_param g_param;
static pthread_t g_th;
static bool g_run;
static volatile sig_atomic_t g_exit;

static void handle_signal(int sig)
{
    (void)sig;
    g_exit = 1;
}

int run_uvc(int width, int height, int fcc, int fps)
{
    int fd;
    int ret;
    unsigned int handle;
    char *buffer;
    int handle_fd;
    size_t size;
    char file_name[128];

    snprintf(file_name, sizeof(file_name), "%dx%d.jpg", width, height);
    if (fcc == V4L2_PIX_FMT_MJPEG) {
        if (access(file_name, F_OK)) {
            fprintf(stderr, "file %s not exist\n", file_name);
            return -1;
        }
    }

    fd = drm_open();
    if (fd < 0)
        return -1;

    size = ALIGN(width, 16) * ALIGN(height, 16) *
           (fcc == V4L2_PIX_FMT_YUYV ? 2 : 3) / 2;
    ret = drm_alloc(fd, size, 16, &handle, 0);
    if (ret)
        goto close_drm;

    ret = drm_handle_to_fd(fd, handle, &handle_fd, 0);
    if (ret)
        goto free_drm;

    buffer = (char*)drm_map_buffer(fd, handle, size);
    if (!buffer) {
        printf("drm map buffer fail.\n");
        goto free_drm;
    }

    if (fcc == V4L2_PIX_FMT_MJPEG) {
        struct stat st;
        if (stat(file_name, &st) == 0 && (size_t)st.st_size <= size) {
            FILE *fp = fopen(file_name, "rb");
            if (fp) {
                size_t read_size;

                size = (size_t)st.st_size;
                read_size = fread(buffer, 1, size, fp);
                fclose(fp);
                if (read_size != size) {
                    fprintf(stderr, "read %s failed\n", file_name);
                    goto free_drm;
                }
            }
        }
    } else if (fcc == V4L2_PIX_FMT_YUYV) {
        fill_yuyv((uint8_t *)buffer, (uint32_t)width, (uint32_t)height);
        size = (size_t)width * (size_t)height * 2;
    } else {
        fill_nv12((uint8_t *)buffer, (uint32_t)width, (uint32_t)height,
                  (uint32_t)width, (uint32_t)height);
    }

    while (g_run) {
        uvc_read_camera_buffer(buffer, handle_fd, size, NULL, 0);
        if (fps > 0)
            usleep((useconds_t)(1000000u / (unsigned int)fps));
    }

    drm_unmap_buffer(buffer, size);
    ret = 0;
free_drm:
    drm_free(fd, handle);
close_drm:
    drm_close(fd);
    return ret;
}

static void *uvc_thread(void *arg)
{
    struct camera_param *param = (struct camera_param *)arg;
    run_uvc(param->width, param->height, param->fcc, param->fps);
    pthread_exit(NULL);
}

static int open_uvc(void *userdata, int width, int height, int fcc, int fps)
{
    (void)userdata;
    if (g_run)
        return 0;
    g_run = true;
    g_param.width = width;
    g_param.height = height;
    g_param.fcc = fcc;
    g_param.fps = fps;
    return pthread_create(&g_th, NULL, uvc_thread, &g_param);
}

static void close_uvc(void *userdata)
{
    (void)userdata;
    if (!g_run)
        return;
    g_run = false;
    pthread_join(g_th, NULL);
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    register_uvc_open_camera(open_uvc, NULL);
    register_uvc_close_camera(close_uvc, NULL);

    /* Keep the demo's producer configuration aligned with the supplied JPEG. */
    if (uvc_set_config(1080, 1920, UVC_FMT_MJPEG, 30) != 0) {
        fprintf(stderr, "uvc_set_config failed\n");
        uvc_control_module_close();
        return 1;
    }

    if (uvc_control_run(0) != 0) {
        fprintf(stderr, "uvc_control_run failed\n");
        uvc_control_module_close();
        return 1;
    }

    while (!g_exit)
        pause();

    uvc_control_join(0);
    return 0;
}

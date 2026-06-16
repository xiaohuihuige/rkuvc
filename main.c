#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <linux/videodev2.h>

#include "rkuvc.h"
#include "drm.h"

#define ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))

struct uvc_camera_ctx {
    int width;
    int height;
    int fcc;
    int fps;
    pthread_t thread;
    bool run;
    bool thread_active;
};

static void fill_image(uint8_t *buf, uint32_t width, uint32_t height,
                       uint32_t hor_stride, uint32_t ver_stride)
{
    uint8_t *buf_y = buf;
    uint32_t x, y;
    uint8_t *p = buf_y;

    for (y = 0; y < height; y++, p += hor_stride) {
        for (x = 0; x < width; x++)
            p[x] = (uint8_t)(x + y);
    }

    p = buf + hor_stride * ver_stride;
    for (y = 0; y < height / 2; y++, p += hor_stride) {
        for (x = 0; x < width / 2; x++) {
            p[x * 2 + 0] = (uint8_t)(128 + y);
            p[x * 2 + 1] = (uint8_t)(64 + x);
        }
    }
}

static int camera_load_mjpeg(struct uvc_camera_ctx *ctx, char *buffer, size_t *size)
{
    char file_name[128];
    struct stat st;

    snprintf(file_name, sizeof(file_name), "%dx%d.jpg", ctx->width, ctx->height);
    if (access(file_name, F_OK)) {
        printf("file %s not exist.\n", file_name);
        return -1;
    }
    if (stat(file_name, &st) != 0)
        return -1;

    {
        FILE *fp = fopen(file_name, "rb");

        if (!fp)
            return -1;
        *size = st.st_size;
        fread(buffer, 1, *size, fp);
        fclose(fp);
    }
    return 0;
}

static int camera_run_loop(struct uvc_camera_ctx *ctx)
{
    int fd;
    int ret;
    unsigned int handle;
    char *buffer;
    int handle_fd;
    size_t size;

    fd = drm_open();
    if (fd < 0)
        return -1;

    size = ALIGN(ctx->width, 16) * ALIGN(ctx->height, 16) * 3 / 2;
    ret = drm_alloc(fd, size, 16, &handle, 0);
    if (ret)
        goto err_close;

    ret = drm_handle_to_fd(fd, handle, &handle_fd, 0);
    if (ret)
        goto err_free;

    buffer = (char *)drm_map_buffer(fd, handle, size);
    if (!buffer) {
        ret = -1;
        goto err_free;
    }

    if (ctx->fcc == V4L2_PIX_FMT_MJPEG) {
        if (camera_load_mjpeg(ctx, buffer, &size) != 0) {
            ret = -1;
            goto err_unmap;
        }
    } else {
        fill_image((uint8_t *)buffer, ctx->width, ctx->height,
                   ctx->width, ctx->height);
    }

    while (ctx->run)
        uvc_read_camera_buffer(buffer, handle_fd, size, NULL, 0);

err_unmap:
    drm_unmap_buffer(buffer, size);
err_free:
    drm_free(fd, handle);
err_close:
    drm_close(fd);
    return ret;
}

static void *camera_thread_entry(void *arg)
{
    struct uvc_camera_ctx *ctx = arg;

    camera_run_loop(ctx);
    return NULL;
}

static int camera_open(void *userdata, int width, int height, int fcc, int fps)
{
    printf("camera_open\n");

    struct uvc_camera_ctx *ctx = userdata;
    int ret;

    if (ctx->thread_active)
        return 0;

    ctx->width = width;
    ctx->height = height;
    ctx->fcc = fcc;
    ctx->fps = fps;
    ctx->run = true;
    ret = pthread_create(&ctx->thread, NULL, camera_thread_entry, ctx);
    if (ret == 0)
        ctx->thread_active = true;
    return ret;
}

static void camera_close(void *userdata)
{
    printf("camera_close\n");
    struct uvc_camera_ctx *ctx = userdata;

    if (!ctx->thread_active)
        return;

    ctx->run = false;
    pthread_join(ctx->thread, NULL);
    ctx->thread_active = false;
}

int main(int argc, char *argv[])
{
    struct uvc_app *app;
    struct uvc_camera_ctx camera;

    (void)argc;
    (void)argv;

    memset(&camera, 0, sizeof(camera));

    register_uvc_open_camera(camera_open, &camera);
    register_uvc_close_camera(camera_close, &camera);

    app = uvc_app_create();
    if (!app)
        return -1;

    if (uvc_app_run(app, 0) != 0) {
        uvc_app_destroy(app);
        return -1;
    }

    while (1)
        sleep(5);

    uvc_app_destroy(app);
    return 0;
}

/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uvc_app.h"
#include "uvc_log.h"

#include <stdlib.h>

#include "uvc_control.h"
#include "uvc_device_monitor.h"
#include "uvc_event.h"
#include "uvc_session.h"
#include "uvc_video.h"

struct uvc_app {
    struct uvc_event_bus *bus;
    struct uvc_session *session;
    struct uvc_device_monitor *monitor;
    bool running;
};

struct uvc_app *uvc_app_create(void)
{
    struct uvc_app *app = calloc(1, sizeof(*app));

    uvc_log_printf("uvc app: create begin\n");
    if (!app) {
        uvc_log_printf("uvc app: allocate app failed\n");
        return NULL;
    }

    app->bus = uvc_event_bus_create();
    if (!app->bus) {
        uvc_log_printf("uvc app: create event bus failed\n");
        free(app);
        return NULL;
    }

    app->session = uvc_session_create(app->bus);
    app->monitor = uvc_device_monitor_create(app->bus);
    if (!app->session || !app->monitor) {
        uvc_log_printf("uvc app: create session/monitor failed session=%p monitor=%p\n",
                       (void *)app->session, (void *)app->monitor);
        uvc_app_destroy(app);
        return NULL;
    }

    if (uvc_control_module_open() != 0) {
        uvc_log_printf("uvc app: open control module failed\n");
        uvc_app_destroy(app);
        return NULL;
    }

    uvc_log_printf("uvc app: create complete app=%p\n", (void *)app);
    return app;
}

void uvc_app_destroy(struct uvc_app *app)
{
    if (!app)
        return;

    uvc_log_printf("uvc app: destroy begin app=%p running=%d\n",
                   (void *)app, app->running);
    uvc_app_stop(app);
    uvc_control_module_close();
    uvc_session_destroy(app->session);
    uvc_device_monitor_destroy(app->monitor);
    uvc_event_bus_destroy(app->bus);
    uvc_log_printf("uvc app: destroy complete\n");
    free(app);
}

static int uvc_app_run_straight(void)
{
    if (check_uvc_video_id() != 0)
        return -1;
    add_uvc_video();
    return 0;
}

static int uvc_app_run_hotplug(struct uvc_app *app)
{
    int ret;

    ret = uvc_session_start(app->session);
    if (ret != 0)
        return ret;

    ret = uvc_device_monitor_start(app->monitor);
    if (ret != 0) {
        uvc_session_stop(app->session);
        return ret;
    }

    return 0;
}

int uvc_app_run(struct uvc_app *app, uint32_t flags)
{
    int ret;

    if (!app || app->running) {
        uvc_log_printf("uvc app: run rejected app=%p running=%d flags=0x%x\n",
                       (void *)app, app ? app->running : 0, flags);
        return -1;
    }

    uvc_log_printf("uvc app: run begin mode=%s flags=0x%x\n",
                   (flags & UVC_APP_FLAG_CHECK_STRAIGHT) ? "straight" : "hotplug",
                   flags);

    if (flags & UVC_APP_FLAG_CHECK_STRAIGHT)
        ret = uvc_app_run_straight();
    else
        ret = uvc_app_run_hotplug(app);

    if (ret == 0)
        app->running = true;
    uvc_log_printf("uvc app: run end ret=%d running=%d\n", ret, app->running);
    return ret;
}

void uvc_app_stop(struct uvc_app *app)
{
    if (!app || !app->running)
        return;

    uvc_log_printf("uvc app: stop begin app=%p\n", (void *)app);
    uvc_device_monitor_stop(app->monitor);
    uvc_session_stop(app->session);
    uvc_video_id_exit_all();
    app->running = false;
    uvc_log_printf("uvc app: stop complete\n");
}

/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uvc_session.h"
#include "uvc_log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uvc_control.h"
#include "uvc_event.h"
#include "uvc_video.h"

struct uvc_session {
    struct uvc_event_bus *bus;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool thread_run;
    bool usb_attached;
    bool rescan_pending;
    bool teardown_pending;
    bool device_ready;
};

#define SESSION_VIDEO_NODE_RETRY_MS 20
#define SESSION_VIDEO_NODE_RETRIES 25
#define SESSION_GADGET_START_RETRY_MS 5
#define SESSION_GADGET_START_RETRIES 100
#define SESSION_ATTACH_RETRY_MS 500

static void session_do_attach(struct uvc_session *session);

struct uvc_session *uvc_session_create(struct uvc_event_bus *bus)
{
    struct uvc_session *session;

    if (!bus)
        return NULL;

    session = calloc(1, sizeof(*session));
    if (!session)
        return NULL;

    session->bus = bus;
    pthread_mutex_init(&session->lock, NULL);
    pthread_cond_init(&session->cond, NULL);
    return session;
}

void uvc_session_destroy(struct uvc_session *session)
{
    if (!session)
        return;

    uvc_session_stop(session);
    pthread_mutex_destroy(&session->lock);
    pthread_cond_destroy(&session->cond);
    free(session);
}

static void session_signal_rescan(struct uvc_session *session)
{
    pthread_mutex_lock(&session->lock);
    session->rescan_pending = true;
    pthread_cond_signal(&session->cond);
    pthread_mutex_unlock(&session->lock);
}

static void session_signal_teardown(struct uvc_session *session)
{
    pthread_mutex_lock(&session->lock);
    session->teardown_pending = true;
    session->rescan_pending = false;
    session->device_ready = false;
    pthread_cond_signal(&session->cond);
    pthread_mutex_unlock(&session->lock);
}

static void session_set_usb(struct uvc_session *session, bool attached)
{
    pthread_mutex_lock(&session->lock);
    session->usb_attached = attached;
    pthread_mutex_unlock(&session->lock);
}

static bool session_usb_attached(struct uvc_session *session)
{
    bool attached;

    pthread_mutex_lock(&session->lock);
    attached = session->usb_attached;
    pthread_mutex_unlock(&session->lock);
    return attached;
}

static void session_set_device_ready(struct uvc_session *session, bool ready)
{
    pthread_mutex_lock(&session->lock);
    session->device_ready = ready;
    session->rescan_pending = false;
    pthread_mutex_unlock(&session->lock);
}

static bool session_gadget_thread_active(int video_id)
{
    pthread_t *tp = uvc_video_get_uvc_pid(video_id);

    return tp && *tp != 0;
}

static void session_do_teardown(struct uvc_session *session)
{
    int video_id = -1;
    bool reattach;

    uvc_log_printf("uvc session: teardown\n");
    /* Release the kernel UVC output buffers while the video object still
     * exists.  Looking up the id after uvc_video_id_exit_all() always fails,
     * leaving stale V4L2 buffers behind and causing EBUSY on the next attach. */
    if (check_uvc_video_id() == 0)
        video_id = uvc_control_video_id(0);
    if (video_id >= 0)
        uvc_video_force_uvc_node_idle(video_id);

    uvc_control_exit();
    uvc_video_id_exit_all();

    /* The first idle request wakes the gadget thread.  Once it has joined,
     * repeat it while the /dev/video node is still available to clear any
     * buffers that were owned by the departing thread. */
    if (video_id >= 0)
        uvc_video_force_uvc_node_idle(video_id);

    session_set_device_ready(session, false);
    pthread_mutex_lock(&session->lock);
    reattach = session->thread_run && session->usb_attached;
    pthread_mutex_unlock(&session->lock);

    if (reattach)
        session_do_attach(session);
}

static int session_attach_settle_ms(void)
{
    const char *env = getenv("UVC_ATTACH_SETTLE_MS");
    int ms;

    if (!env || !env[0])
        return 400;
    ms = atoi(env);
    if (ms < 0)
        ms = 0;
    if (ms > 5000)
        ms = 5000;
    return ms;
}

static void session_cleanup_stale_gadget(int video_id)
{
    if (video_id < 0)
        return;

    /* uvc_video_id_check: non-zero when id is already registered. */
    if (uvc_video_id_check(video_id) == 0)
        return;

    if (uvc_get_user_run_state(video_id) && session_gadget_thread_active(video_id))
        return;

    uvc_log_printf("uvc session: cleaning stale gadget (video%d)\n", video_id);
    uvc_control_exit();
    uvc_video_id_exit_all();
}

static bool session_wait_for_video_node(struct uvc_session *session)
{
    int retry;

    for (retry = 0; retry <= SESSION_VIDEO_NODE_RETRIES; ++retry) {
        if (check_uvc_video_id() == 0)
            return true;
        if (!session_usb_attached(session))
            return false;
        if (retry < SESSION_VIDEO_NODE_RETRIES)
            usleep((useconds_t)SESSION_VIDEO_NODE_RETRY_MS * 1000);
    }

    uvc_log_printf("uvc session: UVC video node did not appear\n");
    return false;
}

static bool session_wait_for_gadget_start(int video_id)
{
    int retry;

    for (retry = 0; retry < SESSION_GADGET_START_RETRIES; ++retry) {
        if (uvc_get_user_run_state(video_id))
            return true;
        usleep((useconds_t)SESSION_GADGET_START_RETRY_MS * 1000);
    }

    return uvc_get_user_run_state(video_id);
}

static void session_schedule_attach_retry(struct uvc_session *session)
{
    uvc_log_printf("uvc session: schedule attach retry in %d ms\n",
                   SESSION_ATTACH_RETRY_MS);
    usleep((useconds_t)SESSION_ATTACH_RETRY_MS * 1000);
    if (session_usb_attached(session))
        session_signal_rescan(session);
}

static void session_do_attach(struct uvc_session *session)
{
    int video_id = -1;

    uvc_log_printf("uvc session: attach begin usb_attached=%d\n",
                   session_usb_attached(session));
    if (!session_usb_attached(session))
        return;

    if (!session_wait_for_video_node(session)) {
        if (session_usb_attached(session))
            session_schedule_attach_retry(session);
        return;
    }

    video_id = uvc_control_video_id(0);
    uvc_log_printf("uvc session: scan selected video_id=%d\n", video_id);
    if (video_id < 0) {
        uvc_log_printf("uvc session: no UVC id after scan\n");
        session_schedule_attach_retry(session);
        return;
    }

    if (uvc_video_id_check(video_id) != 0 &&
        uvc_get_user_run_state(video_id) &&
        session_gadget_thread_active(video_id)) {
        session_set_device_ready(session, true);
        uvc_log_printf("uvc session: gadget already running (video%d)\n", video_id);
        return;
    }

    session_cleanup_stale_gadget(video_id);

    if (video_id >= 0) {
        uvc_log_printf("uvc session: prepare video%d settle=%d ms\n",
                       video_id, session_attach_settle_ms());
        if (uvc_video_drain_gadget_thread(video_id) != 0) {
            session_set_device_ready(session, false);
            uvc_log_printf("uvc session: gadget drain failed (video%d), wait for next attach\n",
                   video_id);
            session_schedule_attach_retry(session);
            return;
        }
        uvc_video_force_uvc_node_idle(video_id);
        usleep((useconds_t)session_attach_settle_ms() * 1000);
    }

    add_uvc_video();

    if (uvc_video_id_check(video_id) == 0) {
        session_cleanup_stale_gadget(video_id);
        add_uvc_video();
    }

    if (uvc_video_id_check(video_id) == 0) {
        uvc_log_printf("uvc session: add uvc video failed (video%d)\n", video_id);
        session_set_device_ready(session, false);
        session_schedule_attach_retry(session);
        return;
    }

    session_set_device_ready(session, session_wait_for_gadget_start(video_id));

    if (uvc_session_device_ready(session))
        uvc_log_printf("uvc session: gadget ready for streaming\n");
    else
        uvc_log_printf("uvc session: gadget thread did not start (video%d)\n", video_id);

    if (!uvc_session_device_ready(session))
        session_schedule_attach_retry(session);
}

static bool session_pop_work(struct uvc_session *session,
                             bool *teardown, bool *rescan, bool wait)
{
    bool work = false;

    pthread_mutex_lock(&session->lock);
    if (wait) {
        while (session->thread_run && !session->teardown_pending &&
               !session->rescan_pending)
            pthread_cond_wait(&session->cond, &session->lock);
    }

    if (!session->thread_run) {
        pthread_mutex_unlock(&session->lock);
        return false;
    }

    if (!session->teardown_pending && !session->rescan_pending) {
        pthread_mutex_unlock(&session->lock);
        return false;
    }

    *teardown = session->teardown_pending;
    *rescan = session->rescan_pending;
    if (*teardown)
        session->teardown_pending = false;
    else
        session->rescan_pending = false;
    work = *teardown || *rescan;
    pthread_mutex_unlock(&session->lock);
    return work;
}

static void *session_thread_main(void *arg)
{
    struct uvc_session *session = arg;

    uvc_log_printf("uvc session: worker thread started\n");
    while (session->thread_run) {
        bool do_teardown = false;
        bool do_rescan = false;

        if (!session_pop_work(session, &do_teardown, &do_rescan, true))
            break;

        do {
            uvc_log_printf("uvc session: process work teardown=%d rescan=%d\n",
                           do_teardown, do_rescan);
            if (do_teardown)
                session_do_teardown(session);
            else if (do_rescan)
                session_do_attach(session);
        } while (session_pop_work(session, &do_teardown, &do_rescan, false));
    }

    session_do_teardown(session);
    uvc_log_printf("uvc session: worker thread exited\n");
    return NULL;
}

static void session_on_event(const struct uvc_event *event, void *userdata)
{
    struct uvc_session *session = userdata;

    if (!session || !event)
        return;

    switch (event->type) {
    case UVC_EVENT_USB_DETACHED:
        uvc_log_printf("uvc session: received USB detached event\n");
        session_set_usb(session, false);
        session_signal_teardown(session);
        break;
    case UVC_EVENT_USB_ATTACHED:
        uvc_log_printf("uvc session: received USB attached event\n");
        session_set_usb(session, true);
        session_signal_rescan(session);
        break;
    default:
        break;
    }
}

int uvc_session_start(struct uvc_session *session)
{
    int ret;

    if (!session || session->thread_run)
        return -1;

    uvc_log_printf("uvc session: start\n");

    session->thread_run = true;

    ret = uvc_event_subscribe(session->bus, session_on_event, session);
    if (ret != 0)
    {
        uvc_log_printf("uvc session: event subscribe failed ret=%d\n", ret);
        return ret;
    }

    ret = pthread_create(&session->thread, NULL, session_thread_main, session);
    if (ret != 0) {
        uvc_log_printf("uvc session: create worker failed ret=%d\n", ret);
        session->thread_run = false;
        uvc_event_unsubscribe(session->bus, session_on_event, session);
        return ret;
    }

    return 0;
}

void uvc_session_stop(struct uvc_session *session)
{
    if (!session || !session->thread_run)
        return;

    uvc_log_printf("uvc session: stop begin\n");
    pthread_mutex_lock(&session->lock);
    session->thread_run = false;
    pthread_cond_signal(&session->cond);
    pthread_mutex_unlock(&session->lock);

    pthread_join(session->thread, NULL);
    uvc_event_unsubscribe(session->bus, session_on_event, session);
    memset(&session->thread, 0, sizeof(session->thread));
    uvc_log_printf("uvc session: stop complete\n");
}

bool uvc_session_usb_attached(struct uvc_session *session)
{
    bool attached;

    if (!session)
        return false;

    pthread_mutex_lock(&session->lock);
    attached = session->usb_attached;
    pthread_mutex_unlock(&session->lock);
    return attached;
}

bool uvc_session_device_ready(struct uvc_session *session)
{
    bool ready;

    if (!session)
        return false;

    pthread_mutex_lock(&session->lock);
    ready = session->device_ready;
    pthread_mutex_unlock(&session->lock);
    return ready;
}

/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uvc_event.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define UVC_EVENT_MAX_SUBSCRIBERS 8

struct uvc_event_subscriber {
    uvc_event_handler_fn handler;
    void *userdata;
    bool used;
};

struct uvc_event_bus {
    pthread_mutex_t lock;
    struct uvc_event_subscriber subs[UVC_EVENT_MAX_SUBSCRIBERS];
};

struct uvc_event_bus *uvc_event_bus_create(void)
{
    struct uvc_event_bus *bus = calloc(1, sizeof(*bus));

    if (!bus)
        return NULL;
    pthread_mutex_init(&bus->lock, NULL);
    return bus;
}

void uvc_event_bus_destroy(struct uvc_event_bus *bus)
{
    if (!bus)
        return;
    pthread_mutex_destroy(&bus->lock);
    free(bus);
}

static int event_find_slot(struct uvc_event_bus *bus, uvc_event_handler_fn handler,
                           void *userdata, int *free_slot)
{
    int i;

    *free_slot = -1;
    for (i = 0; i < UVC_EVENT_MAX_SUBSCRIBERS; i++) {
        if (bus->subs[i].used && bus->subs[i].handler == handler &&
            bus->subs[i].userdata == userdata)
            return i;
        if (!bus->subs[i].used && *free_slot < 0)
            *free_slot = i;
    }
    return -1;
}

int uvc_event_subscribe(struct uvc_event_bus *bus,
                        uvc_event_handler_fn handler, void *userdata)
{
    int free_slot = -1;

    if (!bus || !handler)
        return -1;

    pthread_mutex_lock(&bus->lock);
    if (event_find_slot(bus, handler, userdata, &free_slot) >= 0) {
        pthread_mutex_unlock(&bus->lock);
        return 0;
    }
    if (free_slot < 0) {
        pthread_mutex_unlock(&bus->lock);
        return -1;
    }

    bus->subs[free_slot].handler = handler;
    bus->subs[free_slot].userdata = userdata;
    bus->subs[free_slot].used = true;
    pthread_mutex_unlock(&bus->lock);
    return 0;
}

void uvc_event_unsubscribe(struct uvc_event_bus *bus,
                           uvc_event_handler_fn handler, void *userdata)
{
    int i;

    if (!bus || !handler)
        return;

    pthread_mutex_lock(&bus->lock);
    for (i = 0; i < UVC_EVENT_MAX_SUBSCRIBERS; i++) {
        if (!bus->subs[i].used)
            continue;
        if (bus->subs[i].handler == handler &&
            bus->subs[i].userdata == userdata) {
            memset(&bus->subs[i], 0, sizeof(bus->subs[i]));
            break;
        }
    }
    pthread_mutex_unlock(&bus->lock);
}

static int event_copy_handlers(struct uvc_event_bus *bus,
                               uvc_event_handler_fn *handlers,
                               void **userdata, int max)
{
    int count = 0;
    int i;

    for (i = 0; i < UVC_EVENT_MAX_SUBSCRIBERS; i++) {
        if (!bus->subs[i].used)
            continue;
        if (count >= max)
            break;
        handlers[count] = bus->subs[i].handler;
        userdata[count] = bus->subs[i].userdata;
        count++;
    }
    return count;
}

void uvc_event_publish(struct uvc_event_bus *bus, const struct uvc_event *event)
{
    uvc_event_handler_fn handlers[UVC_EVENT_MAX_SUBSCRIBERS];
    void *userdata[UVC_EVENT_MAX_SUBSCRIBERS];
    int count;
    int i;

    if (!bus || !event)
        return;

    pthread_mutex_lock(&bus->lock);
    count = event_copy_handlers(bus, handlers, userdata,
                                UVC_EVENT_MAX_SUBSCRIBERS);
    pthread_mutex_unlock(&bus->lock);

    for (i = 0; i < count; i++)
        handlers[i](event, userdata[i]);
}

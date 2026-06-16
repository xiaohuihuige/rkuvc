/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#ifndef UVC_EVENT_H
#define UVC_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum uvc_event_type {
    UVC_EVENT_USB_ATTACHED = 0,
    UVC_EVENT_USB_DETACHED,
    UVC_EVENT_V4L2_ADD,
    UVC_EVENT_V4L2_REMOVE,
} uvc_event_type;

struct uvc_event {
    uvc_event_type type;
    int video_id;
};

struct uvc_event_bus;

typedef void (*uvc_event_handler_fn)(const struct uvc_event *event, void *userdata);

struct uvc_event_bus *uvc_event_bus_create(void);
void uvc_event_bus_destroy(struct uvc_event_bus *bus);

int uvc_event_subscribe(struct uvc_event_bus *bus,
                        uvc_event_handler_fn handler, void *userdata);
void uvc_event_unsubscribe(struct uvc_event_bus *bus,
                           uvc_event_handler_fn handler, void *userdata);

void uvc_event_publish(struct uvc_event_bus *bus, const struct uvc_event *event);

#ifdef __cplusplus
}
#endif

#endif

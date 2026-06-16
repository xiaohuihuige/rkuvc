/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#ifndef UVC_MONITOR_PARSE_H
#define UVC_MONITOR_PARSE_H

#include <stdbool.h>
#include <stddef.h>

struct uvc_event_bus;

#define UVC_UEVENT_STR_MAX 30

struct uvc_uevent_msg {
    char *strs[UVC_UEVENT_STR_MAX];
    int size;
};

struct uvc_monitor_extcon {
    struct uvc_event_bus *bus;
    /** Last published stream-ready state (-1 unknown, 0 off, 1 on). */
    int stream_last;
    /** Consecutive not-ready samples while stream_last was on (debounce unplug). */
    int stream_off_streak;
};

const char *uvc_monitor_uevent_get_value(const struct uvc_uevent_msg *msg,
                                         const char *key);

int uvc_monitor_uevent_decode(char *buf, int len, struct uvc_uevent_msg *msg);

bool uvc_monitor_uevent_is_extcon0_change(const struct uvc_uevent_msg *msg);

bool uvc_monitor_extcon_stream_ready(void);

void uvc_monitor_extcon_publish(struct uvc_monitor_extcon *ext);

void uvc_monitor_extcon_sync(struct uvc_monitor_extcon *ext);

void uvc_monitor_dispatch_uevent(struct uvc_event_bus *bus,
                                 struct uvc_monitor_extcon *extcon,
                                 const struct uvc_uevent_msg *msg);

#endif

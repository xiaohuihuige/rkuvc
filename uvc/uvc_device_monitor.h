/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#ifndef UVC_DEVICE_MONITOR_H
#define UVC_DEVICE_MONITOR_H

struct uvc_event_bus;
struct uvc_device_monitor;

struct uvc_device_monitor *uvc_device_monitor_create(struct uvc_event_bus *bus);
void uvc_device_monitor_destroy(struct uvc_device_monitor *mon);

int uvc_device_monitor_start(struct uvc_device_monitor *mon);
void uvc_device_monitor_stop(struct uvc_device_monitor *mon);

#endif

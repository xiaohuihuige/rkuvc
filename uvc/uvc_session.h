/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#ifndef UVC_SESSION_H
#define UVC_SESSION_H

#include <stdbool.h>

struct uvc_event_bus;
struct uvc_session;

struct uvc_session *uvc_session_create(struct uvc_event_bus *bus);
void uvc_session_destroy(struct uvc_session *session);

int uvc_session_start(struct uvc_session *session);
void uvc_session_stop(struct uvc_session *session);

bool uvc_session_usb_attached(struct uvc_session *session);
bool uvc_session_device_ready(struct uvc_session *session);

#endif

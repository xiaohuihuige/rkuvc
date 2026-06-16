/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uevent.h"

#include <stdio.h>

int uevent_monitor_run(uint32_t flags)
{
    (void)flags;
    printf("uevent_monitor_run: use uvc_app_create() and uvc_app_run()\n");
    return -1;
}

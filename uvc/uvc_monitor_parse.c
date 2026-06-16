/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uvc_monitor_parse.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uvc_event.h"
#include "uvc_control.h"

#define EXTCON0_CABLE0_DEFAULT \
    "/sys/class/extcon/extcon0/cable.0/state"
#define EXTCON0_CABLE3_DEFAULT \
    "/sys/class/extcon/extcon0/cable.3/state"

const char *uvc_monitor_uevent_get_value(const struct uvc_uevent_msg *msg,
                                         const char *key)
{
    size_t key_len = strlen(key);
    int i;

    for (i = 0; i < msg->size; i++) {
        const char *line = msg->strs[i];

        if (!strncmp(line, key, key_len) && line[key_len] == '=')
            return line + key_len + 1;
    }

    return NULL;
}

int uvc_monitor_uevent_decode(char *buf, int len, struct uvc_uevent_msg *msg)
{
    int i = 0;

    msg->size = 0;
    if (len <= 0)
        return -1;

    while (i < len && buf[i] != '\0')
        i++;
    if (i >= len)
        return -1;
    i++;

    while (i < len && msg->size < UVC_UEVENT_STR_MAX) {
        if (buf[i] == '\0') {
            i++;
            continue;
        }
        msg->strs[msg->size++] = &buf[i];
        while (i < len && buf[i] != '\0')
            i++;
        if (i < len)
            i++;
    }

    return msg->size > 0 ? 0 : -1;
}

static bool devpath_is_extcon0(const char *devpath)
{
    if (!devpath)
        return false;

    if (strstr(devpath, "/extcon/extcon0"))
        return true;
    if (strstr(devpath, "/extcon0"))
        return true;

    return false;
}

bool uvc_monitor_uevent_is_extcon0_change(const struct uvc_uevent_msg *msg)
{
    const char *action;
    const char *devpath;

    if (!msg || msg->size <= 0)
        return false;

    action = uvc_monitor_uevent_get_value(msg, "ACTION");
    if (!action || strcmp(action, "change") != 0)
        return false;

    devpath = uvc_monitor_uevent_get_value(msg, "DEVPATH");
    return devpath_is_extcon0(devpath);
}

static const char *cable0_state_path(void)
{
    const char *path = getenv("UVC_EXTCON_CABLE0_STATE");

    if (path && path[0])
        return path;
    return EXTCON0_CABLE0_DEFAULT;
}

static const char *cable3_state_path(void)
{
    const char *path = getenv("UVC_EXTCON_CABLE3_STATE");

    if (path && path[0])
        return path;
    return EXTCON0_CABLE3_DEFAULT;
}

static int read_sysfs_text(const char *path, char *buf, size_t len)
{
    int fd;
    ssize_t n;

    if (!path || !buf || len == 0)
        return -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return -1;

    buf[n] = '\0';
    return 0;
}

static void sysfs_trim(char *text)
{
    size_t n;

    if (!text)
        return;

    n = strlen(text);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r' ||
                     text[n - 1] == ' ' || text[n - 1] == '\t'))
        text[--n] = '\0';

    while (text[0] == ' ' || text[0] == '\t') {
        memmove(text, text + 1, strlen(text));
    }
}

/** Per-cable state file is often a single digit; extcon root state is KEY=0|1 lines. */
static bool sysfs_text_key_is_one(const char *text, const char *key)
{
    char buf[64];
    const char *line;
    size_t key_len;

    if (!text || !key)
        return false;

    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    sysfs_trim(buf);

    if (strchr(buf, '=') == NULL) {
        if (strcmp(buf, "1") == 0)
            return true;
        if (strcmp(buf, "0") == 0)
            return false;
        return atoi(buf) != 0;
    }

    key_len = strlen(key);
    line = buf;
    while (line && *line) {
        const char *eol = strchr(line, '\n');

        if (!strncmp(line, key, key_len) && line[key_len] == '=' &&
            line[key_len + 1] == '1')
            return true;

        if (!strncmp(line, key, key_len) && line[key_len] == '=' &&
            line[key_len + 1] == '0')
            return false;

        if (!eol)
            break;
        line = eol + 1;
    }

    return false;
}

static bool sysfs_cable_is_zero(const char *text)
{
    char buf[64];

    if (!text)
        return false;

    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    sysfs_trim(buf);

    if (strchr(buf, '=') == NULL)
        return strcmp(buf, "0") == 0;

    return false;
}

static bool uvc_monitor_extcon_cables_all_off(void)
{
    char cable0[64];
    char cable3[64];

    if (read_sysfs_text(cable0_state_path(), cable0, sizeof(cable0)) != 0)
        return false;
    if (read_sysfs_text(cable3_state_path(), cable3, sizeof(cable3)) != 0)
        return false;

    sysfs_trim(cable0);
    sysfs_trim(cable3);

    return sysfs_cable_is_zero(cable0) && sysfs_cable_is_zero(cable3);
}

bool uvc_monitor_extcon_stream_ready(void)
{
    char cable0[64];
    char cable3[64];
    bool c0;
    bool c3;

    if (read_sysfs_text(cable0_state_path(), cable0, sizeof(cable0)) != 0)
        return false;
    if (read_sysfs_text(cable3_state_path(), cable3, sizeof(cable3)) != 0)
        return false;

    sysfs_trim(cable0);
    sysfs_trim(cable3);

    c0 = sysfs_text_key_is_one(cable0, "USB");
    c3 = sysfs_text_key_is_one(cable3, "SDP");

    if (getenv("UVC_EXTCON_VERBOSE"))
        printf("uvc monitor extcon: cable.0=%s (%d)  cable.3=%s (%d)\n",
               cable0, c0 ? 1 : 0, cable3, c3 ? 1 : 0);

    return c0 && c3;
}

static int extcon_off_debounce_count(void)
{
    const char *env = getenv("UVC_EXTCON_OFF_DEBOUNCE");
    int n;

    if (!env || !env[0])
        return 5;
    n = atoi(env);
    if (n < 1)
        n = 1;
    if (n > 10)
        n = 10;
    return n;
}

void uvc_monitor_extcon_publish(struct uvc_monitor_extcon *ext)
{
    struct uvc_event ev;
    bool ready;
    int off_need;

    if (!ext || !ext->bus)
        return;

    ready = uvc_monitor_extcon_stream_ready();

    if (ready) {
        bool was_off = ext->stream_last != 1;

        ext->stream_off_streak = 0;
        ext->stream_last = 1;

        if (!was_off)
            return;

        printf("uvc monitor extcon: UVC connected (cable.0=1 and cable.3=1)\n");
        ev.video_id = -1;
        ev.type = UVC_EVENT_USB_ATTACHED;
        uvc_event_publish(ext->bus, &ev);
        return;
    }

    if (ext->stream_last != 1)
        return;

    /*
     * Host COMMIT/STREAMON often glitches extcon (one cable reads 0 briefly).
     * While UVC is streaming, only treat unplug when both cables read 0.
     */
    if (uvc_control_host_streaming() && !uvc_monitor_extcon_cables_all_off()) {
        ext->stream_off_streak = 0;
        return;
    }

    ext->stream_off_streak++;
    off_need = extcon_off_debounce_count();
    if (ext->stream_off_streak < off_need)
        return;

    ext->stream_last = 0;
    ext->stream_off_streak = 0;

    ev.video_id = -1;
    ev.type = UVC_EVENT_USB_DETACHED;
    printf("uvc monitor extcon: UVC disconnected (cable.0/cable.3 not both 1)\n");
    uvc_event_publish(ext->bus, &ev);
}

void uvc_monitor_extcon_sync(struct uvc_monitor_extcon *ext)
{
    if (!ext)
        return;
    /*
     * Force a fresh edge: already-plugged boot (no uevent) still publishes
     * ATTACHED when both cable states read 1.
     */
    ext->stream_last = -1;
    ext->stream_off_streak = 0;
    uvc_monitor_extcon_publish(ext);
}

void uvc_monitor_dispatch_uevent(struct uvc_event_bus *bus,
                                 struct uvc_monitor_extcon *extcon,
                                 const struct uvc_uevent_msg *msg)
{
    const char *action;
    const char *devpath;
    const char *devname;

    if (!bus || !extcon || !msg)
        return;

    if (!uvc_monitor_uevent_is_extcon0_change(msg))
        return;

    action = uvc_monitor_uevent_get_value(msg, "ACTION");
    devpath = uvc_monitor_uevent_get_value(msg, "DEVPATH");
    devname = uvc_monitor_uevent_get_value(msg, "DEVNAME");
    if (getenv("UVC_EXTCON_VERBOSE"))
        printf("uvc monitor: extcon0 uevent ACTION=%s DEVNAME=%s DEVPATH=%s\n",
               action ? action : "?",
               devname ? devname : "?",
               devpath ? devpath : "?");

    uvc_monitor_extcon_publish(extcon);
}

/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uvc_device_monitor.h"

#include "uvc_monitor_parse.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/socket.h>

#include <linux/netlink.h>

#include "uvc_epoll.h"
#include "uvc_event.h"

struct uvc_device_monitor {
    struct uvc_event_bus *bus;
    struct uvc_monitor_extcon extcon;
    volatile bool run;
    bool started;
    pthread_t tid;
    int wake_pipe[2];
};

struct monitor_nl_io {
    struct uvc_device_monitor *mon;
    struct msghdr msg;
    struct iovec iov;
    char buf[512];
};

struct uvc_device_monitor *uvc_device_monitor_create(struct uvc_event_bus *bus)
{
    struct uvc_device_monitor *mon;

    if (!bus)
        return NULL;

    mon = calloc(1, sizeof(*mon));
    if (!mon)
        return NULL;

    mon->bus = bus;
    mon->extcon.bus = bus;
    mon->wake_pipe[0] = mon->wake_pipe[1] = -1;
    return mon;
}

void uvc_device_monitor_destroy(struct uvc_device_monitor *mon)
{
    if (!mon)
        return;

    uvc_device_monitor_stop(mon);
    free(mon);
}

static void monitor_wake_handler(int fd, uint32_t revents, void *userdata)
{
    char drain[16];

    (void)userdata;
    if (!uvc_epoll_readable(revents))
        return;
    while (read(fd, drain, sizeof(drain)) > 0)
        ;
}

static void monitor_netlink_handler(int fd, uint32_t revents, void *userdata)
{
    struct monitor_nl_io *io = userdata;
    struct uvc_uevent_msg msg;
    int len;

    if (uvc_epoll_error(revents) || !uvc_epoll_readable(revents))
        return;

    len = recvmsg(fd, &io->msg, 0);
    if (len < 0)
        return;

    if (len < (int)sizeof(io->buf))
        io->buf[len] = '\0';
    else
        len = sizeof(io->buf) - 1;

    if (uvc_monitor_uevent_decode(io->buf, len, &msg) != 0)
        return;

    uvc_monitor_dispatch_uevent(io->mon->bus, &io->mon->extcon, &msg);
}

static int monitor_open_netlink(int *sockfd)
{
    struct sockaddr_nl sa;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = NETLINK_KOBJECT_UEVENT;
    sa.nl_pid = getpid();

    *sockfd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (*sockfd < 0)
        return -errno;

    if (bind(*sockfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(*sockfd);
        *sockfd = -1;
        return -errno;
    }

    return 0;
}

static int monitor_loop_should_stop(void *ctx)
{
    struct uvc_device_monitor *mon = ctx;

    return !mon->run;
}

static void monitor_thread_loop(struct uvc_device_monitor *mon)
{
    struct monitor_nl_io nl_io;
    struct uvc_epoll *ep = NULL;
    int sockfd = -1;
    int lr;

    if (monitor_open_netlink(&sockfd) != 0)
        return;

    ep = uvc_epoll_create(8);
    if (!ep)
        goto out;

    memset(&nl_io, 0, sizeof(nl_io));
    nl_io.mon = mon;
    nl_io.iov.iov_base = nl_io.buf;
    nl_io.iov.iov_len = sizeof(nl_io.buf);
    nl_io.msg.msg_iov = &nl_io.iov;
    nl_io.msg.msg_iovlen = 1;

    if (uvc_epoll_add(ep, sockfd, UVC_EPOLL_EVT_IN,
                      monitor_netlink_handler, &nl_io) != 0)
        goto out;

    if (mon->wake_pipe[0] >= 0)
        uvc_epoll_add(ep, mon->wake_pipe[0], UVC_EPOLL_EVT_IN,
                      monitor_wake_handler, NULL);

    uvc_monitor_extcon_sync(&mon->extcon);

    lr = uvc_epoll_loop(ep, NULL, NULL, monitor_loop_should_stop, mon);
    if (lr < 0)
        printf("uvc monitor: epoll %s\n", strerror(-lr));

out:
    uvc_epoll_destroy(ep);
    if (sockfd >= 0)
        close(sockfd);
}

static void *monitor_thread_entry(void *arg)
{
    struct uvc_device_monitor *mon = arg;

    prctl(PR_SET_NAME, "uvc_dev_monitor", 0, 0, 0);
    monitor_thread_loop(mon);
    mon->started = false;
    return NULL;
}

int uvc_device_monitor_start(struct uvc_device_monitor *mon)
{
    int ret;

    if (!mon || mon->started)
        return 0;

    if (mon->wake_pipe[0] < 0 && pipe(mon->wake_pipe) != 0)
        return -errno;

    mon->run = true;
    ret = pthread_create(&mon->tid, NULL, monitor_thread_entry, mon);
    if (ret != 0) {
        mon->run = false;
        return ret;
    }

    mon->started = true;
    return 0;
}

void uvc_device_monitor_stop(struct uvc_device_monitor *mon)
{
    char c = 1;

    if (!mon || !mon->started)
        return;

    mon->run = false;
    if (mon->wake_pipe[1] >= 0)
        write(mon->wake_pipe[1], &c, 1);

    pthread_join(mon->tid, NULL);
    mon->started = false;

    if (mon->wake_pipe[0] >= 0)
        close(mon->wake_pipe[0]);
    if (mon->wake_pipe[1] >= 0)
        close(mon->wake_pipe[1]);
    mon->wake_pipe[0] = mon->wake_pipe[1] = -1;
}

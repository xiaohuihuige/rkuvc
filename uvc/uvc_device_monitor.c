/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR MIT)
 */

#include "uvc_device_monitor.h"
#include "uvc_log.h"

#include "uvc_monitor_parse.h"

#include <errno.h>
#include <fcntl.h>
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
    bool rebind_requested;
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
    if (len < 0) {
        uvc_log_printf("uvc monitor: recvmsg failed: %s (%d)\n", strerror(errno), errno);
        return;
    }

    if (len < (int)sizeof(io->buf))
        io->buf[len] = '\0';
    else
        len = sizeof(io->buf) - 1;

    if (uvc_monitor_uevent_decode(io->buf, len, &msg) != 0)
        return;

    {
        const char *action = uvc_monitor_uevent_get_value(&msg, "ACTION");
        const char *devname = uvc_monitor_uevent_get_value(&msg, "DEVNAME");
        const char *devpath = uvc_monitor_uevent_get_value(&msg, "DEVPATH");
        uvc_log_printf("uvc monitor: uevent action=%s devname=%s devpath=%s fields=%d\n",
                       action ? action : "-", devname ? devname : "-",
                       devpath ? devpath : "-", msg.size);
    }

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

static int monitor_timeout_ms(void *ctx)
{
    (void)ctx;
    return 500;
}

static void monitor_uvc_rebind_if_stuck(struct uvc_device_monitor *mon)
{
    FILE *fp;
    char state[32] = {0};
    char udc[128] = {0};
    int fd;
    ssize_t n;

    fp = fopen("/sys/class/android_usb/android0/state", "r");
    if (!fp)
        return;
    if (!fgets(state, sizeof(state), fp)) {
        fclose(fp);
        return;
    }
    fclose(fp);
    if (strncmp(state, "DISCONNECTED", 12) != 0 ||
        !uvc_monitor_extcon_stream_ready()) {
        if (strncmp(state, "DISCONNECTED", 12) != 0)
            mon->rebind_requested = false;
        return;
    }
    if (mon->rebind_requested)
        return;

    fd = open("/sys/kernel/config/usb_gadget/rockchip/UDC", O_RDWR);
    if (fd < 0)
        return;
    n = read(fd, udc, sizeof(udc) - 1);
    if (n <= 0) {
        close(fd);
        return;
    }
    while (n > 0 && (udc[n - 1] == '\n' || udc[n - 1] == '\r'))
        n--;
    udc[n] = '\0';
    if (n == 0) {
        close(fd);
        return;
    }

    uvc_log_printf("uvc monitor: UVC-only UDC rebind (%s), preserving ADB/RNDIS config\n", udc);
    if (lseek(fd, 0, SEEK_SET) >= 0 && write(fd, "\n", 1) == 1) {
        usleep(200000);
        if (lseek(fd, 0, SEEK_SET) >= 0)
            (void)write(fd, udc, (size_t)n);
        mon->rebind_requested = true;
    }
    close(fd);
}

static void monitor_on_timeout(void *ctx)
{
    struct uvc_device_monitor *mon = ctx;

    uvc_monitor_extcon_publish(&mon->extcon);
    monitor_uvc_rebind_if_stuck(mon);
}

static void monitor_thread_loop(struct uvc_device_monitor *mon)
{
    struct monitor_nl_io nl_io;
    struct uvc_epoll *ep = NULL;
    int sockfd = -1;
    int lr;

    if (monitor_open_netlink(&sockfd) != 0) {
        uvc_log_printf("uvc monitor: open netlink failed: %s (%d)\n",
                       strerror(errno), errno);
        return;
    }

    uvc_log_printf("uvc monitor: loop started netlink_fd=%d wake_fd=%d\n",
                   sockfd, mon->wake_pipe[0]);

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

    lr = uvc_epoll_loop(ep, monitor_timeout_ms, monitor_on_timeout,
                        monitor_loop_should_stop, mon);
    if (lr < 0)
        uvc_log_printf("uvc monitor: epoll %s\n", strerror(-lr));

out:
    uvc_log_printf("uvc monitor: loop stopped\n");
    uvc_epoll_destroy(ep);
    if (sockfd >= 0)
        close(sockfd);
}

static void *monitor_thread_entry(void *arg)
{
    struct uvc_device_monitor *mon = arg;

    prctl(PR_SET_NAME, "uvc_dev_monitor", 0, 0, 0);
    while (mon->run) {
        monitor_thread_loop(mon);
        if (mon->run) {
            uvc_log_printf("uvc monitor: restarting after unexpected exit\n");
            sleep(1);
        }
    }
    return NULL;
}

int uvc_device_monitor_start(struct uvc_device_monitor *mon)
{
    int error;
    int flags;
    int ret;

    if (!mon || mon->started)
        return 0;

    uvc_log_printf("uvc monitor: start\n");

    if (mon->wake_pipe[0] < 0 && pipe(mon->wake_pipe) != 0)
        return -errno;

    flags = fcntl(mon->wake_pipe[0], F_GETFL, 0);
    if (flags < 0 || fcntl(mon->wake_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0)
        goto err_pipe;
    flags = fcntl(mon->wake_pipe[1], F_GETFL, 0);
    if (flags < 0 || fcntl(mon->wake_pipe[1], F_SETFL, flags | O_NONBLOCK) < 0)
        goto err_pipe;
    fcntl(mon->wake_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(mon->wake_pipe[1], F_SETFD, FD_CLOEXEC);

    mon->run = true;
    ret = pthread_create(&mon->tid, NULL, monitor_thread_entry, mon);
    if (ret != 0) {
        mon->run = false;
        error = ret;
        goto err_pipe_code;
    }

    mon->started = true;
    uvc_log_printf("uvc monitor: worker started tid=%lu\n", (unsigned long)mon->tid);
    return 0;

err_pipe:
    error = errno;
err_pipe_code:
    uvc_log_printf("uvc monitor: start failed error=%d (%s)\n",
                   error, strerror(error));
    close(mon->wake_pipe[0]);
    close(mon->wake_pipe[1]);
    mon->wake_pipe[0] = mon->wake_pipe[1] = -1;
    return -error;
}

void uvc_device_monitor_stop(struct uvc_device_monitor *mon)
{
    char c = 1;

    if (!mon || !mon->started)
        return;

    uvc_log_printf("uvc monitor: stop begin\n");
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
    uvc_log_printf("uvc monitor: stop complete\n");
}

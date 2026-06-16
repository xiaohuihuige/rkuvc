/*
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL), available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "uvc_epoll.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

struct uvc_epoll_entry {
    int fd;
    uint32_t events;
    uvc_epoll_handler_fn handler;
    void *userdata;
};

struct uvc_epoll {
    int epfd;
    int max_entries;
    struct epoll_event *events;
    struct uvc_epoll_entry *entries;
    int entry_count;
};

struct uvc_epoll *uvc_epoll_create(int max_entries)
{
    struct uvc_epoll *ep;

    if (max_entries <= 0)
        max_entries = 16;

    ep = calloc(1, sizeof(*ep));
    if (!ep)
        return NULL;

    ep->max_entries = max_entries;
    ep->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ep->epfd < 0) {
        free(ep);
        return NULL;
    }

    ep->events = calloc((size_t)max_entries, sizeof(*ep->events));
    ep->entries = calloc((size_t)max_entries, sizeof(*ep->entries));
    if (!ep->events || !ep->entries) {
        uvc_epoll_destroy(ep);
        return NULL;
    }

    return ep;
}

void uvc_epoll_destroy(struct uvc_epoll *ep)
{
    if (!ep)
        return;

    if (ep->epfd >= 0)
        close(ep->epfd);

    free(ep->events);
    free(ep->entries);
    free(ep);
}

static struct uvc_epoll_entry *uvc_epoll_find_entry(struct uvc_epoll *ep, int fd)
{
    int i;

    for (i = 0; i < ep->entry_count; i++) {
        if (ep->entries[i].fd == fd)
            return &ep->entries[i];
    }

    return NULL;
}

static int uvc_epoll_ctl(struct uvc_epoll *ep, int op, int fd, uint32_t events,
                         uvc_epoll_handler_fn handler, void *userdata)
{
    struct uvc_epoll_entry *entry;
    struct epoll_event ev;
    int ret;

    if (!ep || fd < 0)
        return -EINVAL;

    entry = uvc_epoll_find_entry(ep, fd);
    if (op == EPOLL_CTL_ADD) {
        if (entry)
            return -EEXIST;
        if (ep->entry_count >= ep->max_entries)
            return -ENOSPC;
        entry = &ep->entries[ep->entry_count++];
        entry->fd = fd;
        entry->events = events;
        entry->handler = handler;
        entry->userdata = userdata;
    } else if (!entry) {
        return -ENOENT;
    } else if (op == EPOLL_CTL_MOD) {
        entry->events = events;
        if (handler)
            entry->handler = handler;
        if (userdata)
            entry->userdata = userdata;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    ret = epoll_ctl(ep->epfd, op, fd, op == EPOLL_CTL_DEL ? NULL : &ev);
    if (ret < 0) {
        if (op == EPOLL_CTL_ADD)
            ep->entry_count--;
        return -errno;
    }

    if (op == EPOLL_CTL_DEL) {
        *entry = ep->entries[ep->entry_count - 1];
        ep->entry_count--;
    }

    return 0;
}

int uvc_epoll_add(struct uvc_epoll *ep, int fd, uint32_t events,
                  uvc_epoll_handler_fn handler, void *userdata)
{
    return uvc_epoll_ctl(ep, EPOLL_CTL_ADD, fd, events, handler, userdata);
}

int uvc_epoll_mod(struct uvc_epoll *ep, int fd, uint32_t events,
                  uvc_epoll_handler_fn handler, void *userdata)
{
    return uvc_epoll_ctl(ep, EPOLL_CTL_MOD, fd, events, handler, userdata);
}

int uvc_epoll_del(struct uvc_epoll *ep, int fd)
{
    return uvc_epoll_ctl(ep, EPOLL_CTL_DEL, fd, 0, NULL, NULL);
}

int uvc_epoll_wait(struct uvc_epoll *ep, int timeout_ms)
{
    int n;
    int i;
    int dispatched = 0;

    if (!ep || ep->epfd < 0)
        return -EINVAL;

    for (;;) {
        n = epoll_wait(ep->epfd, ep->events, ep->max_entries, timeout_ms);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        break;
    }

    for (i = 0; i < n; i++) {
        struct uvc_epoll_entry *entry;
        int fd = ep->events[i].data.fd;
        uint32_t revents;

        entry = uvc_epoll_find_entry(ep, fd);
        if (!entry || !entry->handler)
            continue;

        revents = ep->events[i].events &
                  (entry->events | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
        if (!revents)
            continue;

        entry->handler(fd, revents, entry->userdata);
        dispatched++;
    }

    return dispatched;
}

int uvc_epoll_loop(struct uvc_epoll *ep,
                   int (*get_timeout_ms)(void *ctx),
                   void (*on_timeout)(void *ctx),
                   int (*should_stop)(void *ctx),
                   void *loop_ctx)
{
    int wait_ms;
    int wr;

    if (!ep)
        return -EINVAL;

    for (;;) {
        if (should_stop && should_stop(loop_ctx))
            return 0;

        wait_ms = get_timeout_ms ? get_timeout_ms(loop_ctx) : -1;

        wr = uvc_epoll_wait(ep, wait_ms);
        if (wr < 0)
            return wr;

        if (wr == 0 && wait_ms >= 0 && on_timeout)
            on_timeout(loop_ctx);

        if (should_stop && should_stop(loop_ctx))
            return 0;
    }
}

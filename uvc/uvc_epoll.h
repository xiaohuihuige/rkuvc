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

#ifndef UVC_EPOLL_H
#define UVC_EPOLL_H

#include <stdint.h>
#include <sys/epoll.h>

struct uvc_epoll;

typedef void (*uvc_epoll_handler_fn)(int fd, uint32_t revents, void *userdata);

/* Interest flags for uvc_epoll_add/mod (same as epoll). */
#define UVC_EPOLL_EVT_IN   EPOLLIN
#define UVC_EPOLL_EVT_OUT  EPOLLOUT
#define UVC_EPOLL_EVT_PRI  EPOLLPRI
#define UVC_EPOLL_EVT_ERR  (EPOLLERR | EPOLLHUP | EPOLLRDHUP)

static inline int uvc_epoll_readable(uint32_t revents)
{
    return (revents & (EPOLLIN | EPOLLRDHUP)) != 0;
}

static inline int uvc_epoll_writable(uint32_t revents)
{
    return (revents & EPOLLOUT) != 0;
}

static inline int uvc_epoll_priority(uint32_t revents)
{
    return (revents & EPOLLPRI) != 0;
}

static inline int uvc_epoll_error(uint32_t revents)
{
    return (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
}

struct uvc_epoll *uvc_epoll_create(int max_entries);
void uvc_epoll_destroy(struct uvc_epoll *ep);

int uvc_epoll_add(struct uvc_epoll *ep, int fd, uint32_t events,
                  uvc_epoll_handler_fn handler, void *userdata);
int uvc_epoll_mod(struct uvc_epoll *ep, int fd, uint32_t events,
                  uvc_epoll_handler_fn handler, void *userdata);
int uvc_epoll_del(struct uvc_epoll *ep, int fd);

/*
 * timeout_ms: -1 wait indefinitely, 0 poll, >0 wait up to N ms.
 * Returns the number of dispatched handlers, 0 on timeout, -1 on error.
 */
int uvc_epoll_wait(struct uvc_epoll *ep, int timeout_ms);

/*
 * Run until should_stop() is true. get_timeout_ms() supplies each wait
 * (NULL => -1). on_timeout() is called after a wait that returned 0 when
 * the configured timeout was >= 0 (NULL get_timeout_ms counts as -1).
 * Returns 0 when stopped normally, negative errno from uvc_epoll_wait.
 */
int uvc_epoll_loop(struct uvc_epoll *ep,
                   int (*get_timeout_ms)(void *ctx),
                   void (*on_timeout)(void *ctx),
                   int (*should_stop)(void *ctx),
                   void *ctx);

#endif

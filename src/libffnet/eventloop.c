// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffnet.h"
#include "ffnet/ffutil.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct FdSlot {
    FfEvCallback cb;
    void        *ctx;
    uint32_t     events;        /* FF_EV_* mask */
    int          in_use;
} FdSlot;

struct FfEvLoop {
    int        epfd;
    int        stop_flag;
    FdSlot    *slots;
    size_t     cap;             /* sparse array indexed by fd */
};

static int grow_slots(FfEvLoop *L, int fd)
{
    if ((size_t)fd < L->cap) return FFE_OK;
    size_t new_cap = L->cap ? L->cap : 64;
    while (new_cap <= (size_t)fd) new_cap *= 2;
    FdSlot *p = (FdSlot *)realloc(L->slots, new_cap * sizeof(FdSlot));
    if (!p) return FFE_NOMEM;
    memset(p + L->cap, 0, (new_cap - L->cap) * sizeof(FdSlot));
    L->slots = p;
    L->cap   = new_cap;
    return FFE_OK;
}

static uint32_t to_epoll(uint32_t ev)
{
    uint32_t r = 0;
    if (ev & FF_EV_READ)  r |= EPOLLIN;
    if (ev & FF_EV_WRITE) r |= EPOLLOUT;
    return r;
}

static uint32_t from_epoll(uint32_t ep)
{
    uint32_t r = 0;
    if (ep & (EPOLLIN | EPOLLHUP | EPOLLERR)) r |= FF_EV_READ;
    if (ep & EPOLLOUT)                         r |= FF_EV_WRITE;
    return r;
}

FfEvLoop *ffnet_evloop_new(void)
{
    FfEvLoop *L = (FfEvLoop *)calloc(1, sizeof(FfEvLoop));
    if (!L) return NULL;
    L->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (L->epfd < 0) {
        free(L);
        return NULL;
    }
    return L;
}

void ffnet_evloop_free(FfEvLoop *L)
{
    if (!L) return;
    if (L->epfd >= 0) close(L->epfd);
    free(L->slots);
    free(L);
}

int ffnet_evloop_add(FfEvLoop *L, int fd, uint32_t events,
                     FfEvCallback cb, void *ctx)
{
    if (!L || fd < 0 || !cb) return FFE_INVAL;
    int rc = grow_slots(L, fd);
    if (rc != FFE_OK) return rc;
    if (L->slots[fd].in_use) return FFE_INVAL;

    struct epoll_event ev = { .events = to_epoll(events), .data.fd = fd };
    if (epoll_ctl(L->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return ffutil_map_errno(-errno);
    }
    L->slots[fd].cb     = cb;
    L->slots[fd].ctx    = ctx;
    L->slots[fd].events = events;
    L->slots[fd].in_use = 1;
    return FFE_OK;
}

int ffnet_evloop_mod(FfEvLoop *L, int fd, uint32_t events)
{
    if (!L || fd < 0 || (size_t)fd >= L->cap || !L->slots[fd].in_use)
        return FFE_INVAL;
    struct epoll_event ev = { .events = to_epoll(events), .data.fd = fd };
    if (epoll_ctl(L->epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        return ffutil_map_errno(-errno);
    }
    L->slots[fd].events = events;
    return FFE_OK;
}

int ffnet_evloop_del(FfEvLoop *L, int fd)
{
    if (!L || fd < 0 || (size_t)fd >= L->cap || !L->slots[fd].in_use)
        return FFE_INVAL;
    if (epoll_ctl(L->epfd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        return ffutil_map_errno(-errno);
    }
    L->slots[fd].in_use = 0;
    L->slots[fd].cb     = NULL;
    L->slots[fd].ctx    = NULL;
    L->slots[fd].events = 0;
    return FFE_OK;
}

int ffnet_evloop_run(FfEvLoop *L)
{
    if (!L) return FFE_INVAL;
    struct epoll_event evs[64];
    L->stop_flag = 0;
    for (;;) {
        if (L->stop_flag) return FFE_OK;

        int n = epoll_wait(L->epfd, evs, 64, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return ffutil_map_errno(-errno);
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if ((size_t)fd >= L->cap || !L->slots[fd].in_use) continue;
            uint32_t fired = from_epoll(evs[i].events);
            if (fired == 0) continue;
            L->slots[fd].cb(L, fd, fired, L->slots[fd].ctx);
            /* callback may have called del/free; we don't dereference
             * L->slots[fd] again before next epoll_wait. */
        }
    }
}

void ffnet_evloop_stop(FfEvLoop *L)
{
    if (L) L->stop_flag = 1;
}

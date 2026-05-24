// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffnet.h"
#include "ffnet/ffproto.h"
#include "ffnet/ffutil.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Per-worker self-pipe for stop. The signal handler writes; workers' loops
 * fire on the read end and call ffnet_evloop_stop. */
typedef struct Worker {
    int               id;
    int               stop_pipe[2];      /* [0]=read, [1]=write */
    const Protocol   *proto;
    const char       *host;
    uint16_t          port;
    int               listen_fd;
    FfEvLoop         *loop;
    pthread_t         thread;
} Worker;

/* Per-connection bookkeeping: holds the protocol's ctx + back-ref to the
 * worker (for accessing proto->close on teardown). Allocated on accept,
 * freed on close. */
typedef struct Conn {
    void   *proto_ctx;
    Worker *w;
} Conn;

static void conn_cb(FfEvLoop *loop, int fd, uint32_t events, void *ctx)
{
    Conn *c = (Conn *)ctx;
    int rc = c->w->proto->on_event(fd, c->proto_ctx, events);
    if (rc > 0) {
        if (ffnet_evloop_mod(loop, fd, (uint32_t)rc) != FFE_OK) {
            FF_LOGW("worker %d evloop_mod fd=%d failed", c->w->id, fd);
        }
        return;
    }
    if (rc < 0) {
        FF_LOGW("worker %d conn fd=%d: %s", c->w->id, fd, ffutil_strerror_v(rc));
    }
    c->w->proto->close(fd, c->proto_ctx);
    ffnet_evloop_del(loop, fd);
    close(fd);
    free(c);
}

static void accept_cb(FfEvLoop *loop, int fd, uint32_t events, void *ctx)
{
    (void)events;
    Worker *w = (Worker *)ctx;

    /* LT epoll: drain the accept backlog. */
    for (;;) {
        int cfd = ffnet_tcp_accept(fd);
        if (cfd == FFE_AGAIN) return;
        if (cfd < 0) {
            FF_LOGW("worker %d accept: %s", w->id, ffutil_strerror_v(cfd));
            return;
        }

        int rc = ffnet_set_nonblocking(cfd);
        if (rc != FFE_OK) {
            FF_LOGW("worker %d set_nonblocking: %s", w->id, ffutil_strerror_v(rc));
            close(cfd);
            continue;
        }

        Conn *c = (Conn *)calloc(1, sizeof(Conn));
        if (!c) { close(cfd); continue; }
        c->w = w;

        rc = w->proto->open(cfd, &c->proto_ctx);
        if (rc != FFE_OK) {
            FF_LOGW("worker %d proto->open: %s", w->id, ffutil_strerror_v(rc));
            free(c);
            close(cfd);
            continue;
        }

        rc = ffnet_evloop_add(loop, cfd, FF_EV_READ, conn_cb, c);
        if (rc != FFE_OK) {
            FF_LOGW("worker %d evloop_add: %s", w->id, ffutil_strerror_v(rc));
            w->proto->close(cfd, c->proto_ctx);
            free(c);
            close(cfd);
            continue;
        }
    }
}

static void stop_pipe_cb(FfEvLoop *loop, int fd, uint32_t events, void *ctx)
{
    (void)events; (void)ctx;
    char buf[16];
    while (read(fd, buf, sizeof buf) > 0) { /* drain */ }
    ffnet_evloop_stop(loop);
}

static void *worker_main(void *arg)
{
    Worker *w = (Worker *)arg;
    int rc;

    w->listen_fd = ffnet_tcp_create();
    if (w->listen_fd < 0) {
        FF_LOGE("worker %d create: %s", w->id, ffutil_strerror_v(w->listen_fd));
        return NULL;
    }

    if ((rc = ffnet_tcp_set_reuseport(w->listen_fd)) != FFE_OK) {
        FF_LOGE("worker %d reuseport: %s", w->id, ffutil_strerror_v(rc));
        goto err_fd;
    }
    if ((rc = ffnet_set_nonblocking(w->listen_fd)) != FFE_OK) {
        FF_LOGE("worker %d nonblocking: %s", w->id, ffutil_strerror_v(rc));
        goto err_fd;
    }
    if ((rc = ffnet_tcp_bind_listen(w->listen_fd, w->host, w->port, 128)) != FFE_OK) {
        FF_LOGE("worker %d bind_listen %s:%u: %s",
                w->id, w->host, (unsigned)w->port, ffutil_strerror_v(rc));
        goto err_fd;
    }

    w->loop = ffnet_evloop_new();
    if (!w->loop) {
        FF_LOGE("worker %d evloop_new failed", w->id);
        goto err_fd;
    }

    if ((rc = ffnet_evloop_add(w->loop, w->listen_fd, FF_EV_READ, accept_cb, w)) != FFE_OK) {
        FF_LOGE("worker %d evloop_add listen: %s", w->id, ffutil_strerror_v(rc));
        goto err_loop;
    }
    if ((rc = ffnet_evloop_add(w->loop, w->stop_pipe[0], FF_EV_READ, stop_pipe_cb, NULL)) != FFE_OK) {
        FF_LOGE("worker %d evloop_add stop_pipe: %s", w->id, ffutil_strerror_v(rc));
        goto err_loop;
    }

    if (w->id == 0) {
        int p = ffnet_tcp_local_port(w->listen_fd);
        if (p > 0) {
            printf("listening on %s:%d\n", w->host, p);
            fflush(stdout);
        }
    }

    ffnet_evloop_run(w->loop);

err_loop:
    ffnet_evloop_free(w->loop);
    w->loop = NULL;
err_fd:
    if (w->listen_fd >= 0) {
        close(w->listen_fd);
        w->listen_fd = -1;
    }
    return NULL;
}

/* --- signal handler shim --- */
static Worker *g_workers = NULL;
static int     g_n_workers = 0;

static void on_sigint(int signo)
{
    (void)signo;
    for (int i = 0; i < g_n_workers; i++) {
        if (g_workers[i].stop_pipe[1] >= 0) {
            ssize_t r = write(g_workers[i].stop_pipe[1], "x", 1);
            (void)r;
        }
    }
}

int ffnet_server_run(const Protocol *proto, const char *host,
                     uint16_t port, int workers)
{
    if (!proto || !host || workers < 1) return FFE_INVAL;

    Worker *ws = (Worker *)calloc((size_t)workers, sizeof(Worker));
    if (!ws) return FFE_NOMEM;

    for (int i = 0; i < workers; i++) {
        ws[i].id        = i;
        ws[i].proto     = proto;
        ws[i].host      = host;
        ws[i].port      = port;
        ws[i].listen_fd = -1;
        ws[i].stop_pipe[0] = -1;
        ws[i].stop_pipe[1] = -1;
        if (pipe(ws[i].stop_pipe) != 0) {
            for (int j = 0; j < i; j++) {
                if (ws[j].stop_pipe[0] >= 0) close(ws[j].stop_pipe[0]);
                if (ws[j].stop_pipe[1] >= 0) close(ws[j].stop_pipe[1]);
            }
            free(ws);
            return ffutil_map_errno(-errno);
        }
        ffnet_set_nonblocking(ws[i].stop_pipe[0]);
    }

    g_workers   = ws;
    g_n_workers = workers;
    struct sigaction sa = { .sa_handler = on_sigint };
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (workers == 1) {
        worker_main(&ws[0]);
    } else {
        for (int i = 0; i < workers; i++) {
            pthread_create(&ws[i].thread, NULL, worker_main, &ws[i]);
        }
        for (int i = 0; i < workers; i++) {
            pthread_join(ws[i].thread, NULL);
        }
    }

    for (int i = 0; i < workers; i++) {
        if (ws[i].stop_pipe[0] >= 0) close(ws[i].stop_pipe[0]);
        if (ws[i].stop_pipe[1] >= 0) close(ws[i].stop_pipe[1]);
    }
    free(ws);
    g_workers   = NULL;
    g_n_workers = 0;
    return FFE_OK;
}

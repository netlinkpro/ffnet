// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FFNET_FFNET_H
#define FFNET_FFNET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create a blocking IPv4 TCP socket.
 * Returns fd >= 0 on success, FFE_IO on failure. */
int ffnet_tcp_create(void);

/* Bind `fd` to `addr:port` and start listening with `backlog`.
 * `addr` is a numeric IPv4 dotted-quad ("0.0.0.0", "127.0.0.1");
 * DNS resolution is deferred to a later slice.
 * `port == 0` requests an ephemeral port (query via ffnet_tcp_local_port).
 * Sets SO_REUSEADDR before bind.
 * Returns FFE_OK or FFE_IO / FFE_INVAL. */
int ffnet_tcp_bind_listen(int fd, const char *addr, uint16_t port, int backlog);

/* Accept a connection on the listening socket `listen_fd`.
 * Returns the client fd (>= 0) on success, FFE_IO on failure. */
int ffnet_tcp_accept(int listen_fd);

/* Returns the local port `fd` is bound to (useful after port=0 binding).
 * Returns the port (1..65535) on success, FFE_IO on failure. */
int ffnet_tcp_local_port(int fd);

/* Close a TCP fd. Returns FFE_OK or FFE_IO. */
int ffnet_tcp_close(int fd);

/* ===== Non-blocking + multi-worker helpers ===== */

/* Set O_NONBLOCK on fd. Returns FFE_OK or negative. */
int ffnet_set_nonblocking(int fd);

/* Enable SO_REUSEPORT on a listening socket. Call before
 * ffnet_tcp_bind_listen on each worker's listen fd so the kernel
 * distributes incoming SYNs across workers. */
int ffnet_tcp_set_reuseport(int fd);

/* ===== Event loop ===== */

/* Opaque epoll-based event loop. One loop per thread; never shared. */
typedef struct FfEvLoop FfEvLoop;

#define FF_EV_READ  (1u << 0)
#define FF_EV_WRITE (1u << 1)

/* Fired when fd has the requested events ready. `events` is the subset
 * of FF_EV_* that actually fired. */
typedef void (*FfEvCallback)(FfEvLoop *loop, int fd, uint32_t events, void *ctx);

FfEvLoop *ffnet_evloop_new(void);
void      ffnet_evloop_free(FfEvLoop *loop);

int  ffnet_evloop_add(FfEvLoop *loop, int fd, uint32_t events,
                      FfEvCallback cb, void *ctx);
int  ffnet_evloop_mod(FfEvLoop *loop, int fd, uint32_t events);
int  ffnet_evloop_del(FfEvLoop *loop, int fd);

/* Block until ffnet_evloop_stop() is called or until no fds remain
 * registered. Returns FFE_OK or negative. */
int  ffnet_evloop_run(FfEvLoop *loop);
void ffnet_evloop_stop(FfEvLoop *loop);

/* ===== High-level server ===== */

struct Protocol;   /* forward decl from ffproto.h */

/* Spawn `workers` pthreads, each binds to host:port with SO_REUSEPORT,
 * runs its own event loop, dispatches to the protocol's open/on_event/close.
 *   workers == 0 -> FFE_INVAL.
 *   workers == 1 -> runs in calling thread (no pthread spawned).
 *   workers >= 2 -> spawns pthreads; calling thread joins.
 * Blocks until SIGINT/SIGTERM. */
int ffnet_server_run(const struct Protocol *proto, const char *host,
                     uint16_t port, int workers);

#ifdef __cplusplus
}
#endif
#endif /* FFNET_FFNET_H */

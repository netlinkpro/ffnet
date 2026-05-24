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

#ifdef __cplusplus
}
#endif
#endif /* FFNET_FFNET_H */

// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffnet.h"
#include "ffnet/ffutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/socket.h>

/* Asm-side primitives (Linux x86_64 raw syscalls). Each returns >=0 on
 * success or -errno on failure. */
int _ff_asm_socket_create(void);
int _ff_asm_bind_listen(int fd, uint32_t ip_be, uint16_t port_be, int backlog);
int _ff_asm_accept(int fd);
int _ff_asm_local_port(int fd);
int _ff_asm_close(int fd);

/* Parse a dotted-quad IPv4 string into a big-endian u32.
 * Strict: 1..3 decimal digits per part, exactly three dots, no trailing chars. */
static int parse_ipv4(const char *s, uint32_t *out)
{
    if (!s) return FFE_INVAL;

    uint32_t host = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return FFE_INVAL;

        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10u + (unsigned)(*s - '0');
            if (v > 255u) return FFE_INVAL;
            s++;
            if (++digits > 3) return FFE_INVAL;
        }
        host = (host << 8) | v;

        if (part < 3) {
            if (*s != '.') return FFE_INVAL;
            s++;
        }
    }
    if (*s != '\0') return FFE_INVAL;

    /* host order → big-endian */
    *out = ((host & 0x000000FFu) << 24) |
           ((host & 0x0000FF00u) <<  8) |
           ((host & 0x00FF0000u) >>  8) |
           ((host & 0xFF000000u) >> 24);
    return FFE_OK;
}

int ffnet_tcp_create(void)
{
    int rc = _ff_asm_socket_create();
    return rc < 0 ? ffutil_map_errno(rc) : rc;
}

int ffnet_tcp_bind_listen(int fd, const char *addr, uint16_t port, int backlog)
{
    uint32_t ip_be;
    int rc = parse_ipv4(addr, &ip_be);
    if (rc != FFE_OK) return rc;

    uint16_t port_be = (uint16_t)(((uint16_t)(port << 8)) | (port >> 8));
    rc = _ff_asm_bind_listen(fd, ip_be, port_be, backlog);
    return rc < 0 ? ffutil_map_errno(rc) : FFE_OK;
}

int ffnet_tcp_accept(int listen_fd)
{
    int rc = _ff_asm_accept(listen_fd);
    return rc < 0 ? ffutil_map_errno(rc) : rc;
}

int ffnet_tcp_local_port(int fd)
{
    int rc = _ff_asm_local_port(fd);
    return rc < 0 ? ffutil_map_errno(rc) : rc;
}

int ffnet_tcp_close(int fd)
{
    int rc = _ff_asm_close(fd);
    return rc < 0 ? ffutil_map_errno(rc) : FFE_OK;
}

int ffnet_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return ffutil_map_errno(-errno);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return ffutil_map_errno(-errno);
    return FFE_OK;
}

int ffnet_tcp_set_reuseport(int fd)
{
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0)
        return ffutil_map_errno(-errno);
    return FFE_OK;
}

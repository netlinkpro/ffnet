// SPDX-License-Identifier: LGPL-2.1-or-later
/* test_tcp.c — bind/listen/accept smoke test.
 * Uses fork() instead of pthreads to keep slice-1 deps minimal. */
#include "ffnet/ffnet.h"
#include "ffnet/ffutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = ffnet_tcp_create();
    if (listen_fd < 0) {
        fprintf(stderr, "FAIL: ffnet_tcp_create: %s\n",
                ffutil_strerror(listen_fd));
        return 1;
    }

    int rc = ffnet_tcp_bind_listen(listen_fd, "127.0.0.1", 0, 4);
    if (rc != FFE_OK) {
        fprintf(stderr, "FAIL: bind_listen: %s\n", ffutil_strerror(rc));
        return 1;
    }

    int port = ffnet_tcp_local_port(listen_fd);
    if (port <= 0) {
        fprintf(stderr, "FAIL: local_port: %s\n", ffutil_strerror(port));
        return 1;
    }
    printf("ok: bound to 127.0.0.1:%d\n", port);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        /* Child: connect via libc + send "hi". */
        int cs = socket(AF_INET, SOCK_STREAM, 0);
        if (cs < 0) { perror("child socket"); _exit(1); }

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons((uint16_t)port);
        sa.sin_addr.s_addr = htonl(0x7F000001);   /* 127.0.0.1 */

        if (connect(cs, (struct sockaddr *)&sa, sizeof sa) != 0) {
            perror("child connect");
            _exit(1);
        }
        if (write(cs, "hi", 2) != 2) {
            perror("child write");
            _exit(1);
        }
        close(cs);
        _exit(0);
    }

    /* Parent: accept the child's connection. */
    int client_fd = ffnet_tcp_accept(listen_fd);
    if (client_fd < 0) {
        fprintf(stderr, "FAIL: accept: %s\n", ffutil_strerror(client_fd));
        return 1;
    }
    printf("ok: accepted client\n");

    char buf[8] = {0};
    ssize_t n = read(client_fd, buf, sizeof buf - 1);
    if (n != 2 || memcmp(buf, "hi", 2) != 0) {
        fprintf(stderr, "FAIL: read got %zd bytes, content %.*s\n",
                n, (int)(n > 0 ? n : 0), buf);
        ffnet_tcp_close(client_fd);
        ffnet_tcp_close(listen_fd);
        return 1;
    }
    printf("ok: read 'hi'\n");

    ffnet_tcp_close(client_fd);
    ffnet_tcp_close(listen_fd);

    int status;
    if (waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: child exited badly (status=%d)\n", status);
        return 1;
    }
    printf("ok: child clean exit\n");

    /* --- write-to-closed sub-test -------------------------------------
     * Exercises that a real libc write() failing with EPIPE round-trips
     * through ffutil_map_errno into FFE_PIPE without losing errno. */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            perror("socketpair");
            return 1;
        }
        close(sv[0]);                              /* peer end gone */
        ssize_t w = write(sv[1], "x", 1);          /* expect EPIPE */
        int saved_errno = errno;
        close(sv[1]);
        if (w != -1 || saved_errno != EPIPE) {
            fprintf(stderr, "FAIL: write-to-closed: w=%zd errno=%d\n",
                    w, saved_errno);
            return 1;
        }
        int rc = ffutil_map_errno(-saved_errno);
        if (rc != FFE_PIPE) {
            fprintf(stderr, "FAIL: map_errno(EPIPE)=%d, want FFE_PIPE=%d\n",
                    rc, FFE_PIPE);
            return 1;
        }
        printf("ok: write-to-closed -> FFE_PIPE\n");
    }

    return 0;
}

// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffnet.h"
#include "ffnet/ffutil.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails = 0;
static int reads_fired = 0;

static void rd_cb(FfEvLoop *loop, int fd, uint32_t events, void *ctx)
{
    (void)ctx;
    if (events & FF_EV_READ) {
        char buf[32];
        ssize_t n = read(fd, buf, sizeof buf);
        (void)n;
        reads_fired++;
    }
    /* Stop after first wake so the test terminates. */
    ffnet_evloop_stop(loop);
}

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return 1;
    }

    FfEvLoop *L = ffnet_evloop_new();
    if (!L) {
        fprintf(stderr, "FAIL: evloop_new returned NULL\n");
        return 1;
    }
    printf("ok: evloop_new\n");

    int rc = ffnet_evloop_add(L, sv[0], FF_EV_READ, rd_cb, NULL);
    if (rc != FFE_OK) {
        fprintf(stderr, "FAIL: evloop_add: %s\n", ffutil_strerror(rc));
        fails++;
    } else {
        printf("ok: evloop_add\n");
    }

    /* Write to the other end; the loop should wake and call rd_cb once. */
    if (write(sv[1], "x", 1) != 1) {
        fprintf(stderr, "FAIL: write to pipe\n");
        fails++;
    }

    rc = ffnet_evloop_run(L);
    if (rc != FFE_OK) {
        fprintf(stderr, "FAIL: evloop_run: %s\n", ffutil_strerror(rc));
        fails++;
    } else if (reads_fired != 1) {
        fprintf(stderr, "FAIL: reads_fired=%d (want 1)\n", reads_fired);
        fails++;
    } else {
        printf("ok: evloop_run + callback fired\n");
    }

    rc = ffnet_evloop_mod(L, sv[0], FF_EV_READ | FF_EV_WRITE);
    if (rc != FFE_OK) {
        fprintf(stderr, "FAIL: evloop_mod: %s\n", ffutil_strerror(rc));
        fails++;
    } else {
        printf("ok: evloop_mod\n");
    }

    rc = ffnet_evloop_del(L, sv[0]);
    if (rc != FFE_OK) {
        fprintf(stderr, "FAIL: evloop_del: %s\n", ffutil_strerror(rc));
        fails++;
    } else {
        printf("ok: evloop_del\n");
    }

    /* Operations on deleted fd should return FFE_INVAL. */
    rc = ffnet_evloop_mod(L, sv[0], FF_EV_READ);
    if (rc != FFE_INVAL) {
        fprintf(stderr, "FAIL: mod-after-del: rc=%d (want FFE_INVAL)\n", rc);
        fails++;
    } else {
        printf("ok: mod-after-del returns FFE_INVAL\n");
    }

    ffnet_evloop_free(L);
    printf("ok: evloop_free\n");

    close(sv[0]);
    close(sv[1]);
    return fails == 0 ? 0 : 1;
}

// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffhttp.h"

int ffhttp_echo_handler(const HttpRequest *req, HttpResponse *resp, void *user)
{
    (void)user;
    ffhttp_resp_status(resp, 200);
    ffhttp_resp_header(resp, "Content-Type", "application/octet-stream");

    size_t n;
    const uint8_t *body = ffhttp_req_body(req, &n);
    if (body && n > 0) {
        ffhttp_resp_body(resp, body, n);
    }
    return 0;
}

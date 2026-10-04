#include <stdio.h>
#include <stdlib.h>

#include "apps/apps.h"
#include "core/sha256.h"
#include "core/util.h"
#include "sim/simnet.h"
#include "test.h"

struct client {
    char resp[65536];
    size_t len;
};

static void client_readable(struct us_sock *s)
{
    struct client *c = us_ctx(s);
    c->len += us_read(s, c->resp + c->len, sizeof(c->resp) - 1 - c->len);
    c->resp[c->len] = '\0';
}

static const struct us_tcp_ops client_ops = { .on_readable = client_readable };

static struct us_sock *http_pair(struct simnet *net, struct client *cl)
{
    struct stack_config ca, cb;
    stack_config_defaults(&ca);
    stack_config_defaults(&cb);
    ca.addr = SIM_ADDR_A;
    cb.addr = SIM_ADDR_B;
    simnet_init(net, 3);
    net->link[0].delay_ns = net->link[1].delay_ns = 100 * NSEC_PER_USEC;
    struct us_stack *a = simnet_attach(net, 0, &ca);
    http_listen(simnet_attach(net, 1, &cb), APP_HTTP_PORT);
    memset(cl, 0, sizeof(*cl));
    return us_connect(a, SIM_ADDR_B, APP_HTTP_PORT, &client_ops, cl);
}

static void settle(struct simnet *net)
{
    uint64_t end = net->now + NSEC_PER_SEC;
    while (simnet_step_until(net, end))
        ;
}

TEST(http_headers_and_body_in_one_segment)
{
    struct simnet net;
    struct client cl;
    struct us_sock *s = http_pair(&net, &cl);
    char body[3000];
    for (size_t i = 0; i < sizeof(body); i++)
        body[i] = (char)(i * 31);
    char req[4096];
    int hl = snprintf(req, sizeof(req), "POST /sha256 HTTP/1.1\r\nContent-Length: %zu\r\n\r\n",
                      sizeof(body));
    memcpy(req + hl, body, sizeof(body));
    us_write(s, req, (size_t)hl + sizeof(body));
    settle(&net);

    struct sha256 sha;
    uint8_t digest[SHA256_DIGEST_LEN];
    char hex[2 * SHA256_DIGEST_LEN + 1];
    sha256_init(&sha);
    sha256_update(&sha, body, sizeof(body));
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    CHECK(strstr(cl.resp, "200 OK") != NULL);
    CHECK(strstr(cl.resp, hex) != NULL);
    simnet_free(&net);
}

TEST(http_pipelined_requests_answered_in_order)
{
    struct simnet net;
    struct client cl;
    struct us_sock *s = http_pair(&net, &cl);
    const char *reqs = "GET /bytes/5 HTTP/1.1\r\n\r\nGET /nope HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n";
    us_write(s, reqs, strlen(reqs));
    settle(&net);
    char *first = strstr(cl.resp, "200 OK");
    char *second = strstr(cl.resp, "404 Not Found");
    char *third = second ? strstr(second, "Hello from ustack") : NULL;
    CHECK(first && second && third && first < second);
    simnet_free(&net);
}

TEST(http_rejects_oversized_request_line)
{
    struct simnet net;
    struct client cl;
    struct us_sock *s = http_pair(&net, &cl);
    char req[1024] = "GET /";
    memset(req + 5, 'a', 600);
    strcpy(req + 605, " HTTP/1.1\r\n\r\n");
    us_write(s, req, strlen(req));
    settle(&net);
    CHECK(strstr(cl.resp, "400 Bad Request") != NULL);
    simnet_free(&net);
}

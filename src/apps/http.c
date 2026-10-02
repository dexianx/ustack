#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "apps/apps.h"
#include "core/pattern.h"
#include "core/sha256.h"
#include "core/util.h"
#include "stack.h"

#define REQ_MAX 8192
#define CHUNK   65536

enum http_phase { READ_HEAD, READ_BODY, SEND_BODY };

struct http_conn {
    enum http_phase phase;
    bool close_after;
    uint64_t body_left;
    uint64_t send_off;
    uint64_t send_end;
    struct sha256 sha;
    size_t req_len;
    char req[REQ_MAX];
};

struct request {
    const char *method;
    const char *path;
    uint64_t content_length;
    bool keep_alive;
};

static const char INDEX_HTML[] =
    "<!doctype html><title>ustack</title>"
    "<h1>Hello from ustack</h1>"
    "<p>This page was served by a TCP/IP stack written from scratch in C.</p>\n";

static void respond(struct us_sock *s, struct http_conn *h, const char *status, const char *type,
                    const char *body, uint64_t body_len)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nServer: ustack\r\nContent-Type: %s\r\n"
                     "Content-Length: %" PRIu64 "\r\nConnection: %s\r\n\r\n",
                     status, type, body_len, h->close_after ? "close" : "keep-alive");
    us_write(s, head, (size_t)n);
    if (body)
        us_write(s, body, (size_t)body_len);
}

static void http_close(struct us_sock *s, struct http_conn *h)
{
    us_set_data(s, NULL);
    free(h);
    us_close(s);
}

static void pump_body(struct us_sock *s, struct http_conn *h)
{
    uint8_t buf[CHUNK];
    while (h->send_off < h->send_end) {
        size_t n = MIN(us_writable(s), (size_t)(h->send_end - h->send_off));
        n = MIN(n, sizeof(buf));
        if (!n)
            return;
        pattern_fill(h->send_off, buf, n);
        h->send_off += us_write(s, buf, n);
    }
    h->phase = READ_HEAD;
}

static bool parse_request(char *head, struct request *req)
{
    char *line_end = strstr(head, "\r\n");
    if (!line_end)
        return false;
    *line_end = '\0';
    char *sp1 = strchr(head, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2)
        return false;
    *sp1 = *sp2 = '\0';
    req->method = head;
    req->path = sp1 + 1;
    req->keep_alive = strcmp(sp2 + 1, "HTTP/1.0") != 0;
    req->content_length = 0;

    for (char *line = line_end + 2; *line;) {
        char *end = strstr(line, "\r\n");
        if (!end)
            break;
        *end = '\0';
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char *value = colon + 1;
            while (*value == ' ')
                value++;
            if (!strcasecmp(line, "Content-Length"))
                req->content_length = strtoull(value, NULL, 10);
            else if (!strcasecmp(line, "Connection"))
                req->keep_alive = strcasecmp(value, "close") != 0 &&
                                  (req->keep_alive || !strcasecmp(value, "keep-alive"));
        }
        line = end + 2;
    }
    return true;
}

static void finish_upload(struct us_sock *s, struct http_conn *h)
{
    uint8_t digest[SHA256_DIGEST_LEN];
    char hex[2 * SHA256_DIGEST_LEN + 2];
    sha256_final(&h->sha, digest);
    sha256_hex(digest, hex);
    strcat(hex, "\n");
    respond(s, h, "200 OK", "text/plain", hex, strlen(hex));
    h->phase = READ_HEAD;
}

static void handle(struct us_sock *s, struct http_conn *h, const struct request *req)
{
    h->close_after = !req->keep_alive;
    unsigned long long n;
    char tail;

    if (!strcmp(req->method, "POST") && !strcmp(req->path, "/sha256")) {
        sha256_init(&h->sha);
        h->body_left = req->content_length;
        h->phase = READ_BODY;
        if (!h->body_left)
            finish_upload(s, h);
        return;
    }
    if (strcmp(req->method, "GET")) {
        h->close_after = true;
        respond(s, h, "405 Method Not Allowed", "text/plain", "method not allowed\n", 19);
        return;
    }
    if (!strcmp(req->path, "/")) {
        respond(s, h, "200 OK", "text/html", INDEX_HTML, sizeof(INDEX_HTML) - 1);
    } else if (!strcmp(req->path, "/stats")) {
        char body[2048];
        size_t len = stack_stats_json(us_sock_stack(s), body, sizeof(body));
        respond(s, h, "200 OK", "application/json", body, len);
    } else if (sscanf(req->path, "/bytes/%llu%c", &n, &tail) == 1) {
        respond(s, h, "200 OK", "application/octet-stream", NULL, n);
        h->send_off = 0;
        h->send_end = n;
        h->phase = SEND_BODY;
        pump_body(s, h);
    } else {
        respond(s, h, "404 Not Found", "text/plain", "not found\n", 10);
    }
}

static void consume_req(struct http_conn *h, size_t n)
{
    memmove(h->req, h->req + n, h->req_len - n);
    h->req_len -= n;
}

static void feed_body(struct us_sock *s, struct http_conn *h)
{
    size_t take = (size_t)MIN((uint64_t)h->req_len, h->body_left);
    if (take) {
        sha256_update(&h->sha, h->req, take);
        h->body_left -= take;
        consume_req(h, take);
    }
    uint8_t buf[CHUNK];
    while (h->body_left) {
        size_t got = us_read(s, buf, (size_t)MIN((uint64_t)sizeof(buf), h->body_left));
        if (!got)
            return;
        sha256_update(&h->sha, buf, got);
        h->body_left -= got;
    }
    finish_upload(s, h);
}

static void http_process(struct us_sock *s)
{
    struct http_conn *h = us_data(s);
    for (;;) {
        if (h->phase == SEND_BODY)
            return;
        if (h->phase == READ_HEAD && h->close_after) {
            http_close(s, h);
            return;
        }
        if (h->phase == READ_BODY) {
            feed_body(s, h);
            if (h->phase == READ_BODY)
                break;
            continue;
        }

        h->req_len += us_read(s, h->req + h->req_len, REQ_MAX - 1 - h->req_len);
        h->req[h->req_len] = '\0';
        char *end = strstr(h->req, "\r\n\r\n");
        if (!end) {
            if (h->req_len == REQ_MAX - 1) {
                h->close_after = true;
                respond(s, h, "431 Request Header Fields Too Large", "text/plain", "", 0);
                http_close(s, h);
                return;
            }
            break;
        }
        size_t head_len = (size_t)(end - h->req) + 4;
        end[2] = '\0';
        struct request req;
        bool ok = parse_request(h->req, &req);
        consume_req(h, head_len);
        if (!ok) {
            h->close_after = true;
            respond(s, h, "400 Bad Request", "text/plain", "bad request\n", 12);
            continue;
        }
        handle(s, h, &req);
    }
    if (us_eof(s) && h->phase == READ_HEAD)
        http_close(s, h);
}

static void http_open(struct us_sock *s)
{
    struct http_conn *h = calloc(1, sizeof(*h));
    if (!h) {
        us_abort(s);
        return;
    }
    us_set_nodelay(s, true);
    us_set_data(s, h);
}

static void http_writable(struct us_sock *s)
{
    struct http_conn *h = us_data(s);
    if (h->phase != SEND_BODY)
        return;
    pump_body(s, h);
    if (h->phase != SEND_BODY)
        http_process(s);
}

static void http_closed(struct us_sock *s, enum us_err err)
{
    (void)err;
    free(us_data(s));
}

static const struct us_tcp_ops http_ops = {
    .on_open = http_open,
    .on_readable = http_process,
    .on_writable = http_writable,
    .on_close = http_closed,
};

int http_listen(struct us_stack *st, uint16_t port)
{
    pattern_init();
    return us_listen(st, port, &http_ops, NULL);
}

size_t stack_stats_json(const struct us_stack *st, char *buf, size_t cap)
{
    const struct stack_stats *s = &st->stats;
    int n = snprintf(buf, cap,
                     "{\"rx_packets\":%" PRIu64 ",\"tx_packets\":%" PRIu64
                     ",\"tcp_rx_segs\":%" PRIu64 ",\"tcp_tx_segs\":%" PRIu64
                     ",\"tcp_retrans\":%" PRIu64 ",\"tcp_rto\":%" PRIu64 ",\"tcp_tlp\":%" PRIu64
                     ",\"tcp_recoveries\":%" PRIu64 ",\"tcp_ooo\":%" PRIu64
                     ",\"tcp_opened\":%" PRIu64 ",\"tcp_closed\":%" PRIu64
                     ",\"tcp_bad\":%" PRIu64 ",\"ip_bad\":%" PRIu64 ",\"active_conns\":%u}\n",
                     s->rx_packets, s->tx_packets, s->tcp_rx_segs, s->tcp_tx_segs, s->tcp_retrans,
                     s->tcp_rto, s->tcp_tlp, s->tcp_recoveries, s->tcp_ooo, s->tcp_opened,
                     s->tcp_closed, s->tcp_bad, s->ip_bad, st->tcp.count);
    return n < 0 ? 0 : MIN((size_t)n, cap - 1);
}

#include <stdlib.h>

#include "apps/apps.h"
#include "core/util.h"

#define CHUNK 65536

static void echo_pump(struct us_sock *s)
{
    uint8_t buf[CHUNK];
    for (;;) {
        size_t n = MIN(us_writable(s), us_readable(s));
        n = MIN(n, sizeof(buf));
        if (!n)
            break;
        us_write(s, buf, us_read(s, buf, n));
    }
    if (us_eof(s))
        us_close(s);
}

static void echo_open(struct us_sock *s)
{
    us_set_nodelay(s, true);
}

static void discard_readable(struct us_sock *s)
{
    uint8_t buf[CHUNK];
    while (us_read(s, buf, sizeof(buf)))
        ;
    if (us_eof(s))
        us_close(s);
}

static const struct us_tcp_ops echo_ops = {
    .on_open = echo_open,
    .on_readable = echo_pump,
    .on_writable = echo_pump,
};

static const struct us_tcp_ops discard_ops = {
    .on_readable = discard_readable,
};

int echo_listen(struct us_stack *st, uint16_t port)
{
    return us_listen(st, port, &echo_ops, NULL);
}

int discard_listen(struct us_stack *st, uint16_t port)
{
    return us_listen(st, port, &discard_ops, NULL);
}

static void udp_echo(struct us_stack *st, void *ctx, uint32_t saddr, uint16_t sport,
                     uint16_t dport, const uint8_t *data, size_t len)
{
    (void)ctx;
    us_udp_send(st, dport, saddr, sport, data, len);
}

void apps_register(struct us_stack *st)
{
    http_listen(st, APP_HTTP_PORT);
    echo_listen(st, APP_ECHO_PORT);
    discard_listen(st, APP_DISCARD_PORT);
    us_udp_bind(st, APP_ECHO_PORT, udp_echo, NULL);
}

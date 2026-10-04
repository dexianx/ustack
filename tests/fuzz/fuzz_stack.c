#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/apps.h"
#include "core/checksum.h"
#include "core/pattern.h"
#include "core/util.h"
#include "net/proto.h"
#include "sim/simnet.h"
#include "tcp/tcp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct reader {
    const uint8_t *p;
    size_t n;
};

static uint8_t u8(struct reader *r)
{
    if (!r->n)
        return 0;
    r->n--;
    return *r->p++;
}

static uint16_t u16(struct reader *r) { return (uint16_t)(u8(r) << 8 | u8(r)); }
static uint32_t u32(struct reader *r) { return (uint32_t)u16(r) << 16 | u16(r); }

static size_t take(struct reader *r, size_t want, const uint8_t **out)
{
    size_t n = MIN(want, r->n);
    *out = r->p;
    r->p += n;
    r->n -= n;
    return n;
}

static struct us_sock *server_sock;

static void srv_open(struct us_sock *s) { server_sock = s; }
static void srv_close(struct us_sock *s, enum us_err e) { (void)s, (void)e; server_sock = NULL; }

static const struct us_tcp_ops srv_ops = { .on_open = srv_open, .on_close = srv_close };
static const struct us_tcp_ops cli_ops = { 0 };

static void check(struct simnet *net)
{
    char why[160];
    if (!simnet_check_all(net, why, sizeof(why))) {
        fprintf(stderr, "invariant violated: %s\n", why);
        abort();
    }
}

static void deliver(struct simnet *net, int steps)
{
    while (steps-- > 0 && simnet_step(net))
        check(net);
}

static void send_raw(struct simnet *net, struct reader *r)
{
    static uint8_t pkt[2048] __attribute__((aligned(8)));
    const uint8_t *src;
    size_t n = take(r, u16(r) % sizeof(pkt), &src);
    memcpy(pkt, src, n);
    stack_input(net->node[1], pkt, n, u8(r) & 1);
    stack_flush(net->node[1]);
}

static void send_segment(struct simnet *net, struct tcp_conn *peer, struct reader *r)
{
    static uint8_t pkt[2048] __attribute__((aligned(8)));
    struct ipv4_hdr *ip = (struct ipv4_hdr *)pkt;
    struct tcp_hdr *th = (struct tcp_hdr *)(pkt + IPV4_HDR_LEN);
    uint8_t flags = u8(r);
    int32_t seq_off = (int32_t)u32(r) >> (u8(r) % 32);
    int32_t ack_off = (int32_t)u32(r) >> (u8(r) % 32);
    uint16_t wnd = u16(r);
    size_t optlen = (u8(r) % 11) * 4;
    const uint8_t *opts, *payload;
    size_t got = take(r, optlen, &opts);
    size_t plen = take(r, u16(r) % 1400, &payload);
    size_t tcplen = TCP_HDR_LEN + optlen + plen;

    *ip = (struct ipv4_hdr){ .ver_ihl = 0x45, .total_len = htons((uint16_t)(IPV4_HDR_LEN + tcplen)),
                             .ttl = 64, .proto = IPPROTO_TCP_, .saddr = htonl(SIM_ADDR_A),
                             .daddr = htonl(SIM_ADDR_B) };
    ip->csum = csum_compute(ip, IPV4_HDR_LEN);
    *th = (struct tcp_hdr){ .sport = htons(peer->lport), .dport = htons(peer->rport),
                            .seq = htonl(peer->snd_nxt + (uint32_t)seq_off),
                            .ack = htonl(peer->rcv_nxt + (uint32_t)ack_off),
                            .doff = (uint8_t)(((TCP_HDR_LEN + optlen) / 4) << 4), .flags = flags,
                            .wnd = htons(wnd) };
    memset(th + 1, 1, optlen);
    memcpy(th + 1, opts, got);
    memcpy((uint8_t *)(th + 1) + optlen, payload, plen);
    th->csum = csum_fold(csum_add(csum_pseudo(SIM_ADDR_A, SIM_ADDR_B, IPPROTO_TCP_, (uint16_t)tcplen),
                                  th, tcplen));
    stack_input(net->node[1], pkt, IPV4_HDR_LEN + tcplen, false);
    stack_flush(net->node[1]);
}

static void app_action(struct simnet *net, struct reader *r)
{
    static uint8_t buf[65536];
    if (!server_sock)
        return;
    switch (u8(r) % 4) {
    case 0:
        us_read(server_sock, buf, u16(r));
        break;
    case 1: {
        size_t n = u16(r);
        pattern_fill(0, buf, n);
        us_write(server_sock, buf, n);
        break;
    }
    case 2:
        us_shutdown(server_sock);
        break;
    default:
        us_close(server_sock);
        server_sock = NULL;
        break;
    }
    stack_flush(net->node[1]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct reader r = { data, size };
    struct simnet net;
    struct stack_config ca, cb;
    stack_config_defaults(&ca);
    stack_config_defaults(&cb);
    ca.addr = SIM_ADDR_A;
    cb.addr = SIM_ADDR_B;
    ca.sndbuf = ca.rcvbuf = cb.sndbuf = cb.rcvbuf = 1u << 16;
    cb.sack = u8(&r) & 1;
    cb.timestamps = u8(&r) & 1;
    pattern_init();

    simnet_init(&net, 1);
    net.link[0].delay_ns = net.link[1].delay_ns = 100 * NSEC_PER_USEC;
    struct us_stack *a = simnet_attach(&net, 0, &ca);
    struct us_stack *b = simnet_attach(&net, 1, &cb);
    us_listen(b, 80, &srv_ops, NULL);
    apps_register(b);
    server_sock = NULL;
    us_connect(a, SIM_ADDR_B, 80, &cli_ops, NULL);
    stack_flush(a);
    deliver(&net, 10);

    struct tcp_conn *first = NULL;
    for (uint32_t i = 0; i <= a->tcp.mask && !first; i++)
        first = a->tcp.buckets[i];
    uint16_t lport = first ? first->lport : 0;

    while (r.n) {
        switch (u8(&r) % 6) {
        case 0:
            send_raw(&net, &r);
            break;
        case 1:
        case 2:
        {
            struct tcp_conn *peer = tcp_lookup(a, SIM_ADDR_B, 80, lport);
            if (peer)
                send_segment(&net, peer, &r);
        }
            break;
        case 3:
            app_action(&net, &r);
            break;
        case 4:
            stack_advance(b, b->now_ns + (uint64_t)u16(&r) * NSEC_PER_MSEC);
            net.now = MAX(net.now, b->now_ns);
            break;
        default:
            deliver(&net, u8(&r));
            break;
        }
        check(&net);
    }
    simnet_free(&net);
    return 0;
}

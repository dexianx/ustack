#include <arpa/inet.h>
#include <stdlib.h>

#include "apps/apps.h"
#include "core/checksum.h"
#include "core/pattern.h"
#include "core/util.h"
#include "net/proto.h"
#include "sim/simnet.h"
#include "tcp/tcp.h"
#include "test.h"

struct peer {
    struct us_sock *s;
    bool opened;
    bool closed;
    enum us_err err;
    bool read_enabled;
    uint64_t rcvd;
    uint64_t to_send;
    uint64_t sent;
    bool close_when_sent;
};

static void peer_pump(struct us_sock *s)
{
    struct peer *p = us_ctx(s);
    uint8_t buf[4096];
    while (p->sent < p->to_send) {
        size_t n = us_writable(s);
        n = n < sizeof(buf) ? n : sizeof(buf);
        if (n > p->to_send - p->sent)
            n = (size_t)(p->to_send - p->sent);
        if (!n)
            return;
        pattern_fill(p->sent, buf, n);
        p->sent += us_write(s, buf, n);
    }
    if (p->close_when_sent)
        us_close(s);
}

static void peer_open(struct us_sock *s)
{
    struct peer *p = us_ctx(s);
    p->s = s;
    p->opened = true;
    peer_pump(s);
}

static void peer_readable(struct us_sock *s)
{
    struct peer *p = us_ctx(s);
    if (!p->read_enabled)
        return;
    uint8_t buf[4096], expect[4096];
    size_t n;
    while ((n = us_read(s, buf, sizeof(buf)))) {
        pattern_fill(p->rcvd, expect, n);
        if (memcmp(buf, expect, n))
            FAIL_AT("payload mismatch at offset %" PRIu64, p->rcvd);
        p->rcvd += n;
    }
}

static void peer_closed(struct us_sock *s, enum us_err err)
{
    struct peer *p = us_ctx(s);
    p->closed = true;
    p->err = err;
}

static const struct us_tcp_ops peer_ops = {
    .on_open = peer_open,
    .on_readable = peer_readable,
    .on_writable = peer_pump,
    .on_close = peer_closed,
};

struct pair {
    struct simnet net;
    struct us_stack *a;
    struct us_stack *b;
};

static void pair_init(struct pair *p, uint64_t start_ns, void (*tweak)(struct stack_config *))
{
    struct stack_config ca, cb;
    stack_config_defaults(&ca);
    stack_config_defaults(&cb);
    ca.addr = SIM_ADDR_A;
    cb.addr = SIM_ADDR_B;
    cb.seed = 77;
    ca.msl_ms = cb.msl_ms = 500;
    if (tweak) {
        tweak(&ca);
        tweak(&cb);
    }
    simnet_init(&p->net, 1);
    if (start_ns)
        p->net.now = start_ns;
    struct sim_link l = { .delay_ns = 500 * NSEC_PER_USEC };
    p->net.link[0] = p->net.link[1] = l;
    p->a = simnet_attach(&p->net, 0, &ca);
    p->b = simnet_attach(&p->net, 1, &cb);
    p->net.trace = getenv("SIM_TRACE") != NULL;
    pattern_init();
}

static void run_for(struct pair *p, uint64_t ns)
{
    uint64_t end = p->net.now + ns;
    while (simnet_step_until(&p->net, end))
        ;
}

static struct tcp_conn *only_conn(struct us_stack *st)
{
    for (uint32_t b = 0; b <= st->tcp.mask; b++)
        if (st->tcp.buckets[b])
            return st->tcp.buckets[b];
    return NULL;
}

TEST(tcp_handshake_negotiates_options)
{
    struct pair p;
    struct peer srv = { .read_enabled = true }, cli = { .read_enabled = true };
    pair_init(&p, 0, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli);
    run_for(&p, 50 * NSEC_PER_MSEC);

    CHECK(cli.opened && srv.opened);
    struct tcp_conn *ca = only_conn(p.a), *cb = only_conn(p.b);
    CHECK(ca && cb);
    if (ca && cb) {
        CHECK_EQ(ca->state, TCP_ESTABLISHED);
        CHECK_EQ(cb->state, TCP_ESTABLISHED);
        CHECK(ca->sack_ok && ca->ts_ok && ca->ws_ok);
        CHECK(cb->sack_ok && cb->ts_ok && cb->ws_ok);
        CHECK_EQ(ca->snd_wscale, cb->rcv_wscale);
        CHECK_EQ(ca->rcv_wscale, cb->snd_wscale);
        CHECK_EQ(ca->mss, 1460 - TCP_OPT_TS_LEN);
        CHECK_EQ(ca->irs, cb->iss);
        CHECK_EQ(cb->irs, ca->iss);
        CHECK(ca->srtt_us >= 1000 && ca->srtt_us < 1500);
    }
    simnet_free(&p.net);
}

static void no_options(struct stack_config *cfg)
{
    cfg->sack = cfg->timestamps = cfg->wscale = false;
}

TEST(tcp_options_fall_back_when_peer_declines)
{
    struct pair p;
    struct peer srv = { 0 }, cli = { 0 };
    pair_init(&p, 0, no_options);
    p.a->cfg.sack = p.a->cfg.timestamps = p.a->cfg.wscale = true;
    us_listen(p.b, 80, &peer_ops, &srv);
    us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli);
    run_for(&p, 50 * NSEC_PER_MSEC);
    struct tcp_conn *ca = only_conn(p.a);
    CHECK(ca && ca->state == TCP_ESTABLISHED);
    if (ca) {
        CHECK(!ca->sack_ok && !ca->ts_ok && !ca->ws_ok);
        CHECK_EQ(ca->mss, 1460);
    }
    simnet_free(&p.net);
}

TEST(tcp_connect_to_closed_port_is_refused)
{
    struct pair p;
    struct peer cli = { 0 };
    pair_init(&p, 0, NULL);
    us_connect(p.a, SIM_ADDR_B, 81, &peer_ops, &cli);
    run_for(&p, 50 * NSEC_PER_MSEC);
    CHECK(cli.closed);
    CHECK_EQ(cli.err, US_EREFUSED);
    CHECK_EQ(p.b->stats.tcp_rst_tx, 1);
    CHECK_EQ(p.a->tcp.count, 0);
    simnet_free(&p.net);
}

TEST(tcp_transfer_survives_sequence_wraparound)
{
    struct pair probe;
    struct peer dummy = { 0 };
    pair_init(&probe, 0, NULL);
    us_connect(probe.a, SIM_ADDR_B, 80, &peer_ops, &dummy);
    seq_t iss = only_conn(probe.a)->iss;
    uint64_t start = probe.net.now;
    simnet_free(&probe.net);

    uint32_t advance = (uint32_t)(0u - 20000u - iss);
    struct pair p;
    struct peer srv = { .read_enabled = true }, cli = { .to_send = 200000 };
    pair_init(&p, start + (uint64_t)advance * 4000, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli);
    struct tcp_conn *ca = only_conn(p.a);
    CHECK(ca && ca->iss > 0xfffe0000u);
    run_for(&p, 2 * NSEC_PER_SEC);
    CHECK_EQ(srv.rcvd, 200000);
    if (ca)
        CHECK(seq_lt(ca->snd_una, 0x10000000u) && ca->snd_una < 0x00100000u);
    simnet_free(&p.net);
}

TEST(tcp_zero_window_probe_and_reopen)
{
    struct pair p;
    struct peer srv = { .read_enabled = false }, cli = { .to_send = 3 << 20 };
    pair_init(&p, 0, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli);
    run_for(&p, 3 * NSEC_PER_SEC);

    struct tcp_conn *ca = only_conn(p.a), *cb = only_conn(p.b);
    CHECK(ca && cb);
    if (!ca || !cb) {
        simnet_free(&p.net);
        return;
    }
    CHECK_EQ(ca->snd_wnd, 0);
    CHECK(cb->rcvbuf.cap - cb->rcvbuf.len < cb->mss);
    CHECK_EQ(ca->rtx_mode, RTX_PERSIST);
    uint64_t probes = p.a->stats.tcp_tx_segs;
    run_for(&p, 3 * NSEC_PER_SEC);
    CHECK(p.a->stats.tcp_tx_segs > probes);
    CHECK_EQ(cb->state, TCP_ESTABLISHED);

    srv.read_enabled = true;
    peer_readable(srv.s);
    run_for(&p, 5 * NSEC_PER_SEC);
    CHECK_EQ(srv.rcvd, 3 << 20);
    simnet_free(&p.net);
}

TEST(tcp_simultaneous_close_reaches_time_wait_then_frees)
{
    struct pair p;
    struct peer srv = { .read_enabled = true }, cli = { .read_enabled = true };
    pair_init(&p, 0, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli);
    run_for(&p, 50 * NSEC_PER_MSEC);
    struct tcp_conn *ca = only_conn(p.a), *cb = only_conn(p.b);
    us_close(cli.s);
    us_close(srv.s);
    stack_flush(p.a);
    stack_flush(p.b);
    run_for(&p, 20 * NSEC_PER_MSEC);
    CHECK_EQ(ca->state, TCP_TIME_WAIT);
    CHECK_EQ(cb->state, TCP_TIME_WAIT);
    run_for(&p, 1100 * NSEC_PER_MSEC);
    CHECK_EQ(p.a->tcp.count, 0);
    CHECK_EQ(p.b->tcp.count, 0);
    simnet_free(&p.net);
}

TEST(tcp_passive_close_skips_time_wait)
{
    struct pair p;
    struct peer srv = { .read_enabled = true }, cli = { .to_send = 1000, .close_when_sent = true };
    pair_init(&p, 0, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli);
    run_for(&p, 50 * NSEC_PER_MSEC);
    struct tcp_conn *cb = only_conn(p.b);
    CHECK(cb && cb->state == TCP_CLOSE_WAIT);
    CHECK(us_eof(srv.s));
    us_close(srv.s);
    stack_flush(p.b);
    run_for(&p, 20 * NSEC_PER_MSEC);
    CHECK_EQ(p.b->tcp.count, 0);
    CHECK_EQ(only_conn(p.a)->state, TCP_TIME_WAIT);
    CHECK_EQ(srv.rcvd, 1000);
    simnet_free(&p.net);
}

static void inject_tcp(struct pair *p, const struct tcp_conn *from, uint8_t flags, seq_t seq,
                       uint32_t tsval)
{
    uint8_t pkt[IPV4_HDR_LEN + TCP_HDR_LEN + 12] __attribute__((aligned(4))) = { 0 };
    struct ipv4_hdr *ip = (struct ipv4_hdr *)pkt;
    struct tcp_hdr *th = (struct tcp_hdr *)(pkt + IPV4_HDR_LEN);
    size_t tcplen = TCP_HDR_LEN + (tsval ? 12 : 0);
    *ip = (struct ipv4_hdr){ .ver_ihl = 0x45, .total_len = htons((uint16_t)(IPV4_HDR_LEN + tcplen)),
                             .ttl = 64, .proto = IPPROTO_TCP_, .saddr = htonl(SIM_ADDR_A),
                             .daddr = htonl(SIM_ADDR_B) };
    ip->csum = csum_compute(ip, IPV4_HDR_LEN);
    *th = (struct tcp_hdr){ .sport = htons(from->lport), .dport = htons(from->rport),
                            .seq = htonl(seq), .ack = htonl(from->rcv_nxt),
                            .doff = (uint8_t)((tcplen / 4) << 4), .flags = flags,
                            .wnd = htons(1000) };
    if (tsval) {
        uint8_t *o = (uint8_t *)(th + 1);
        o[0] = o[1] = 1;
        o[2] = 8;
        o[3] = 10;
        uint32_t v = htonl(tsval), e = htonl(from->ts_recent);
        memcpy(o + 4, &v, 4);
        memcpy(o + 8, &e, 4);
    }
    th->csum = csum_fold(csum_add(csum_pseudo(SIM_ADDR_A, SIM_ADDR_B, IPPROTO_TCP_, (uint16_t)tcplen),
                                  th, tcplen));
    simnet_inject(&p->net, 1, pkt, (uint32_t)(IPV4_HDR_LEN + tcplen), p->net.now);
}

TEST(tcp_rfc5961_blind_reset_gets_challenge_ack)
{
    struct pair p;
    struct peer srv = { .read_enabled = true }, cli = { 0 };
    pair_init(&p, 0, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli);
    run_for(&p, 50 * NSEC_PER_MSEC);
    struct tcp_conn *ca = only_conn(p.a);

    inject_tcp(&p, ca, TH_RST, ca->snd_nxt + 100, 0);
    run_for(&p, 5 * NSEC_PER_MSEC);
    CHECK_EQ(p.b->stats.tcp_challenge_ack, 1);
    CHECK(!srv.closed);

    inject_tcp(&p, ca, TH_SYN, ca->snd_nxt + 5, 0);
    run_for(&p, 5 * NSEC_PER_MSEC);
    CHECK_EQ(p.b->stats.tcp_challenge_ack, 2);
    CHECK(!srv.closed);

    inject_tcp(&p, ca, TH_RST, ca->snd_nxt, 0);
    run_for(&p, 5 * NSEC_PER_MSEC);
    CHECK(srv.closed);
    CHECK_EQ(srv.err, US_ERESET);
    simnet_free(&p.net);
}

TEST(tcp_paws_drops_old_timestamps)
{
    struct pair p;
    struct peer srv = { .read_enabled = true }, cli = { 0 };
    pair_init(&p, 0, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli);
    run_for(&p, 50 * NSEC_PER_MSEC);
    struct tcp_conn *ca = only_conn(p.a), *cb = only_conn(p.b);
    inject_tcp(&p, ca, TH_ACK, ca->snd_nxt, cb->ts_recent - 1000);
    run_for(&p, 5 * NSEC_PER_MSEC);
    CHECK_EQ(p.b->stats.tcp_paws, 1);
    simnet_free(&p.net);
}

TEST(tcp_bad_checksum_is_dropped)
{
    struct pair p;
    struct peer srv = { 0 };
    pair_init(&p, 0, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    struct tcp_conn fake = { .lport = 1234, .rport = 80 };
    inject_tcp(&p, &fake, TH_SYN, 1, 0);
    p.net.heap[0].data[IPV4_HDR_LEN + 4] ^= 0x40;
    run_for(&p, 5 * NSEC_PER_MSEC);
    CHECK_EQ(p.b->stats.tcp_bad, 1);
    CHECK_EQ(p.b->tcp.count, 0);
    simnet_free(&p.net);
}

TEST(tcp_ten_thousand_concurrent_connections)
{
    struct pair p;
    static struct peer cli[10000];
    struct peer srv = { .read_enabled = false };
    pair_init(&p, 0, NULL);
    us_listen(p.b, 80, &peer_ops, &srv);
    memset(cli, 0, sizeof(cli));
    for (int i = 0; i < 10000; i++) {
        cli[i].to_send = 100;
        us_connect(p.a, SIM_ADDR_B, 80, &peer_ops, &cli[i]);
    }
    stack_flush(p.a);
    run_for(&p, NSEC_PER_SEC);
    int opened = 0;
    for (int i = 0; i < 10000; i++)
        opened += cli[i].opened;
    CHECK_EQ(opened, 10000);
    CHECK_EQ(p.b->tcp.count, 10000);
    uint64_t buffered = 0;
    for (uint32_t b = 0; b <= p.b->tcp.mask; b++)
        for (struct tcp_conn *c = p.b->tcp.buckets[b]; c; c = c->hnext)
            buffered += c->rcvbuf.len;
    CHECK_EQ(buffered, 10000 * 100);
    simnet_free(&p.net);
}

TEST(tcp_option_parser_rejects_malformed)
{
    struct tcp_opts o;
    const uint8_t bad_len[] = { 2, 10, 0, 0 };
    const uint8_t zero_len[] = { 8, 0 };
    const uint8_t ok[] = { 2, 4, 0x05, 0xb4, 1, 3, 3, 7, 4, 2, 0 };
    CHECK(!tcp_parse_options(bad_len, sizeof(bad_len), &o));
    CHECK(!tcp_parse_options(zero_len, sizeof(zero_len), &o));
    CHECK(tcp_parse_options(ok, sizeof(ok), &o));
    CHECK(o.has_mss && o.mss == 1460);
    CHECK(o.has_wscale && o.wscale == 7);
    CHECK(o.sack_ok);
}

static void udp_capture(struct us_stack *st, void *ctx, uint32_t saddr, uint16_t sport,
                        uint16_t dport, const uint8_t *data, size_t len)
{
    (void)st, (void)saddr, (void)sport, (void)dport;
    memcpy(ctx, data, len < 15 ? len : 15);
}

TEST(udp_echo_and_icmp_port_unreachable)
{
    struct pair p;
    char got[16] = { 0 };
    pair_init(&p, 0, NULL);
    apps_register(p.b);
    us_udp_bind(p.a, 5000, udp_capture, got);
    us_udp_send(p.a, 5000, SIM_ADDR_B, APP_ECHO_PORT, "ping over udp", 13);
    us_udp_send(p.a, 5000, SIM_ADDR_B, 9999, "nobody home", 11);
    run_for(&p, 10 * NSEC_PER_MSEC);
    CHECK_STR(got, "ping over udp");
    CHECK_EQ(p.b->stats.udp_no_port, 1);
    CHECK_EQ(p.a->stats.ip_unknown_proto + p.a->stats.icmp_bad, 0);
    simnet_free(&p.net);
}

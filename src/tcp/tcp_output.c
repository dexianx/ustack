#include <arpa/inet.h>
#include <string.h>

#include "core/checksum.h"
#include "core/util.h"
#include "net/proto.h"
#include "tcp/tcp.h"

#define TCP_MAX_OPT_LEN 40

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint32_t tcp_ts_now(const struct tcp_conn *c)
{
    return (uint32_t)stack_now_ms(c->st) + c->ts_offset;
}

static size_t put_ts(const struct tcp_conn *c, uint8_t *p)
{
    p[0] = 1;
    p[1] = 1;
    p[2] = 8;
    p[3] = 10;
    put32(p + 4, tcp_ts_now(c));
    put32(p + 8, c->ts_recent);
    return TCP_OPT_TS_LEN;
}

static size_t syn_options(const struct tcp_conn *c, uint8_t *p)
{
    const struct stack_config *cfg = &c->st->cfg;
    bool synack = c->state == TCP_SYN_RCVD;
    bool ws = synack ? c->ws_ok : cfg->wscale;
    bool sack = synack ? c->sack_ok : cfg->sack;
    bool ts = synack ? c->ts_ok : cfg->timestamps;
    size_t n = 0;

    p[n++] = 2;
    p[n++] = 4;
    put16(p + n, (uint16_t)(cfg->mtu - IPV4_HDR_LEN - TCP_HDR_LEN));
    n += 2;
    if (ts) {
        if (sack) {
            p[n++] = 4;
            p[n++] = 2;
        } else {
            p[n++] = 1;
            p[n++] = 1;
        }
        p[n++] = 8;
        p[n++] = 10;
        put32(p + n, tcp_ts_now(c));
        put32(p + n + 4, synack ? c->ts_recent : 0);
        n += 8;
    } else if (sack) {
        p[n++] = 1;
        p[n++] = 1;
        p[n++] = 4;
        p[n++] = 2;
    }
    if (ws) {
        uint8_t shift = 0;
        while (shift < TCP_MAX_WSCALE && (c->rcvbuf.cap >> shift) > 65535)
            shift++;
        p[n++] = 1;
        p[n++] = 3;
        p[n++] = 3;
        p[n++] = shift;
    }
    return n;
}

static size_t sack_options(const struct tcp_conn *c, uint8_t *p, size_t room)
{
    if (!c->sack_ok || seqset_empty(&c->ooo) || room < 12)
        return 0;
    size_t max_blocks = MIN((room - 4) / 8, (size_t)TCP_MAX_SACKS);

    struct seq_range blocks[TCP_MAX_SACKS];
    size_t nb = 0;
    const struct seq_range *recent = seqset_find(&c->ooo, c->last_ooo.start);
    if (recent)
        blocks[nb++] = *recent;
    for (uint32_t i = c->ooo.n; i-- > 0 && nb < max_blocks;)
        if (&c->ooo.r[i] != recent)
            blocks[nb++] = c->ooo.r[i];

    p[0] = 1;
    p[1] = 1;
    p[2] = 5;
    p[3] = (uint8_t)(2 + 8 * nb);
    for (size_t i = 0; i < nb; i++) {
        put32(p + 4 + 8 * i, blocks[i].start);
        put32(p + 8 + 8 * i, blocks[i].end);
    }
    return 4 + 8 * nb;
}

uint32_t tcp_rcv_window(struct tcp_conn *c)
{
    uint32_t room = ring_space(&c->rcvbuf);
    seq_t right = c->rcv_nxt + room;
    uint32_t sws = MIN(c->rcvbuf.cap / 2, (uint32_t)c->mss);
    if (seq_lt(right, c->rcv_adv + sws) && seq_ge(c->rcv_adv, c->rcv_nxt))
        right = c->rcv_adv;
    uint32_t wnd = MIN(right - c->rcv_nxt, 65535u << c->rcv_wscale);
    wnd = (wnd >> c->rcv_wscale) << c->rcv_wscale;
    c->rcv_adv = seq_max(c->rcv_adv, c->rcv_nxt + wnd);
    return wnd;
}

static void xmit(struct tcp_conn *c, seq_t seq, uint8_t flags, uint32_t data_len, uint32_t buf_off)
{
    struct us_stack *st = c->st;
    uint8_t *pkt = st->txbuf;
    struct tcp_hdr *th = (struct tcp_hdr *)(pkt + IPV4_HDR_LEN);
    uint8_t *opt = (uint8_t *)(th + 1);
    size_t olen;

    if (flags & TH_SYN) {
        olen = syn_options(c, opt);
    } else {
        olen = c->ts_ok ? put_ts(c, opt) : 0;
        if (flags & TH_ACK)
            olen += sack_options(c, opt + olen, TCP_MAX_OPT_LEN - olen);
    }
    while (olen & 3)
        opt[olen++] = 1;

    size_t hlen = TCP_HDR_LEN + olen;
    uint32_t wnd = flags & TH_SYN ? MIN(ring_space(&c->rcvbuf), 65535u) : tcp_rcv_window(c) >> c->rcv_wscale;
    if (flags & TH_SYN)
        c->rcv_adv = c->rcv_nxt + wnd;
    th->sport = htons(c->lport);
    th->dport = htons(c->rport);
    th->seq = htonl(seq);
    th->ack = htonl(flags & TH_ACK ? c->rcv_nxt : 0);
    th->doff = (uint8_t)((hlen / 4) << 4);
    th->flags = flags;
    th->wnd = htons((uint16_t)wnd);
    th->csum = 0;
    th->urg = 0;
    if (data_len)
        ring_read_at(&c->sndbuf, buf_off, pkt + IPV4_HDR_LEN + hlen, data_len);

    size_t tcplen = hlen + data_len;
    uint64_t pseudo = csum_pseudo(st->cfg.addr, c->raddr, IPPROTO_TCP_, (uint16_t)tcplen);
    struct netif_tx meta = { 0 };
    if (st->nif.offload) {
        th->csum = (uint16_t)~csum_fold(pseudo);
        meta = (struct netif_tx){
            .gso_size = data_len > c->mss ? c->mss : 0,
            .csum_start = IPV4_HDR_LEN,
            .csum_offset = offsetof(struct tcp_hdr, csum),
            .csum_partial = true,
        };
    } else {
        th->csum = csum_fold(csum_add(pseudo, th, tcplen));
    }
    ip_output(st, pkt, tcplen, IPPROTO_TCP_, c->raddr, st->nif.offload ? &meta : NULL);
    st->stats.tcp_tx_segs++;

    if (flags & TH_ACK) {
        c->segs_unacked = 0;
        c->ack_now = false;
        timer_cancel(&st->wheel, &c->delack_timer);
    }
}

void tcp_send_ack(struct tcp_conn *c)
{
    seq_t wnd_end = c->snd_una + c->snd_wnd;
    xmit(c, seq_lt(wnd_end, c->snd_nxt) ? wnd_end : c->snd_nxt, TH_ACK, 0, 0);
}

void tcp_send_probe(struct tcp_conn *c)
{
    xmit(c, c->snd_una - 1, TH_ACK, 0, 0);
}

void tcp_send_rst_conn(struct tcp_conn *c)
{
    c->st->stats.tcp_rst_tx++;
    xmit(c, c->snd_nxt, TH_RST | TH_ACK, 0, 0);
}

void tcp_send_syn(struct tcp_conn *c)
{
    txq_push(&c->txq, c->iss, 1, c->st->now_ns, REC_SYN);
    c->snd_nxt = c->iss + 1;
    xmit(c, c->iss, c->state == TCP_SYN_SENT ? TH_SYN : TH_SYN | TH_ACK, 0, 0);
}

void tcp_send_reset(struct us_stack *st, const struct tcp_seg *seg)
{
    uint8_t *pkt = st->txbuf;
    struct tcp_hdr *th = (struct tcp_hdr *)(pkt + IPV4_HDR_LEN);
    bool has_ack = seg->flags & TH_ACK;
    seq_t ack = seg->seq + seg->len + !!(seg->flags & TH_SYN) + !!(seg->flags & TH_FIN);

    *th = (struct tcp_hdr){
        .sport = htons(seg->dport),
        .dport = htons(seg->sport),
        .seq = htonl(has_ack ? seg->ack : 0),
        .ack = htonl(has_ack ? 0 : ack),
        .doff = (TCP_HDR_LEN / 4) << 4,
        .flags = has_ack ? TH_RST : TH_RST | TH_ACK,
    };
    uint64_t pseudo = csum_pseudo(st->cfg.addr, seg->saddr, IPPROTO_TCP_, TCP_HDR_LEN);
    th->csum = csum_fold(csum_add(pseudo, th, TCP_HDR_LEN));
    st->stats.tcp_rst_tx++;
    ip_output(st, pkt, TCP_HDR_LEN, IPPROTO_TCP_, seg->saddr, NULL);
}

static void fin_sent(struct tcp_conn *c)
{
    c->fin_sent = true;
    if (c->state == TCP_ESTABLISHED)
        tcp_set_state(c, TCP_FIN_WAIT_1);
    else if (c->state == TCP_CLOSE_WAIT)
        tcp_set_state(c, TCP_LAST_ACK);
}

void tcp_retransmit_rec(struct tcp_conn *c, struct tx_rec *r)
{
    uint32_t syn = !!(r->flags & REC_SYN);
    uint32_t fin = !!(r->flags & REC_FIN);
    uint32_t data_len = r->len - syn - fin;
    uint8_t flags = TH_ACK;
    if (syn)
        flags = c->state == TCP_SYN_SENT ? TH_SYN : TH_SYN | TH_ACK;
    if (fin)
        flags |= TH_FIN;
    if (data_len)
        flags |= TH_PSH;

    xmit(c, r->seq, flags, data_len, r->seq + syn - c->snd_buf_seq);

    r->xmit_ns = c->st->now_ns;
    if (r->flags & REC_LOST) {
        r->flags &= ~REC_LOST;
        c->lost_bytes -= r->len;
    }
    if (!(r->flags & REC_RETRANS)) {
        r->flags |= REC_RETRANS;
        c->retrans_out++;
    }
    c->st->stats.tcp_retrans++;
}

bool tcp_send_new(struct tcp_conn *c, uint32_t limit)
{
    seq_t end = tcp_snd_data_end(c);
    uint32_t avail = seq_gt(end, c->snd_nxt) ? end - c->snd_nxt : 0;
    uint32_t n = MIN(avail, limit);
    bool fin = c->shut_wr && !c->fin_sent && c->snd_nxt + n == end;
    if (!n && !fin)
        return false;

    uint8_t flags = TH_ACK;
    if (n && c->snd_nxt + n == end)
        flags |= TH_PSH;
    if (fin)
        flags |= TH_FIN;
    txq_push(&c->txq, c->snd_nxt, n + fin, c->st->now_ns, fin ? REC_FIN : 0);
    xmit(c, c->snd_nxt, flags, n, c->snd_nxt - c->snd_buf_seq);
    c->snd_nxt += n + fin;
    if (fin)
        fin_sent(c);
    return true;
}

static struct tx_rec *first_lost(struct tcp_conn *c)
{
    for (uint32_t i = c->lost_hint; i < c->txq.n; i++) {
        struct tx_rec *r = txq_at(&c->txq, i);
        if (r->flags & REC_LOST) {
            c->lost_hint = i;
            return r;
        }
    }
    c->lost_hint = c->txq.n;
    return NULL;
}

static uint32_t max_segment(const struct tcp_conn *c)
{
    if (!c->st->nif.offload)
        return c->mss;
    return (STACK_MAX_SEGMENT - TCP_MAX_OPT_LEN) / c->mss * c->mss;
}

static void send_data(struct tcp_conn *c)
{
    c->cwnd_limited = false;
    while (c->lost_bytes) {
        if (tcp_inflight(c) >= c->cwnd) {
            c->cwnd_limited = true;
            return;
        }
        struct tx_rec *r = first_lost(c);
        if (!r)
            break;
        tcp_retransmit_rec(c, r);
    }

    if (!tcp_can_send_data(c->state))
        return;
    for (;;) {
        seq_t end = tcp_snd_data_end(c);
        uint32_t avail = seq_gt(end, c->snd_nxt) ? end - c->snd_nxt : 0;
        if (!avail) {
            if (c->shut_wr && !c->fin_sent)
                tcp_send_new(c, 0);
            return;
        }
        seq_t wnd_end = c->snd_una + c->snd_wnd;
        uint32_t wnd_room = seq_gt(wnd_end, c->snd_nxt) ? wnd_end - c->snd_nxt : 0;
        uint32_t inflight = tcp_inflight(c);
        uint32_t cwnd_room = c->cwnd > inflight ? c->cwnd - inflight : 0;
        if (cwnd_room < MIN(avail, (uint32_t)c->mss) && cwnd_room < wnd_room)
            c->cwnd_limited = true;

        uint32_t n = MIN(MIN(avail, wnd_room), MIN(cwnd_room, max_segment(c)));
        if (!n)
            return;
        if (n < c->mss && n < avail && n < c->max_sndwnd / 2)
            return;
        if (n < c->mss && !c->nodelay && tcp_flight(c))
            return;
        tcp_send_new(c, n);
    }
}

void tcp_output(struct tcp_conn *c)
{
    switch (c->state) {
    case TCP_CLOSED:
    case TCP_LISTEN:
        return;
    case TCP_SYN_SENT:
    case TCP_SYN_RCVD:
        tcp_rearm_rtx(c);
        return;
    case TCP_TIME_WAIT:
        if (c->ack_now)
            tcp_send_ack(c);
        return;
    default:
        break;
    }
    send_data(c);
    if (c->ack_now)
        tcp_send_ack(c);
    tcp_rearm_rtx(c);
}

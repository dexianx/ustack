#include <arpa/inet.h>
#include <string.h>

#include "core/checksum.h"
#include "core/util.h"
#include "net/proto.h"
#include "tcp/tcp.h"

static uint16_t load16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool tcp_parse_options(const uint8_t *p, size_t len, struct tcp_opts *o)
{
    memset(o, 0, sizeof(*o));
    while (len) {
        uint8_t kind = p[0];
        if (kind == 0)
            break;
        if (kind == 1) {
            p++;
            len--;
            continue;
        }
        if (len < 2 || p[1] < 2 || p[1] > len)
            return false;
        uint8_t olen = p[1];
        switch (kind) {
        case 2:
            if (olen == 4) {
                o->has_mss = true;
                o->mss = load16(p + 2);
            }
            break;
        case 3:
            if (olen == 3) {
                o->has_wscale = true;
                o->wscale = MIN(p[2], TCP_MAX_WSCALE);
            }
            break;
        case 4:
            o->sack_ok = olen == 2;
            break;
        case 5:
            if ((olen - 2) % 8 == 0) {
                o->nsacks = (uint8_t)MIN((olen - 2) / 8, TCP_MAX_SACKS);
                for (uint8_t i = 0; i < o->nsacks; i++) {
                    o->sacks[i].start = load32(p + 2 + 8 * i);
                    o->sacks[i].end = load32(p + 6 + 8 * i);
                }
            }
            break;
        case 8:
            if (olen == 10) {
                o->has_ts = true;
                o->tsval = load32(p + 2);
                o->tsecr = load32(p + 6);
            }
            break;
        default:
            break;
        }
        p += olen;
        len -= olen;
    }
    return true;
}

static uint32_t seg_span(const struct tcp_seg *s)
{
    return s->len + !!(s->flags & TH_SYN) + !!(s->flags & TH_FIN);
}

static void negotiate(struct tcp_conn *c, const struct tcp_seg *seg, bool we_offered)
{
    const struct stack_config *cfg = &c->st->cfg;
    const struct tcp_opts *o = &seg->opt;

    c->peer_mss = o->has_mss && o->mss >= 64 ? o->mss : TCP_DEFAULT_MSS;
    c->ws_ok = we_offered && cfg->wscale && o->has_wscale;
    c->snd_wscale = c->ws_ok ? o->wscale : 0;
    c->rcv_wscale = 0;
    if (c->ws_ok)
        while (c->rcv_wscale < TCP_MAX_WSCALE && (c->rcvbuf.cap >> c->rcv_wscale) > 65535)
            c->rcv_wscale++;
    c->sack_ok = we_offered && cfg->sack && o->sack_ok;
    c->ts_ok = we_offered && cfg->timestamps && o->has_ts;
    if (c->ts_ok)
        c->ts_recent = o->tsval;

    uint32_t mss = MIN(c->peer_mss, (uint32_t)cfg->mtu - IPV4_HDR_LEN - TCP_HDR_LEN);
    if (c->ts_ok)
        mss -= TCP_OPT_TS_LEN;
    c->mss = (uint16_t)MAX(mss, 64u);

    c->irs = seg->seq;
    c->rcv_nxt = seg->seq + 1;
    c->rcv_adv = c->rcv_nxt;
    c->snd_wnd = seg->wnd;
    c->snd_wl1 = seg->seq;
    c->snd_wl2 = seg->ack;
}

static void init_cwnd(struct tcp_conn *c)
{
    c->cwnd = MIN(10u * c->mss, MAX(2u * c->mss, 14600u));
    c->st->cfg.cc->init(c);
}

static void listen_input(struct us_stack *st, struct tcp_listener *l, const struct tcp_seg *seg)
{
    if (seg->flags & TH_RST)
        return;
    if (seg->flags & TH_ACK) {
        tcp_send_reset(st, seg);
        return;
    }
    if (!(seg->flags & TH_SYN))
        return;

    struct tcp_conn *c = tcp_conn_new(st, seg->saddr, seg->sport, seg->dport);
    if (!c) {
        st->stats.tcp_syn_dropped++;
        return;
    }
    c->ops = l->ops;
    c->ctx = l->ctx;
    negotiate(c, seg, true);
    tcp_set_state(c, TCP_SYN_RCVD);
    tcp_send_syn(c);
    tcp_rearm_rtx(c);
}

static void syn_sent_input(struct tcp_conn *c, struct tcp_seg *seg)
{
    if ((seg->flags & TH_ACK) && (seq_le(seg->ack, c->iss) || seq_gt(seg->ack, c->snd_nxt))) {
        if (!(seg->flags & TH_RST))
            tcp_send_reset(c->st, seg);
        return;
    }
    if (seg->flags & TH_RST) {
        if (seg->flags & TH_ACK) {
            c->st->stats.tcp_refused++;
            tcp_conn_destroy(c, US_EREFUSED);
        }
        return;
    }
    if (!(seg->flags & TH_SYN))
        return;

    negotiate(c, seg, true);
    init_cwnd(c);
    if (!(seg->flags & TH_ACK)) {
        tcp_set_state(c, TCP_SYN_RCVD);
        tcp_retransmit_rec(c, txq_at(&c->txq, 0));
        return;
    }
    tcp_ack_received(c, seg);
    tcp_set_state(c, TCP_ESTABLISHED);
    c->ack_now = true;
    tcp_mark_dirty(c);
    tcp_notify_open(c);
}

/* RFC 9293 3.10.7.4, with Linux's inclusive right edge so zero-window ACKs are processed. */
static bool acceptable(const struct tcp_conn *c, const struct tcp_seg *seg)
{
    seq_t right = seq_max(c->rcv_adv, c->rcv_nxt);
    return seq_ge(seg->seq + seg_span(seg), c->rcv_nxt) && seq_le(seg->seq, right);
}

static void trim_to_window(struct tcp_conn *c, struct tcp_seg *seg)
{
    if (seq_lt(seg->seq, c->rcv_nxt)) {
        uint32_t d = c->rcv_nxt - seg->seq;
        if (seg->flags & TH_SYN) {
            seg->flags &= (uint8_t)~TH_SYN;
            seg->seq++;
            d--;
        }
        if (d > seg->len) {
            seg->flags &= (uint8_t)~TH_FIN;
            d = seg->len;
        }
        seg->data += d;
        seg->len -= d;
        seg->seq += d;
        c->st->stats.tcp_dup++;
        c->ack_now = true;
    }
    seq_t right = seq_max(c->rcv_adv, c->rcv_nxt);
    if (seq_gt(seg->seq + seg->len, right)) {
        seg->len = seq_gt(right, seg->seq) ? right - seg->seq : 0;
        seg->flags &= (uint8_t)~TH_FIN;
        c->ack_now = true;
    }
}

static void fin_in(struct tcp_conn *c)
{
    c->rcv_nxt++;
    c->fin_rcvd = true;
    c->fin_pending = false;
    c->ack_now = true;
    bool fin_acked = c->fin_sent && c->snd_una == c->snd_nxt;
    switch (c->state) {
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
        tcp_set_state(c, TCP_CLOSE_WAIT);
        break;
    case TCP_FIN_WAIT_1:
        tcp_set_state(c, fin_acked ? TCP_TIME_WAIT : TCP_CLOSING);
        break;
    case TCP_FIN_WAIT_2:
        tcp_set_state(c, TCP_TIME_WAIT);
        break;
    default:
        break;
    }
    tcp_notify_readable(c);
}

static void data_in(struct tcp_conn *c, const struct tcp_seg *seg)
{
    uint32_t off = seg->seq - c->rcv_nxt;
    uint32_t room = ring_space(&c->rcvbuf);
    if (off >= room) {
        c->ack_now = true;
        return;
    }
    uint32_t len = MIN(seg->len, room - off);
    if (!ring_reserve(&c->rcvbuf, c->rcvbuf.len + off + len))
        return;
    ring_write_at(&c->rcvbuf, c->rcvbuf.len + off, seg->data, len);

    if (off) {
        if (!seqset_add(&c->ooo, seg->seq, seg->seq + len))
            return;
        c->last_ooo = (struct seq_range){ seg->seq, seg->seq + len };
        c->st->stats.tcp_ooo++;
        c->ack_now = true;
        return;
    }

    c->rcv_nxt += len;
    c->rcvbuf.len += len;
    if (!seqset_empty(&c->ooo)) {
        seq_t end;
        seqset_trim_below(&c->ooo, c->rcv_nxt);
        while (seqset_pop_contiguous(&c->ooo, c->rcv_nxt, &end)) {
            c->rcvbuf.len += end - c->rcv_nxt;
            c->rcv_nxt = end;
            c->ack_now = true;
        }
    }

    if (++c->segs_unacked >= 2 || c->quickack) {
        c->ack_now = true;
        if (c->quickack)
            c->quickack--;
    } else if (!timer_pending(&c->delack_timer)) {
        timer_arm(&c->st->wheel, &c->delack_timer, stack_now_ms(c->st) + c->st->cfg.delack_ms);
    }

    if (c->attached)
        tcp_notify_readable(c);
    else
        ring_consume(&c->rcvbuf, c->rcvbuf.len);
}

static bool paws_reject(struct tcp_conn *c, const struct tcp_seg *seg)
{
    if (!c->ts_ok || !seg->opt.has_ts || (seg->flags & TH_RST))
        return false;
    if ((int32_t)(seg->opt.tsval - c->ts_recent) >= 0)
        return false;
    c->st->stats.tcp_paws++;
    return true;
}

static void challenge_ack(struct tcp_conn *c)
{
    c->st->stats.tcp_challenge_ack++;
    c->ack_now = true;
    tcp_mark_dirty(c);
}

static void synchronized_input(struct tcp_conn *c, struct tcp_seg *seg)
{
    if (c->state == TCP_SYN_RCVD && (seg->flags & TH_SYN) && !(seg->flags & TH_ACK) &&
        seg->seq == c->irs) {
        tcp_retransmit_rec(c, txq_at(&c->txq, 0));
        return;
    }
    if (paws_reject(c, seg) || !acceptable(c, seg)) {
        if (!(seg->flags & TH_RST)) {
            c->st->stats.tcp_dup++;
            c->ack_now = true;
            tcp_mark_dirty(c);
        }
        return;
    }
    if (c->ts_ok && seg->opt.has_ts && seq_le(seg->seq, c->rcv_nxt))
        c->ts_recent = seg->opt.tsval;

    trim_to_window(c, seg);

    if (seg->flags & TH_RST) {
        if (seg->seq == c->rcv_nxt)
            tcp_conn_destroy(c, US_ERESET);
        else
            challenge_ack(c);
        return;
    }
    if (seg->flags & TH_SYN) {
        challenge_ack(c);
        return;
    }
    if (!(seg->flags & TH_ACK))
        return;

    bool accepting = false;
    if (c->state == TCP_SYN_RCVD) {
        if (!seq_gt(seg->ack, c->snd_una) || seq_gt(seg->ack, c->snd_nxt)) {
            tcp_send_reset(c->st, seg);
            return;
        }
        init_cwnd(c);
        tcp_set_state(c, TCP_ESTABLISHED);
        accepting = true;
    }
    if (seq_gt(seg->ack, c->snd_nxt)) {
        c->ack_now = true;
        tcp_mark_dirty(c);
        return;
    }

    tcp_ack_received(c, seg);
    if (c->dead)
        return;
    if (accepting)
        tcp_notify_open(c);
    if (c->dead)
        return;

    bool fin_acked = c->fin_sent && c->snd_una == c->snd_nxt;
    switch (c->state) {
    case TCP_FIN_WAIT_1:
        if (fin_acked)
            tcp_set_state(c, TCP_FIN_WAIT_2);
        break;
    case TCP_CLOSING:
        if (fin_acked)
            tcp_set_state(c, TCP_TIME_WAIT);
        break;
    case TCP_LAST_ACK:
        if (fin_acked) {
            tcp_conn_destroy(c, US_OK);
            return;
        }
        break;
    case TCP_TIME_WAIT:
        if (seg->flags & TH_FIN) {
            c->ack_now = true;
            tcp_set_state(c, TCP_TIME_WAIT);
        }
        tcp_mark_dirty(c);
        return;
    default:
        break;
    }

    if (seg->len && (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
                     c->state == TCP_FIN_WAIT_2))
        data_in(c, seg);
    if (c->dead)
        return;

    if ((seg->flags & TH_FIN) && !c->fin_rcvd) {
        c->fin_seq = seg->seq + seg->len;
        c->fin_pending = true;
    }
    if (c->fin_pending && c->fin_seq == c->rcv_nxt)
        fin_in(c);
    tcp_mark_dirty(c);
}

void tcp_input(struct us_stack *st, const uint8_t *ip, const uint8_t *l4, size_t len, bool csum_ok)
{
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *)ip;
    const struct tcp_hdr *th = (const struct tcp_hdr *)l4;
    size_t doff = len >= TCP_HDR_LEN ? (size_t)(th->doff >> 4) * 4 : 0;
    if (doff < TCP_HDR_LEN || doff > len) {
        st->stats.tcp_bad++;
        return;
    }
    if (!csum_ok) {
        uint64_t sum = csum_pseudo(ntohl(iph->saddr), ntohl(iph->daddr), IPPROTO_TCP_, (uint16_t)len);
        if (csum_fold(csum_add(sum, l4, len)) != 0) {
            st->stats.tcp_bad++;
            return;
        }
    }

    struct tcp_seg seg = {
        .saddr = ntohl(iph->saddr),
        .sport = ntohs(th->sport),
        .dport = ntohs(th->dport),
        .seq = ntohl(th->seq),
        .ack = ntohl(th->ack),
        .wnd = ntohs(th->wnd),
        .flags = th->flags,
        .data = l4 + doff,
        .len = (uint32_t)(len - doff),
    };
    if (!tcp_parse_options(l4 + TCP_HDR_LEN, doff - TCP_HDR_LEN, &seg.opt)) {
        st->stats.tcp_bad++;
        return;
    }
    st->stats.tcp_rx_segs++;

    struct tcp_conn *c = tcp_lookup(st, seg.saddr, seg.sport, seg.dport);
    if (!c) {
        struct tcp_listener *l = tcp_find_listener(st, seg.dport);
        if (l)
            listen_input(st, l, &seg);
        else if (!(seg.flags & TH_RST))
            tcp_send_reset(st, &seg);
        return;
    }
    if (c->state == TCP_SYN_SENT)
        syn_sent_input(c, &seg);
    else
        synchronized_input(c, &seg);
}

#include <stdlib.h>

#include "core/util.h"
#include "tcp/tcp.h"

#define TLP_MIN_NS      (2 * NSEC_PER_MSEC)
#define TLP_DELACK_NS   (200 * NSEC_PER_MSEC)
#define MAX_BACKOFF     12
#define CWND_MAX        (1u << 30)

struct tx_rec *txq_push(struct txq *q, seq_t seq, uint32_t len, uint64_t now, uint32_t flags)
{
    if (q->n == q->cap) {
        uint32_t cap = q->cap ? q->cap * 2 : 64;
        struct tx_rec *r = malloc(cap * sizeof(*r));
        if (!r)
            abort();
        for (uint32_t i = 0; i < q->n; i++)
            r[i] = *txq_at(q, i);
        free(q->r);
        q->r = r;
        q->cap = cap;
        q->head = 0;
    }
    struct tx_rec *r = txq_at(q, q->n++);
    *r = (struct tx_rec){ seq, len, now, flags };
    return r;
}

void txq_free(struct txq *q)
{
    free(q->r);
    *q = (struct txq){ 0 };
}

static void txq_pop(struct txq *q)
{
    q->head = (q->head + 1) & (q->cap - 1);
    q->n--;
}

static uint32_t txq_lower_bound(const struct txq *q, seq_t seq)
{
    uint32_t lo = 0, hi = q->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const struct tx_rec *r = txq_at(q, mid);
        if (seq_le(r->seq + r->len, seq))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* RFC 6298 */
void tcp_rtt_sample(struct tcp_conn *c, uint32_t rtt_us)
{
    const struct stack_config *cfg = &c->st->cfg;
    rtt_us = MAX(rtt_us, 1u);
    c->min_rtt_us = MIN(c->min_rtt_us, rtt_us);
    if (!c->srtt_us) {
        c->srtt_us = rtt_us;
        c->rttvar_us = rtt_us / 2;
    } else {
        uint32_t delta = c->srtt_us > rtt_us ? c->srtt_us - rtt_us : rtt_us - c->srtt_us;
        c->rttvar_us = (3 * c->rttvar_us + delta) / 4;
        c->srtt_us = (7 * c->srtt_us + rtt_us) / 8;
    }
    uint32_t rto = c->srtt_us + MAX(1000u, 4 * c->rttvar_us);
    c->rto_us = MIN(MAX(rto, cfg->rto_min_us), cfg->rto_max_us);
}

/* RFC 8985 6.2 step 2-3: track the most recently sent segment known delivered. */
static void rack_update(struct tcp_conn *c, const struct tx_rec *r)
{
    uint32_t rtt_us = (uint32_t)((c->st->now_ns - r->xmit_ns) / NSEC_PER_USEC);
    seq_t end = r->seq + r->len;
    if ((r->flags & REC_RETRANS) && rtt_us < c->min_rtt_us)
        return;
    if (!(r->flags & REC_RETRANS)) {
        if (seq_lt(end, c->rack_fack))
            c->reordering_seen = true;
        else
            c->rack_fack = end;
    }
    if (r->xmit_ns > c->rack_xmit_ns || (r->xmit_ns == c->rack_xmit_ns && seq_gt(end, c->rack_end))) {
        c->rack_xmit_ns = r->xmit_ns;
        c->rack_end = end;
        c->rack_rtt_us = rtt_us;
    }
}

static uint64_t rack_reo_wnd_ns(const struct tcp_conn *c)
{
    if (c->min_rtt_us == UINT32_MAX)
        return 0;
    if (!c->reordering_seen && (c->phase != CC_OPEN || c->sacked_bytes >= 3u * c->mss))
        return 0;
    return (uint64_t)MIN(c->min_rtt_us / 4, c->srtt_us) * NSEC_PER_USEC;
}

static void mark_lost(struct tcp_conn *c, uint32_t i, struct tx_rec *r)
{
    r->flags |= REC_LOST;
    c->lost_bytes += r->len;
    c->lost_hint = MIN(c->lost_hint, i);
}

/* RFC 8985 6.2 step 5. While no retransmission is outstanding, records are in send-time order. */
static bool rack_detect_loss(struct tcp_conn *c)
{
    c->reo_wait_ns = 0;
    if (!c->rack_xmit_ns)
        return false;

    uint64_t now = c->st->now_ns;
    uint64_t wait = UINT64_MAX;
    uint64_t span = (uint64_t)c->rack_rtt_us * NSEC_PER_USEC + rack_reo_wnd_ns(c);
    bool ordered = c->retrans_out == 0;
    bool lost = false;

    for (uint32_t i = 0; i < c->txq.n; i++) {
        struct tx_rec *r = txq_at(&c->txq, i);
        if (r->flags & (REC_SACKED | REC_LOST))
            continue;
        if (r->xmit_ns > c->rack_xmit_ns) {
            if (ordered)
                break;
            continue;
        }
        if (r->xmit_ns == c->rack_xmit_ns && seq_ge(r->seq + r->len, c->rack_end))
            continue;
        uint64_t deadline = r->xmit_ns + span;
        if (deadline <= now) {
            mark_lost(c, i, r);
            lost = true;
        } else if (deadline - now < wait) {
            wait = deadline - now;
        }
    }
    if (wait != UINT64_MAX)
        c->reo_wait_ns = wait;
    return lost;
}

static void enter_recovery(struct tcp_conn *c)
{
    if (c->phase != CC_OPEN)
        return;
    c->phase = CC_RECOVERY;
    c->recover = c->snd_nxt;
    c->st->cfg.cc->on_congestion(c);
    c->st->stats.tcp_recoveries++;
}

static void forget_rec(struct tcp_conn *c, const struct tx_rec *r)
{
    if (r->flags & REC_SACKED)
        c->sacked_bytes -= r->len;
    else if (r->flags & REC_RETRANS)
        c->retrans_out--;
    if (r->flags & REC_LOST)
        c->lost_bytes -= r->len;
}

/* Karn: no sequence-based RTT sample when the ACK covers retransmitted data. */
static uint64_t advance_una(struct tcp_conn *c, seq_t ack)
{
    uint64_t rtt_xmit = 0;
    bool karn = false;
    while (c->txq.n) {
        struct tx_rec *r = txq_at(&c->txq, 0);
        seq_t end = r->seq + r->len;
        if (seq_le(end, ack)) {
            if (!(r->flags & REC_SACKED)) {
                rack_update(c, r);
                if (r->flags & REC_RETRANS)
                    karn = true;
                else
                    rtt_xmit = MAX(rtt_xmit, r->xmit_ns);
            }
            forget_rec(c, r);
            txq_pop(&c->txq);
            continue;
        }
        if (seq_lt(r->seq, ack)) {
            uint32_t d = ack - r->seq;
            if (r->flags & REC_SACKED)
                c->sacked_bytes -= d;
            if (r->flags & REC_LOST)
                c->lost_bytes -= d;
            r->seq = ack;
            r->len -= d;
        }
        break;
    }
    c->lost_hint = 0;
    c->snd_una = ack;

    seq_t buf_end = seq_min(ack, tcp_snd_data_end(c));
    if (seq_gt(buf_end, c->snd_buf_seq)) {
        ring_consume(&c->sndbuf, buf_end - c->snd_buf_seq);
        ring_release_if_empty(&c->sndbuf);
        c->snd_buf_seq = buf_end;
    }
    return karn ? 0 : rtt_xmit;
}

static uint64_t apply_sack(struct tcp_conn *c, seq_t lo, seq_t hi)
{
    uint64_t rtt_xmit = 0;
    if (!seq_lt(lo, hi) || !seq_gt(hi, c->snd_una) || seq_gt(hi, c->snd_nxt))
        return 0;
    lo = seq_max(lo, c->snd_una);
    for (uint32_t i = txq_lower_bound(&c->txq, lo); i < c->txq.n; i++) {
        struct tx_rec *r = txq_at(&c->txq, i);
        if (seq_gt(r->seq + r->len, hi))
            break;
        if (seq_lt(r->seq, lo) || (r->flags & REC_SACKED))
            continue;
        r->flags |= REC_SACKED;
        c->sacked_bytes += r->len;
        if (r->flags & REC_RETRANS)
            c->retrans_out--;
        if (r->flags & REC_LOST) {
            r->flags &= ~REC_LOST;
            c->lost_bytes -= r->len;
        }
        rack_update(c, r);
        if (!(r->flags & REC_RETRANS))
            rtt_xmit = MAX(rtt_xmit, r->xmit_ns);
    }
    return rtt_xmit;
}

static bool dupack_loss(struct tcp_conn *c, const struct tcp_seg *seg, bool window_update)
{
    if (seg->len || window_update || (seg->flags & (TH_SYN | TH_FIN)) || !c->txq.n ||
        seg->ack != c->snd_una)
        return false;
    if (++c->dupacks != 3)
        return false;
    struct tx_rec *r = txq_at(&c->txq, 0);
    if (r->flags & (REC_SACKED | REC_LOST))
        return false;
    mark_lost(c, 0, r);
    return true;
}

/* RFC 6582: without SACK, a partial ACK during recovery exposes the next hole. */
static bool partial_ack_loss(struct tcp_conn *c)
{
    if (c->phase != CC_RECOVERY || !c->txq.n)
        return false;
    struct tx_rec *r = txq_at(&c->txq, 0);
    if (r->flags & (REC_LOST | REC_RETRANS))
        return false;
    mark_lost(c, 0, r);
    return true;
}

void tcp_ack_received(struct tcp_conn *c, const struct tcp_seg *seg)
{
    seq_t ack = seg->ack;
    uint32_t wnd = seg->wnd << (seg->flags & TH_SYN ? 0 : c->snd_wscale);
    bool window_update = false;
    if (seq_gt(ack, c->snd_una) || seq_lt(c->snd_wl1, seg->seq) ||
        (c->snd_wl1 == seg->seq && wnd > c->snd_wnd)) {
        window_update = wnd != c->snd_wnd;
        c->snd_wnd = wnd;
        c->snd_wl1 = seg->seq;
        c->snd_wl2 = ack;
        c->max_sndwnd = MAX(c->max_sndwnd, wnd);
    }

    uint32_t acked = 0;
    uint32_t freed = 0;
    uint64_t rtt_xmit = 0;
    bool lost = false;
    if (seq_gt(ack, c->snd_una)) {
        acked = ack - c->snd_una;
        uint32_t before = c->sndbuf.len;
        rtt_xmit = advance_una(c, ack);
        freed = before - c->sndbuf.len;
        c->backoff = 0;
        c->retries = 0;
        c->dupacks = 0;
        c->tlp_out = false;
    } else if (!c->sack_ok) {
        lost = dupack_loss(c, seg, window_update);
    }

    if (c->sack_ok)
        for (uint8_t i = 0; i < seg->opt.nsacks; i++)
            rtt_xmit = MAX(rtt_xmit, apply_sack(c, seg->opt.sacks[i].start, seg->opt.sacks[i].end));

    if (rtt_xmit)
        tcp_rtt_sample(c, (uint32_t)((c->st->now_ns - rtt_xmit) / NSEC_PER_USEC));
    if (c->sack_ok)
        lost |= rack_detect_loss(c);

    if (c->phase != CC_OPEN && acked && seq_ge(c->snd_una, c->recover))
        c->phase = CC_OPEN;
    else if (!c->sack_ok && acked)
        lost |= partial_ack_loss(c);
    if (lost)
        enter_recovery(c);
    if (acked && c->phase != CC_RECOVERY && c->state >= TCP_ESTABLISHED) {
        c->st->cfg.cc->on_ack(c, acked);
        c->cwnd = MIN(c->cwnd, CWND_MAX);
    }

    if (acked || lost || window_update || c->reo_wait_ns)
        tcp_mark_dirty(c);
    if (freed && c->attached && !c->shut_wr && c->ops->on_writable)
        c->ops->on_writable((struct us_sock *)c);
}

static void on_rto(struct tcp_conn *c)
{
    struct us_stack *st = c->st;
    if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RCVD) {
        if (++c->retries > st->cfg.syn_retries) {
            tcp_conn_destroy(c, US_ETIMEDOUT);
            return;
        }
        c->backoff = (uint8_t)MIN(c->backoff + 1, MAX_BACKOFF);
        tcp_retransmit_rec(c, txq_at(&c->txq, 0));
        return;
    }
    if (++c->retries > st->cfg.max_retries) {
        tcp_send_rst_conn(c);
        tcp_conn_destroy(c, US_ETIMEDOUT);
        return;
    }

    st->stats.tcp_rto++;
    st->cfg.cc->on_rto(c);
    for (uint32_t i = 0; i < c->txq.n; i++) {
        struct tx_rec *r = txq_at(&c->txq, i);
        if (!(r->flags & (REC_SACKED | REC_LOST)))
            mark_lost(c, i, r);
    }
    c->lost_hint = 0;
    c->phase = CC_LOSS;
    c->recover = c->snd_nxt;
    c->backoff = (uint8_t)MIN(c->backoff + 1, MAX_BACKOFF);
    c->tlp_out = false;
}

/*
 * RFC 8985 7.3 prefers probing with new data, but a receiver may hold the ACK for
 * in-order data (Linux defers it up to 200 ms when the window would shrink). A
 * retransmission of the last segment is a duplicate, which every stack ACKs at once.
 */
static void on_tlp(struct tcp_conn *c)
{
    c->st->stats.tcp_tlp++;
    c->tlp_out = true;
    if (c->txq.n) {
        struct tx_rec *r = txq_at(&c->txq, c->txq.n - 1);
        if (!(r->flags & REC_SACKED))
            tcp_retransmit_rec(c, r);
    }
}

/* Zero-window probe, doubling as the RFC 9293 3.8.6.2.1 override for data held back by SWS avoidance. */
static void on_persist(struct tcp_conn *c)
{
    seq_t wnd_end = c->snd_una + c->snd_wnd;
    if (!seq_gt(wnd_end, c->snd_nxt) || !tcp_send_new(c, MIN((uint32_t)c->mss, wnd_end - c->snd_nxt)))
        tcp_send_probe(c);
    c->backoff = (uint8_t)MIN(c->backoff + 1, MAX_BACKOFF);
}

void tcp_rtx_timer_fired(struct timer *t)
{
    struct tcp_conn *c = container_of(t, struct tcp_conn, rtx_timer);
    enum rtx_mode mode = c->rtx_mode;
    c->rtx_mode = RTX_NONE;
    switch (mode) {
    case RTX_REO:
        if (rack_detect_loss(c))
            enter_recovery(c);
        break;
    case RTX_TLP:
        on_tlp(c);
        break;
    case RTX_PERSIST:
        on_persist(c);
        break;
    case RTX_RTO:
        on_rto(c);
        break;
    case RTX_NONE:
        return;
    }
    tcp_mark_dirty(c);
}

static void arm(struct tcp_conn *c, enum rtx_mode mode, uint64_t deadline_ns)
{
    c->rtx_mode = mode;
    timer_arm(&c->st->wheel, &c->rtx_timer, (deadline_ns + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC);
}

static uint64_t backed_off_rto_ns(const struct tcp_conn *c)
{
    uint64_t rto = (uint64_t)c->rto_us << c->backoff;
    return MIN(rto, (uint64_t)c->st->cfg.rto_max_us) * NSEC_PER_USEC;
}

void tcp_rearm_rtx(struct tcp_conn *c)
{
    struct us_stack *st = c->st;
    uint64_t now = st->now_ns;
    if (c->dead || c->state == TCP_TIME_WAIT || c->state <= TCP_LISTEN) {
        timer_cancel(&st->wheel, &c->rtx_timer);
        c->rtx_mode = RTX_NONE;
        return;
    }

    if (c->snd_una == c->snd_nxt) {
        bool blocked = tcp_can_send_data(c->state) && seq_lt(c->snd_nxt, tcp_snd_data_end(c));
        if (!blocked) {
            timer_cancel(&st->wheel, &c->rtx_timer);
            c->rtx_mode = RTX_NONE;
        } else if (c->rtx_mode != RTX_PERSIST || !timer_pending(&c->rtx_timer)) {
            arm(c, RTX_PERSIST, now + backed_off_rto_ns(c));
        }
        return;
    }

    if (c->reo_wait_ns) {
        arm(c, RTX_REO, now + c->reo_wait_ns);
        return;
    }

    uint64_t rto_deadline = txq_at(&c->txq, 0)->xmit_ns + backed_off_rto_ns(c);
    if (c->sack_ok && !c->tlp_out && c->phase == CC_OPEN && c->srtt_us &&
        tcp_synchronized(c->state)) {
        uint64_t pto = 2ull * c->srtt_us * NSEC_PER_USEC;
        if (tcp_flight(c) <= c->mss)
            pto += TLP_DELACK_NS;
        pto = MAX(pto, TLP_MIN_NS);
        if (now + pto < rto_deadline) {
            arm(c, RTX_TLP, now + pto);
            return;
        }
    }
    arm(c, RTX_RTO, rto_deadline);
}

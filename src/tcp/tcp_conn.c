#include <stdio.h>
#include <stdlib.h>

#include "core/util.h"
#include "tcp/tcp.h"

#define SOCK(c) ((struct us_sock *)(c))
#define CONN(s) ((struct tcp_conn *)(s))

static void delack_fired(struct timer *t);
static void tw_fired(struct timer *t);

const char *tcp_state_name(enum tcp_state s)
{
    static const char *const names[] = {
        "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
        "FIN_WAIT_2", "CLOSING", "TIME_WAIT", "CLOSE_WAIT", "LAST_ACK",
    };
    return (unsigned)s < ARRAY_LEN(names) ? names[s] : "?";
}

static uint32_t tuple_hash(const struct us_stack *st, uint32_t raddr, uint16_t rport,
                           uint16_t lport)
{
    struct {
        uint32_t raddr;
        uint16_t rport;
        uint16_t lport;
    } key = { raddr, rport, lport };
    return (uint32_t)siphash24(&st->hash_key, &key, sizeof(key));
}

static bool tuple_eq(const struct tcp_conn *c, uint32_t raddr, uint16_t rport, uint16_t lport)
{
    return c->raddr == raddr && c->rport == rport && c->lport == lport;
}

struct tcp_conn *tcp_lookup(struct us_stack *st, uint32_t raddr, uint16_t rport, uint16_t lport)
{
    struct tcp_conn *c = st->tcp.cache;
    if (c && tuple_eq(c, raddr, rport, lport))
        return c;
    for (c = st->tcp.buckets[tuple_hash(st, raddr, rport, lport) & st->tcp.mask]; c; c = c->hnext) {
        if (tuple_eq(c, raddr, rport, lport)) {
            st->tcp.cache = c;
            return c;
        }
    }
    return NULL;
}

static void table_grow(struct us_stack *st)
{
    uint32_t nmask = st->tcp.mask * 2 + 1;
    struct tcp_conn **nb = calloc((size_t)nmask + 1, sizeof(*nb));
    if (!nb)
        return;
    for (uint32_t i = 0; i <= st->tcp.mask; i++) {
        struct tcp_conn *c = st->tcp.buckets[i];
        while (c) {
            struct tcp_conn *next = c->hnext;
            uint32_t b = tuple_hash(st, c->raddr, c->rport, c->lport) & nmask;
            c->hnext = nb[b];
            nb[b] = c;
            c = next;
        }
    }
    free(st->tcp.buckets);
    st->tcp.buckets = nb;
    st->tcp.mask = nmask;
}

static void table_insert(struct us_stack *st, struct tcp_conn *c)
{
    if (st->tcp.count > st->tcp.mask)
        table_grow(st);
    struct tcp_conn **b = &st->tcp.buckets[tuple_hash(st, c->raddr, c->rport, c->lport) & st->tcp.mask];
    c->hnext = *b;
    *b = c;
    c->hashed = true;
    st->tcp.count++;
}

static void table_remove(struct us_stack *st, struct tcp_conn *c)
{
    if (!c->hashed)
        return;
    struct tcp_conn **pp = &st->tcp.buckets[tuple_hash(st, c->raddr, c->rport, c->lport) & st->tcp.mask];
    while (*pp != c)
        pp = &(*pp)->hnext;
    *pp = c->hnext;
    c->hashed = false;
    st->tcp.count--;
    if (st->tcp.cache == c)
        st->tcp.cache = NULL;
}

struct tcp_listener *tcp_find_listener(struct us_stack *st, uint16_t port)
{
    for (struct tcp_listener *l = st->tcp.listeners; l; l = l->next)
        if (l->port == port)
            return l;
    return NULL;
}

/* RFC 6528: ISN = M + F(4-tuple, secret), M a 4 microsecond clock. */
static seq_t tcp_isn(struct us_stack *st, uint32_t raddr, uint16_t rport, uint16_t lport)
{
    struct {
        uint32_t laddr;
        uint32_t raddr;
        uint16_t lport;
        uint16_t rport;
    } key = { st->cfg.addr, raddr, lport, rport };
    return (seq_t)(siphash24(&st->isn_key, &key, sizeof(key)) + st->now_ns / 4000);
}

struct tcp_conn *tcp_conn_new(struct us_stack *st, uint32_t raddr, uint16_t rport, uint16_t lport)
{
    if (st->tcp.count >= st->cfg.max_conns)
        return NULL;
    struct tcp_conn *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    ring_init(&c->sndbuf, st->cfg.sndbuf);
    ring_init(&c->rcvbuf, st->cfg.rcvbuf);
    c->st = st;
    c->raddr = raddr;
    c->rport = rport;
    c->lport = lport;
    c->state = TCP_CLOSED;
    c->iss = tcp_isn(st, raddr, rport, lport);
    c->snd_una = c->snd_nxt = c->iss;
    c->snd_buf_seq = c->iss + 1;
    c->ts_offset = (uint32_t)splitmix64(&st->rng);
    c->rto_us = st->cfg.rto_init_us;
    c->min_rtt_us = UINT32_MAX;
    c->ssthresh = UINT32_MAX / 2;
    c->quickack = TCP_QUICKACK_SEGS;
    seqset_init(&c->ooo, TCP_MAX_OOO_RANGES);
    timer_init(&c->rtx_timer, tcp_rtx_timer_fired);
    timer_init(&c->delack_timer, delack_fired);
    timer_init(&c->tw_timer, tw_fired);
    table_insert(st, c);
    st->stats.tcp_opened++;
    return c;
}

void tcp_mark_dirty(struct tcp_conn *c)
{
    if (c->dirty || c->dead)
        return;
    c->dirty = true;
    c->dirty_next = c->st->dirty;
    c->st->dirty = c;
}

void tcp_conn_destroy(struct tcp_conn *c, enum us_err err)
{
    if (c->dead)
        return;
    struct us_stack *st = c->st;
    c->dead = true;
    c->state = TCP_CLOSED;
    timer_cancel(&st->wheel, &c->rtx_timer);
    timer_cancel(&st->wheel, &c->delack_timer);
    timer_cancel(&st->wheel, &c->tw_timer);
    table_remove(st, c);
    c->dead_next = st->dead;
    st->dead = c;
    st->stats.tcp_closed++;
    if (c->attached) {
        c->attached = false;
        if (c->ops->on_close)
            c->ops->on_close(SOCK(c), err);
    }
}

static void conn_free(struct tcp_conn *c)
{
    ring_free(&c->sndbuf);
    ring_free(&c->rcvbuf);
    seqset_free(&c->ooo);
    txq_free(&c->txq);
    free(c);
}

void tcp_reap(struct us_stack *st)
{
    while (st->dead) {
        struct tcp_conn *c = st->dead;
        st->dead = c->dead_next;
        if (c->dirty) {
            struct tcp_conn **pp = &st->dirty;
            while (*pp != c)
                pp = &(*pp)->dirty_next;
            *pp = c->dirty_next;
        }
        conn_free(c);
    }
}

void tcp_table_free(struct us_stack *st)
{
    for (uint32_t i = 0; i <= st->tcp.mask; i++) {
        while (st->tcp.buckets[i]) {
            struct tcp_conn *c = st->tcp.buckets[i];
            c->attached = false;
            tcp_conn_destroy(c, US_OK);
        }
    }
    for (struct tcp_conn *c = st->dead; c; c = c->dead_next)
        c->dirty = false;
    st->dirty = NULL;
    tcp_reap(st);
    while (st->tcp.listeners) {
        struct tcp_listener *l = st->tcp.listeners;
        st->tcp.listeners = l->next;
        free(l);
    }
}

void tcp_set_state(struct tcp_conn *c, enum tcp_state s)
{
    struct us_stack *st = c->st;
    c->state = s;
    switch (s) {
    case TCP_TIME_WAIT:
        timer_cancel(&st->wheel, &c->rtx_timer);
        timer_arm(&st->wheel, &c->tw_timer, stack_now_ms(st) + 2ull * st->cfg.msl_ms);
        break;
    case TCP_FIN_WAIT_2:
        if (!c->attached)
            timer_arm(&st->wheel, &c->tw_timer, stack_now_ms(st) + 2ull * st->cfg.msl_ms);
        break;
    default:
        break;
    }
}

static void delack_fired(struct timer *t)
{
    struct tcp_conn *c = container_of(t, struct tcp_conn, delack_timer);
    c->ack_now = true;
    tcp_mark_dirty(c);
}

static void tw_fired(struct timer *t)
{
    tcp_conn_destroy(container_of(t, struct tcp_conn, tw_timer), US_OK);
}

void tcp_notify_open(struct tcp_conn *c)
{
    if (c->opened)
        return;
    c->opened = true;
    c->attached = true;
    if (c->ops->on_open)
        c->ops->on_open(SOCK(c));
}

void tcp_notify_readable(struct tcp_conn *c)
{
    if (c->attached && c->ops->on_readable)
        c->ops->on_readable(SOCK(c));
}

int us_listen(struct us_stack *st, uint16_t port, const struct us_tcp_ops *ops, void *ctx)
{
    if (tcp_find_listener(st, port))
        return -1;
    struct tcp_listener *l = calloc(1, sizeof(*l));
    if (!l)
        return -1;
    *l = (struct tcp_listener){ st->tcp.listeners, port, ops, ctx };
    st->tcp.listeners = l;
    return 0;
}

static uint16_t pick_port(struct us_stack *st, uint32_t raddr, uint16_t rport)
{
    uint32_t span = (uint32_t)(st->cfg.ephemeral_hi - st->cfg.ephemeral_lo) + 1;
    for (uint32_t i = 0; i < span; i++) {
        uint16_t p = st->next_port;
        st->next_port = p == st->cfg.ephemeral_hi ? st->cfg.ephemeral_lo : (uint16_t)(p + 1);
        if (!tcp_lookup(st, raddr, rport, p) && !tcp_find_listener(st, p))
            return p;
    }
    return 0;
}

struct us_sock *us_connect(struct us_stack *st, uint32_t addr, uint16_t port,
                           const struct us_tcp_ops *ops, void *ctx)
{
    uint16_t lport = pick_port(st, addr, port);
    if (!lport)
        return NULL;
    struct tcp_conn *c = tcp_conn_new(st, addr, port, lport);
    if (!c)
        return NULL;
    c->ops = ops;
    c->ctx = ctx;
    c->attached = true;
    tcp_set_state(c, TCP_SYN_SENT);
    tcp_send_syn(c);
    tcp_rearm_rtx(c);
    return SOCK(c);
}

void *us_ctx(const struct us_sock *s) { return CONN(s)->ctx; }
void *us_data(const struct us_sock *s) { return CONN(s)->data; }
void us_set_data(struct us_sock *s, void *data) { CONN(s)->data = data; }
struct us_stack *us_sock_stack(const struct us_sock *s) { return CONN(s)->st; }
void us_set_nodelay(struct us_sock *s, bool on) { CONN(s)->nodelay = on; }
size_t us_readable(const struct us_sock *s) { return CONN(s)->rcvbuf.len; }

bool us_eof(const struct us_sock *s)
{
    const struct tcp_conn *c = CONN(s);
    return c->dead || (c->fin_rcvd && c->rcvbuf.len == 0);
}

size_t us_read(struct us_sock *s, void *buf, size_t n)
{
    struct tcp_conn *c = CONN(s);
    if (c->dead)
        return 0;
    size_t got = ring_read(&c->rcvbuf, buf, (uint32_t)MIN(n, UINT32_MAX));
    if (seqset_empty(&c->ooo))
        ring_release_if_empty(&c->rcvbuf);
    uint32_t cap = c->rcvbuf.cap;
    uint32_t adv = seq_gt(c->rcv_adv, c->rcv_nxt) ? c->rcv_adv - c->rcv_nxt : 0;
    uint32_t room = ring_space(&c->rcvbuf);
    if (got && adv < cap / 2 && room > adv && room - adv >= MIN(cap / 2, 2u * c->mss)) {
        c->ack_now = true;
        tcp_mark_dirty(c);
    }
    return got;
}

static bool can_write(const struct tcp_conn *c)
{
    return !c->dead && !c->shut_wr &&
           (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RCVD ||
            c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT);
}

size_t us_writable(const struct us_sock *s)
{
    const struct tcp_conn *c = CONN(s);
    return can_write(c) ? ring_space(&c->sndbuf) : 0;
}

size_t us_write(struct us_sock *s, const void *buf, size_t n)
{
    struct tcp_conn *c = CONN(s);
    if (!can_write(c))
        return 0;
    size_t put = ring_append(&c->sndbuf, buf, (uint32_t)MIN(n, UINT32_MAX));
    if (put)
        tcp_mark_dirty(c);
    return put;
}

void us_shutdown(struct us_sock *s)
{
    struct tcp_conn *c = CONN(s);
    if (c->dead || c->shut_wr)
        return;
    if (c->state == TCP_SYN_SENT) {
        c->attached = false;
        tcp_conn_destroy(c, US_OK);
        return;
    }
    c->shut_wr = true;
    tcp_mark_dirty(c);
}

void us_abort(struct us_sock *s)
{
    struct tcp_conn *c = CONN(s);
    if (c->dead)
        return;
    c->attached = false;
    if (c->state != TCP_SYN_SENT)
        tcp_send_rst_conn(c);
    tcp_conn_destroy(c, US_OK);
}

void us_close(struct us_sock *s)
{
    struct tcp_conn *c = CONN(s);
    if (c->dead)
        return;
    if (c->rcvbuf.len) {
        us_abort(s);
        return;
    }
    us_shutdown(s);
    c->attached = false;
    if (c->state == TCP_FIN_WAIT_2)
        tcp_set_state(c, TCP_FIN_WAIT_2);
}

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            snprintf(why, n, __VA_ARGS__);       \
            return false;                        \
        }                                        \
    } while (0)

bool tcp_check_invariants(const struct tcp_conn *c, char *why, size_t n)
{
    CHECK(seq_le(c->snd_una, c->snd_nxt), "snd_una > snd_nxt");
    CHECK(c->sndbuf.len <= c->sndbuf.cap, "sndbuf overflow");
    CHECK(c->rcvbuf.len <= c->rcvbuf.cap, "rcvbuf overflow");
    CHECK(seq_le(c->snd_buf_seq, c->snd_una + 1), "snd_buf_seq ahead of snd_una");

    uint32_t flight = 0, sacked = 0, lost = 0, retrans = 0;
    seq_t expect = c->snd_una;
    for (uint32_t i = 0; i < c->txq.n; i++) {
        const struct tx_rec *r = txq_at(&c->txq, i);
        CHECK(r->seq == expect, "txq gap at rec %u: seq %u expect %u", i, r->seq, expect);
        CHECK(r->len > 0, "empty rec");
        expect = r->seq + r->len;
        flight += r->len;
        if (r->flags & REC_SACKED)
            sacked += r->len;
        if (r->flags & REC_LOST)
            lost += r->len;
        if ((r->flags & REC_RETRANS) && !(r->flags & REC_SACKED))
            retrans++;
        CHECK(!((r->flags & REC_SACKED) && (r->flags & REC_LOST)), "rec sacked and lost");
    }
    CHECK(expect == c->snd_nxt, "txq end %u != snd_nxt %u", expect, c->snd_nxt);
    CHECK(flight == tcp_flight(c), "flight mismatch");
    CHECK(sacked == c->sacked_bytes, "sacked %u != %u", sacked, c->sacked_bytes);
    CHECK(lost == c->lost_bytes, "lost %u != %u", lost, c->lost_bytes);
    CHECK(retrans == c->retrans_out, "retrans %u != %u", retrans, c->retrans_out);

    for (uint32_t i = 0; i < c->ooo.n; i++) {
        const struct seq_range *r = &c->ooo.r[i];
        CHECK(seq_lt(r->start, r->end), "empty ooo range");
        CHECK(seq_gt(r->start, c->rcv_nxt), "ooo range at/below rcv_nxt");
        if (i)
            CHECK(seq_gt(r->start, c->ooo.r[i - 1].end), "ooo ranges overlap");
        CHECK(r->end - c->rcv_nxt <= c->rcvbuf.cap - c->rcvbuf.len, "ooo beyond buffer");
    }
    CHECK(c->cwnd > 0 || c->state < TCP_ESTABLISHED, "zero cwnd");
    return true;
}

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/ring.h"
#include "core/seq.h"
#include "core/seqset.h"
#include "core/timer.h"
#include "stack.h"

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

#define TCP_DEFAULT_MSS 536
#define TCP_MAX_WSCALE  14
#define TCP_MAX_SACKS   4
#define TCP_OPT_TS_LEN  12
#define TCP_QUICKACK_SEGS 16
#define TCP_MAX_OOO_RANGES 4096

enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
};

struct tcp_opts {
    uint16_t mss;
    uint8_t wscale;
    uint8_t nsacks;
    bool has_mss;
    bool has_wscale;
    bool sack_ok;
    bool has_ts;
    uint32_t tsval;
    uint32_t tsecr;
    struct seq_range sacks[TCP_MAX_SACKS];
};

struct tcp_seg {
    uint32_t saddr;
    uint16_t sport;
    uint16_t dport;
    seq_t seq;
    seq_t ack;
    uint32_t wnd;
    uint8_t flags;
    const uint8_t *data;
    uint32_t len;
    struct tcp_opts opt;
};

enum {
    REC_SACKED = 1u << 0,
    REC_LOST = 1u << 1,
    REC_RETRANS = 1u << 2,
    REC_SYN = 1u << 3,
    REC_FIN = 1u << 4,
};

/* One transmitted segment; sequence span includes SYN/FIN. */
struct tx_rec {
    seq_t seq;
    uint32_t len;
    uint64_t xmit_ns;
    uint32_t flags;
};

struct txq {
    struct tx_rec *r;
    uint32_t cap;
    uint32_t head;
    uint32_t n;
};

enum rtx_mode { RTX_NONE, RTX_RTO, RTX_TLP, RTX_REO, RTX_PERSIST };
enum cc_phase { CC_OPEN, CC_RECOVERY, CC_LOSS };

struct cubic {
    double w_max;
    double w_last_max;
    double k;
    double origin;
    double w_est;
    double acc;
    uint64_t epoch_ns;
};

struct tcp_listener {
    struct tcp_listener *next;
    uint16_t port;
    const struct us_tcp_ops *ops;
    void *ctx;
};

struct tcp_conn {
    struct us_stack *st;
    struct tcp_conn *hnext;
    struct tcp_conn *dirty_next;
    struct tcp_conn *dead_next;

    uint32_t raddr;
    uint16_t lport;
    uint16_t rport;
    enum tcp_state state;
    bool dirty;
    bool dead;
    bool hashed;

    const struct us_tcp_ops *ops;
    void *ctx;
    void *data;
    bool attached;
    bool opened;
    bool nodelay;
    bool shut_wr;

    seq_t iss;
    seq_t snd_una;
    seq_t snd_nxt;
    seq_t snd_wl1;
    seq_t snd_wl2;
    seq_t snd_buf_seq;
    seq_t recover;
    uint32_t snd_wnd;
    uint32_t max_sndwnd;
    uint8_t snd_wscale;
    uint16_t mss;
    uint16_t peer_mss;
    struct ring sndbuf;
    struct txq txq;
    uint32_t sacked_bytes;
    uint32_t lost_bytes;
    uint32_t retrans_out;
    uint32_t lost_hint;
    uint8_t dupacks;
    bool fin_sent;
    bool cwnd_limited;

    seq_t irs;
    seq_t rcv_nxt;
    seq_t rcv_adv;
    uint8_t rcv_wscale;
    struct ring rcvbuf;
    struct seqset ooo;
    struct seq_range last_ooo;
    bool fin_rcvd;
    bool fin_pending;
    seq_t fin_seq;
    uint16_t segs_unacked;
    uint16_t quickack;
    bool ack_now;

    bool sack_ok;
    bool ts_ok;
    bool ws_ok;
    uint32_t ts_recent;
    uint32_t ts_offset;

    uint32_t srtt_us;
    uint32_t rttvar_us;
    uint32_t rto_us;
    uint32_t min_rtt_us;
    uint8_t backoff;
    uint8_t retries;

    uint64_t rack_xmit_ns;
    seq_t rack_end;
    seq_t rack_fack;
    uint32_t rack_rtt_us;
    uint64_t reo_wait_ns;
    bool reordering_seen;
    bool tlp_out;

    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t cwnd_acc;
    enum cc_phase phase;
    struct cubic cubic;

    struct timer rtx_timer;
    enum rtx_mode rtx_mode;
    struct timer delack_timer;
    struct timer tw_timer;
};

struct cc_ops {
    const char *name;
    void (*init)(struct tcp_conn *c);
    void (*on_ack)(struct tcp_conn *c, uint32_t acked);
    void (*on_congestion)(struct tcp_conn *c);
    void (*on_rto)(struct tcp_conn *c);
};

extern const struct cc_ops cc_newreno;
extern const struct cc_ops cc_cubic;
const struct cc_ops *cc_find(const char *name);

/* tcp_conn.c */
struct tcp_conn *tcp_lookup(struct us_stack *st, uint32_t raddr, uint16_t rport, uint16_t lport);
struct tcp_listener *tcp_find_listener(struct us_stack *st, uint16_t port);
struct tcp_conn *tcp_conn_new(struct us_stack *st, uint32_t raddr, uint16_t rport, uint16_t lport);
void tcp_conn_destroy(struct tcp_conn *c, enum us_err err);
void tcp_set_state(struct tcp_conn *c, enum tcp_state s);
void tcp_mark_dirty(struct tcp_conn *c);
void tcp_notify_open(struct tcp_conn *c);
void tcp_notify_readable(struct tcp_conn *c);
void tcp_reap(struct us_stack *st);
void tcp_table_free(struct us_stack *st);
const char *tcp_state_name(enum tcp_state s);
bool tcp_check_invariants(const struct tcp_conn *c, char *why, size_t n);

/* tcp_input.c */
void tcp_input(struct us_stack *st, const uint8_t *ip, const uint8_t *l4, size_t len, bool csum_ok);
bool tcp_parse_options(const uint8_t *p, size_t len, struct tcp_opts *o);

/* tcp_output.c */
void tcp_output(struct tcp_conn *c);
void tcp_send_ack(struct tcp_conn *c);
void tcp_send_syn(struct tcp_conn *c);
void tcp_send_probe(struct tcp_conn *c);
void tcp_send_reset(struct us_stack *st, const struct tcp_seg *seg);
void tcp_send_rst_conn(struct tcp_conn *c);
void tcp_retransmit_rec(struct tcp_conn *c, struct tx_rec *r);
bool tcp_send_new(struct tcp_conn *c, uint32_t limit);
uint32_t tcp_rcv_window(struct tcp_conn *c);
uint32_t tcp_ts_now(const struct tcp_conn *c);

/* tcp_recovery.c */
void tcp_ack_received(struct tcp_conn *c, const struct tcp_seg *seg);
void tcp_rearm_rtx(struct tcp_conn *c);
void tcp_rtx_timer_fired(struct timer *t);
void tcp_rtt_sample(struct tcp_conn *c, uint32_t rtt_us);
struct tx_rec *txq_push(struct txq *q, seq_t seq, uint32_t len, uint64_t now, uint32_t flags);
void txq_free(struct txq *q);

static inline struct tx_rec *txq_at(const struct txq *q, uint32_t i)
{
    return &q->r[(q->head + i) & (q->cap - 1)];
}

static inline uint32_t tcp_flight(const struct tcp_conn *c) { return c->snd_nxt - c->snd_una; }

static inline uint32_t tcp_inflight(const struct tcp_conn *c)
{
    return tcp_flight(c) - c->sacked_bytes - c->lost_bytes;
}

static inline seq_t tcp_snd_data_end(const struct tcp_conn *c)
{
    return c->snd_buf_seq + c->sndbuf.len;
}

static inline bool tcp_synchronized(enum tcp_state s)
{
    return s >= TCP_ESTABLISHED;
}

static inline bool tcp_can_send_data(enum tcp_state s)
{
    return s == TCP_ESTABLISHED || s == TCP_CLOSE_WAIT;
}

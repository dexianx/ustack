#include <math.h>
#include <string.h>

#include "core/util.h"
#include "tcp/tcp.h"

static void slow_start(struct tcp_conn *c, uint32_t acked)
{
    c->cwnd += MIN(acked, 2u * c->mss);
}

static void reno_init(struct tcp_conn *c)
{
    c->cwnd_acc = 0;
}

/* RFC 5681 with appropriate byte counting (RFC 3465). */
static void reno_on_ack(struct tcp_conn *c, uint32_t acked)
{
    if (!c->cwnd_limited)
        return;
    if (c->cwnd < c->ssthresh) {
        slow_start(c, acked);
        return;
    }
    c->cwnd_acc += acked;
    if (c->cwnd_acc >= c->cwnd) {
        c->cwnd_acc -= c->cwnd;
        c->cwnd += c->mss;
    }
}

static void reno_on_congestion(struct tcp_conn *c)
{
    c->ssthresh = MAX(c->cwnd / 2, 2u * c->mss);
    c->cwnd = c->ssthresh;
    c->cwnd_acc = 0;
}

static void reno_on_rto(struct tcp_conn *c)
{
    c->ssthresh = MAX(tcp_inflight(c) / 2, 2u * c->mss);
    c->cwnd = c->mss;
    c->cwnd_acc = 0;
}

const struct cc_ops cc_newreno = {
    .name = "newreno",
    .init = reno_init,
    .on_ack = reno_on_ack,
    .on_congestion = reno_on_congestion,
    .on_rto = reno_on_rto,
};

/* RFC 9438. Windows are tracked in segments as doubles. */
#define CUBIC_C    0.4
#define CUBIC_BETA 0.7

static void cubic_init(struct tcp_conn *c)
{
    memset(&c->cubic, 0, sizeof(c->cubic));
}

static void cubic_reduce(struct tcp_conn *c)
{
    struct cubic *cu = &c->cubic;
    double w = (double)c->cwnd / c->mss;
    if (w < cu->w_last_max)
        cu->w_max = w * (1.0 + CUBIC_BETA) / 2.0;
    else
        cu->w_max = w;
    cu->w_last_max = w;
    cu->epoch_ns = 0;
    c->ssthresh = MAX((uint32_t)(c->cwnd * CUBIC_BETA), 2u * c->mss);
}

static void cubic_on_congestion(struct tcp_conn *c)
{
    cubic_reduce(c);
    c->cwnd = c->ssthresh;
}

static void cubic_on_rto(struct tcp_conn *c)
{
    cubic_reduce(c);
    c->cwnd = c->mss;
}

static void cubic_on_ack(struct tcp_conn *c, uint32_t acked)
{
    struct cubic *cu = &c->cubic;
    if (!c->cwnd_limited)
        return;
    if (c->cwnd < c->ssthresh) {
        slow_start(c, acked);
        return;
    }

    uint64_t now = c->st->now_ns;
    double w = (double)c->cwnd / c->mss;
    if (!cu->epoch_ns) {
        cu->epoch_ns = now;
        cu->acc = 0;
        cu->w_est = w;
        if (w < cu->w_max) {
            cu->k = cbrt((cu->w_max - w) / CUBIC_C);
            cu->origin = cu->w_max;
        } else {
            cu->k = 0;
            cu->origin = w;
        }
    }

    double t = (double)(now - cu->epoch_ns) / 1e9 + c->srtt_us / 1e6;
    double target = cu->origin + CUBIC_C * pow(t - cu->k, 3);
    target = MIN(MAX(target, w), 1.5 * w);

    double alpha = 3.0 * (1.0 - CUBIC_BETA) / (1.0 + CUBIC_BETA);
    cu->w_est += alpha * (double)acked / c->cwnd;
    if (cu->w_est > target)
        target = cu->w_est;

    cu->acc += (target - w) / w * acked;
    if (cu->acc >= 1.0) {
        uint32_t inc = (uint32_t)cu->acc;
        c->cwnd += inc;
        cu->acc -= inc;
    }
}

const struct cc_ops cc_cubic = {
    .name = "cubic",
    .init = cubic_init,
    .on_ack = cubic_on_ack,
    .on_congestion = cubic_on_congestion,
    .on_rto = cubic_on_rto,
};

const struct cc_ops *cc_find(const char *name)
{
    if (!strcmp(name, "cubic"))
        return &cc_cubic;
    if (!strcmp(name, "newreno") || !strcmp(name, "reno"))
        return &cc_newreno;
    return NULL;
}

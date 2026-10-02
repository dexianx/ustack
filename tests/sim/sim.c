#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/apps.h"
#include "core/pattern.h"
#include "core/util.h"
#include "sim/simnet.h"
#include "tcp/tcp.h"

#define MAX_CLIENTS 8
#define CHUNK       65536
#define PORT        APP_ECHO_PORT

struct client {
    struct us_sock *s;
    uint64_t total;
    uint64_t sent;
    uint64_t rcvd;
    bool done;
    bool failed;
    char err[96];
};

struct run {
    struct simnet net;
    struct client clients[MAX_CLIENTS];
    int nclients;
    char why[160];
    bool broken;
};

struct params {
    double loss;
    double dup;
    double corrupt;
    uint64_t jitter_ns;
    uint64_t delay_ns;
    uint64_t rate_bps;
    uint32_t queue_bytes;
    int nclients;
    uint64_t bytes;
    uint32_t sndbuf;
    uint32_t rcvbuf;
    bool sack;
    bool ts;
    bool ws;
    const struct cc_ops *cc;
};

struct options {
    uint64_t first_seed;
    int seeds;
    bool verbose;
    bool bulk;
    double loss;
    uint64_t bulk_bytes;
};

static void client_fail(struct client *cl, const char *msg)
{
    if (!cl->failed)
        snprintf(cl->err, sizeof(cl->err), "%s", msg);
    cl->failed = true;
}

static void client_pump(struct us_sock *s)
{
    struct client *cl = us_ctx(s);
    uint8_t buf[CHUNK];
    while (cl->sent < cl->total) {
        size_t n = MIN(us_writable(s), (size_t)MIN((uint64_t)sizeof(buf), cl->total - cl->sent));
        if (!n)
            return;
        pattern_fill(cl->sent, buf, n);
        cl->sent += us_write(s, buf, n);
    }
    us_shutdown(s);
}

static void client_readable(struct us_sock *s)
{
    struct client *cl = us_ctx(s);
    uint8_t buf[CHUNK], expect[CHUNK];
    size_t n;
    while ((n = us_read(s, buf, sizeof(buf)))) {
        pattern_fill(cl->rcvd, expect, n);
        if (cl->rcvd + n > cl->total || memcmp(buf, expect, n)) {
            client_fail(cl, "echoed data mismatch");
            us_abort(s);
            return;
        }
        cl->rcvd += n;
    }
    if (us_eof(s)) {
        if (cl->rcvd != cl->total)
            client_fail(cl, "EOF before all data echoed");
        cl->done = true;
        us_close(s);
    }
}

static void client_closed(struct us_sock *s, enum us_err err)
{
    struct client *cl = us_ctx(s);
    if (!cl->done) {
        char msg[64];
        snprintf(msg, sizeof(msg), "connection closed early (err %d)", err);
        client_fail(cl, msg);
        cl->done = true;
    }
}

static const struct us_tcp_ops client_ops = {
    .on_open = client_pump,
    .on_readable = client_readable,
    .on_writable = client_pump,
    .on_close = client_closed,
};

static bool all_done(void *ctx)
{
    struct run *r = ctx;
    if (r->broken)
        return true;
    for (int i = 0; i < r->nclients; i++)
        if (!r->clients[i].done)
            return false;
    return r->net.node[0]->tcp.count == 0 && r->net.node[1]->tcp.count == 0;
}

static bool check_invariants(struct simnet *net, void *ctx)
{
    struct run *r = ctx;
    if (!simnet_check_all(net, r->why, sizeof(r->why))) {
        r->broken = true;
        return false;
    }
    return true;
}

static uint64_t rand_range(uint64_t *rng, uint64_t lo, uint64_t hi)
{
    return lo + splitmix64(rng) % (hi - lo + 1);
}

static uint64_t rand_log(uint64_t *rng, uint64_t lo, uint64_t hi)
{
    double u = rand_unit(rng);
    return (uint64_t)((double)lo * pow((double)hi / (double)lo, u));
}

static void random_params(uint64_t seed, const struct options *o, struct params *p)
{
    uint64_t rng = seed * 0x2545f4914f6cdd1dull + 1;
    static const double losses[] = { 0, 0, 0.01, 0.02, 0.05, 0.10, 0.15, 0.20 };
    *p = (struct params){
        .loss = o->loss >= 0 ? o->loss : losses[splitmix64(&rng) % ARRAY_LEN(losses)],
        .dup = rand_unit(&rng) < 0.3 ? 0.02 : 0,
        .corrupt = rand_unit(&rng) < 0.3 ? 0.01 : 0,
        .jitter_ns = rand_unit(&rng) < 0.4 ? rand_range(&rng, 100, 5000) * NSEC_PER_USEC : 0,
        .delay_ns = rand_log(&rng, 50, 50000) * NSEC_PER_USEC,
        .rate_bps = rand_log(&rng, 10, 2000) * 1000000ull,
        .queue_bytes = (uint32_t)rand_log(&rng, 16, 1024) * 1024,
        .nclients = (int)rand_range(&rng, 1, 4),
        .bytes = rand_log(&rng, 1, 2u << 20),
        .sndbuf = 1u << rand_range(&rng, 13, 20),
        .rcvbuf = 1u << rand_range(&rng, 13, 20),
        .sack = rand_unit(&rng) < 0.85,
        .ts = rand_unit(&rng) < 0.85,
        .ws = rand_unit(&rng) < 0.85,
        .cc = rand_unit(&rng) < 0.5 ? &cc_cubic : &cc_newreno,
    };
}

static void configure(struct stack_config *cfg, const struct params *p, uint32_t addr, uint64_t seed)
{
    stack_config_defaults(cfg);
    cfg->addr = addr;
    cfg->seed = seed;
    cfg->sndbuf = p->sndbuf;
    cfg->rcvbuf = p->rcvbuf;
    cfg->sack = p->sack;
    cfg->timestamps = p->ts;
    cfg->wscale = p->ws;
    cfg->cc = p->cc;
    cfg->msl_ms = 250;
    cfg->max_retries = 30;
    cfg->syn_retries = 15;
}

static void set_link(struct sim_link *l, const struct params *p)
{
    *l = (struct sim_link){
        .delay_ns = p->delay_ns,
        .jitter_ns = p->jitter_ns,
        .rate_bps = p->rate_bps,
        .queue_bytes = p->queue_bytes,
        .loss = p->loss,
        .dup = p->dup,
        .corrupt = p->corrupt,
    };
}

struct outcome {
    bool ok;
    uint64_t bytes;
    uint64_t virtual_ns;
    uint64_t retrans;
    uint64_t segs;
    char why[200];
};

static void dump_conns(const struct us_stack *st, const char *name)
{
    for (uint32_t b = 0; b <= st->tcp.mask; b++)
        for (const struct tcp_conn *c = st->tcp.buckets[b]; c; c = c->hnext)
            printf("     %s :%u %s una=%u nxt=%u end=%u wnd=%u cwnd=%u flight=%u sacked=%u lost=%u "
                   "rcv_nxt=%u adv=%u rcvq=%u ooo=%u fin_sent=%d shut=%d timer=%d mode=%d att=%d\n",
                   name, c->lport, tcp_state_name(c->state), c->snd_una - c->iss,
                   c->snd_nxt - c->iss, tcp_snd_data_end(c) - c->iss, c->snd_wnd, c->cwnd,
                   tcp_flight(c), c->sacked_bytes, c->lost_bytes, c->rcv_nxt - c->irs,
                   c->rcv_adv - c->irs, c->rcvbuf.len, c->ooo.n, c->fin_sent, c->shut_wr,
                   timer_pending(&c->rtx_timer), c->rtx_mode, c->attached);
}

static bool dump_on_failure;

static void run_one(uint64_t seed, const struct params *p, struct outcome *out)
{
    struct run *r = calloc(1, sizeof(*r));
    struct stack_config ca, cb;
    configure(&ca, p, SIM_ADDR_A, seed);
    configure(&cb, p, SIM_ADDR_B, seed ^ 0xb);
    simnet_init(&r->net, seed);
    set_link(&r->net.link[0], p);
    set_link(&r->net.link[1], p);
    struct us_stack *a = simnet_attach(&r->net, 0, &ca);
    struct us_stack *b = simnet_attach(&r->net, 1, &cb);
    echo_listen(b, PORT);
    r->net.check = check_invariants;
    r->net.trace = getenv("SIM_TRACE") != NULL;
    r->net.check_ctx = r;

    uint64_t start = r->net.now;
    r->nclients = p->nclients;
    for (int i = 0; i < r->nclients; i++) {
        r->clients[i].total = p->bytes;
        r->clients[i].s = us_connect(a, SIM_ADDR_B, PORT, &client_ops, &r->clients[i]);
    }
    stack_flush(a);

    bool finished = simnet_run(&r->net, all_done, r, 900 * NSEC_PER_SEC);
    *out = (struct outcome){
        .ok = finished && !r->broken,
        .bytes = 2 * p->bytes * (uint64_t)p->nclients,
        .virtual_ns = r->net.now - start,
        .retrans = a->stats.tcp_retrans + b->stats.tcp_retrans,
        .segs = a->stats.tcp_tx_segs + b->stats.tcp_tx_segs,
    };
    if (r->broken)
        snprintf(out->why, sizeof(out->why), "invariant violated: %s", r->why);
    else if (!finished)
        snprintf(out->why, sizeof(out->why), "did not finish (a conns %u, b conns %u)",
                 a->tcp.count, b->tcp.count);
    if (!out->ok && dump_on_failure) {
        dump_conns(a, "A");
        dump_conns(b, "B");
    }
    for (int i = 0; i < r->nclients; i++) {
        struct client *cl = &r->clients[i];
        if (cl->failed) {
            out->ok = false;
            snprintf(out->why, sizeof(out->why), "client %d: %s (rcvd %" PRIu64 "/%" PRIu64 ")",
                     i, cl->err, cl->rcvd, cl->total);
        }
    }
    simnet_free(&r->net);
    free(r);
}

static void describe(const struct params *p, char *buf, size_t n)
{
    snprintf(buf, n,
             "loss=%.2f dup=%.2f corrupt=%.2f jitter=%" PRIu64 "us delay=%" PRIu64
             "us rate=%" PRIu64 "Mb q=%uKB conns=%d bytes=%" PRIu64 " snd=%u rcv=%u sack=%d ts=%d ws=%d %s",
             p->loss, p->dup, p->corrupt, p->jitter_ns / 1000, p->delay_ns / 1000,
             p->rate_bps / 1000000, p->queue_bytes / 1024, p->nclients, p->bytes, p->sndbuf,
             p->rcvbuf, p->sack, p->ts, p->ws, p->cc->name);
}

static int run_matrix(const struct options *o)
{
    int failures = 0;
    uint64_t bytes = 0, segs = 0, retrans = 0;
    for (int i = 0; i < o->seeds; i++) {
        uint64_t seed = o->first_seed + (uint64_t)i;
        struct params p;
        struct outcome out;
        char desc[256];
        random_params(seed, o, &p);
        describe(&p, desc, sizeof(desc));
        run_one(seed, &p, &out);
        bytes += out.bytes;
        segs += out.segs;
        retrans += out.retrans;
        if (!out.ok) {
            failures++;
            printf("FAIL seed=%" PRIu64 " %s\n     %s\n", seed, desc, out.why);
        } else if (o->verbose) {
            printf("ok   seed=%" PRIu64 " %.2fs virtual %s\n", seed, out.virtual_ns / 1e9, desc);
        }
        fflush(stdout);
    }
    printf("simulation: %d/%d runs passed, %.1f MB verified end-to-end, %" PRIu64
           " segments, %" PRIu64 " retransmissions\n",
           o->seeds - failures, o->seeds, bytes / 1e6, segs, retrans);
    return failures ? 1 : 0;
}

static int run_bulk(const struct options *o)
{
    static const double losses[] = { 0, 0.01, 0.05, 0.10, 0.20 };
    printf("bulk transfer, %" PRIu64 " MB echoed, 1 Gbit/s link, 1 ms one-way delay\n",
           o->bulk_bytes >> 20);
    int failures = 0;
    for (size_t i = 0; i < ARRAY_LEN(losses); i++) {
        struct params p = {
            .loss = losses[i], .delay_ns = NSEC_PER_MSEC, .rate_bps = 1000000000ull,
            .queue_bytes = 1u << 20, .nclients = 1, .bytes = o->bulk_bytes,
            .sndbuf = 4u << 20, .rcvbuf = 4u << 20, .sack = true, .ts = true, .ws = true,
            .cc = &cc_cubic,
        };
        struct outcome out;
        run_one(o->first_seed, &p, &out);
        failures += !out.ok;
        printf("  loss %4.0f%%: %s  %7.1f Mbit/s goodput per direction, %" PRIu64 " retransmits%s%s\n",
               losses[i] * 100, out.ok ? "intact" : "FAILED",
               (double)p.bytes * 8 / (out.virtual_ns / 1e9) / 1e6, out.retrans,
               out.ok ? "" : " - ", out.why);
    }
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    struct options o = { .first_seed = 1, .seeds = 200, .loss = -1, .bulk_bytes = 64u << 20 };
    static const struct option longopts[] = {
        { "seed", required_argument, 0, 's' }, { "seeds", required_argument, 0, 'n' },
        { "loss", required_argument, 0, 'l' }, { "verbose", no_argument, 0, 'v' },
        { "quick", no_argument, 0, 'q' },      { "bulk", optional_argument, 0, 'b' },
        { 0, 0, 0, 0 },
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "s:n:l:vqb", longopts, NULL)) != -1) {
        switch (ch) {
        case 's': o.first_seed = strtoull(optarg, NULL, 0); o.seeds = 1; o.verbose = true; dump_on_failure = true; break;
        case 'n': o.seeds = atoi(optarg); break;
        case 'l': o.loss = atof(optarg); break;
        case 'v': o.verbose = true; break;
        case 'q': o.seeds = 100; break;
        case 'b':
            o.bulk = true;
            if (optarg)
                o.bulk_bytes = strtoull(optarg, NULL, 0) << 20;
            break;
        default:
            fprintf(stderr, "usage: %s [--quick] [--seeds N] [--seed S] [--loss P] [--bulk[=MB]] [-v]\n", argv[0]);
            return 2;
        }
    }
    pattern_init();
    return o.bulk ? run_bulk(&o) : run_matrix(&o);
}

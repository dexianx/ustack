#include "sim/simnet.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "net/proto.h"
#include "tcp/tcp.h"

static bool event_before(const struct sim_event *a, const struct sim_event *b)
{
    return a->time < b->time || (a->time == b->time && a->order < b->order);
}

static void heap_push(struct simnet *net, struct sim_event ev)
{
    if (net->heap_n == net->heap_cap) {
        net->heap_cap = net->heap_cap ? net->heap_cap * 2 : 256;
        net->heap = realloc(net->heap, net->heap_cap * sizeof(*net->heap));
        if (!net->heap)
            abort();
    }
    uint32_t i = net->heap_n++;
    while (i) {
        uint32_t parent = (i - 1) / 2;
        if (!event_before(&ev, &net->heap[parent]))
            break;
        net->heap[i] = net->heap[parent];
        i = parent;
    }
    net->heap[i] = ev;
}

static struct sim_event heap_pop(struct simnet *net)
{
    struct sim_event top = net->heap[0];
    struct sim_event last = net->heap[--net->heap_n];
    uint32_t i = 0;
    for (;;) {
        uint32_t child = 2 * i + 1;
        if (child >= net->heap_n)
            break;
        if (child + 1 < net->heap_n && event_before(&net->heap[child + 1], &net->heap[child]))
            child++;
        if (!event_before(&net->heap[child], &last))
            break;
        net->heap[i] = net->heap[child];
        i = child;
    }
    if (net->heap_n)
        net->heap[i] = last;
    return top;
}

void simnet_init(struct simnet *net, uint64_t seed)
{
    memset(net, 0, sizeof(*net));
    net->rng = seed;
    net->now = 1000 * NSEC_PER_SEC;
}

void simnet_free(struct simnet *net)
{
    for (int i = 0; i < 2; i++)
        stack_destroy(net->node[i]);
    while (net->heap_n)
        free(heap_pop(net).data);
    free(net->heap);
}

static void enqueue(struct simnet *net, int dst, uint8_t *data, uint32_t len, uint64_t at)
{
    heap_push(net, (struct sim_event){ at, net->order++, dst, len, data });
}

static uint8_t *copy_packet(const uint8_t *pkt, uint32_t len)
{
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy)
        abort();
    memcpy(copy, pkt, len);
    return copy;
}

void simnet_inject(struct simnet *net, int dst, const uint8_t *pkt, uint32_t len, uint64_t at)
{
    enqueue(net, dst, copy_packet(pkt, len), len, at);
}

static void trace_packet(const struct simnet *net, int from, const uint8_t *pkt, const char *fate)
{
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)pkt;
    if (ip->proto != IPPROTO_TCP_)
        return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)(pkt + (ip->ver_ihl & 15) * 4);
    size_t len = ntohs(ip->total_len) - (size_t)(ip->ver_ihl & 15) * 4 - (size_t)(th->doff >> 4) * 4;
    printf("%10.6f %c:%u > :%u %s%s%s%s%s seq=%u ack=%u len=%zu wnd=%u %s\n",
           (double)(net->now % (1000 * NSEC_PER_SEC)) / 1e9, from ? 'B' : 'A', ntohs(th->sport),
           ntohs(th->dport), th->flags & TH_SYN ? "S" : "", th->flags & TH_FIN ? "F" : "",
           th->flags & TH_RST ? "R" : "", th->flags & TH_PSH ? "P" : "",
           th->flags & TH_ACK ? "." : "", ntohl(th->seq), ntohl(th->ack), len, ntohs(th->wnd), fate);
}

static void sim_tx(void *ctx, const uint8_t *pkt, size_t len, const struct netif_tx *meta)
{
    (void)meta;
    struct sim_port *port = ctx;
    struct simnet *net = port->net;
    const struct sim_link *l = &net->link[port->idx];
    net->stats.sent++;

    if (rand_unit(&net->rng) < l->loss) {
        net->stats.lost++;
        if (net->trace)
            trace_packet(net, port->idx, pkt, "LOST");
        return;
    }
    if (net->trace)
        trace_packet(net, port->idx, pkt, "");
    int copies = rand_unit(&net->rng) < l->dup ? 2 : 1;
    net->stats.duplicated += (uint64_t)copies - 1;
    for (int k = 0; k < copies; k++) {
        uint64_t depart = net->now;
        if (l->rate_bps) {
            uint64_t start = MAX(net->now, net->link_free[port->idx]);
            uint64_t backlog = (start - net->now) * l->rate_bps / (8 * NSEC_PER_SEC);
            if (l->queue_bytes && backlog + len > l->queue_bytes) {
                net->stats.queue_drops++;
                continue;
            }
            depart = start + (uint64_t)len * 8 * NSEC_PER_SEC / l->rate_bps;
            net->link_free[port->idx] = depart;
        }
        uint64_t jitter = l->jitter_ns ? splitmix64(&net->rng) % l->jitter_ns : 0;
        uint8_t *copy = copy_packet(pkt, (uint32_t)len);
        if (rand_unit(&net->rng) < l->corrupt) {
            uint64_t bit = splitmix64(&net->rng) % (len * 8);
            copy[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            net->stats.corrupted++;
        }
        enqueue(net, 1 - port->idx, copy, (uint32_t)len, depart + l->delay_ns + jitter);
    }
}

struct us_stack *simnet_attach(struct simnet *net, int idx, const struct stack_config *cfg)
{
    net->port[idx] = (struct sim_port){ net, idx };
    struct netif nif = { .tx = sim_tx, .ctx = &net->port[idx], .offload = false };
    net->node[idx] = stack_create(cfg, &nif, net->now);
    return net->node[idx];
}

static uint64_t next_timer(const struct simnet *net)
{
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 2; i++) {
        const struct us_stack *st = net->node[i];
        if (!st || !st->wheel.armed)
            continue;
        uint64_t t;
        if (net->heap_n)
            t = (st->wheel.tick + 1) * NSEC_PER_MSEC;
        else
            t = wheel_next_expiry(&st->wheel) * NSEC_PER_MSEC;
        best = MIN(best, t);
    }
    return best;
}

bool simnet_step_until(struct simnet *net, uint64_t limit)
{
    uint64_t t_pkt = net->heap_n ? net->heap[0].time : UINT64_MAX;
    uint64_t t_tmr = next_timer(net);
    uint64_t t = MIN(t_pkt, t_tmr);
    bool more = t <= limit;
    if (!more)
        t = limit;
    if (t == UINT64_MAX)
        return false;
    if (t > net->now)
        net->now = t;

    for (int i = 0; i < 2; i++)
        if (net->node[i])
            stack_advance(net->node[i], net->now);

    if (net->heap_n && net->heap[0].time <= net->now) {
        struct sim_event ev = heap_pop(net);
        struct us_stack *st = net->node[ev.dst];
        if (st) {
            net->stats.delivered++;
            st->now_ns = net->now;
            stack_input(st, ev.data, ev.len, false);
            stack_flush(st);
        }
        free(ev.data);
    }
    return more;
}

bool simnet_step(struct simnet *net)
{
    return simnet_step_until(net, UINT64_MAX);
}

bool simnet_check_all(struct simnet *net, char *why, size_t n)
{
    for (int i = 0; i < 2; i++) {
        struct us_stack *st = net->node[i];
        if (!st)
            continue;
        for (uint32_t b = 0; b <= st->tcp.mask; b++)
            for (struct tcp_conn *c = st->tcp.buckets[b]; c; c = c->hnext)
                if (!tcp_check_invariants(c, why, n))
                    return false;
    }
    return true;
}

bool simnet_run(struct simnet *net, bool (*done)(void *ctx), void *ctx, uint64_t deadline_ns)
{
    uint64_t end = net->now + deadline_ns;
    while (!done(ctx)) {
        if (net->now > end || !simnet_step(net))
            return done(ctx);
        if (net->check && !net->check(net, net->check_ctx))
            return false;
    }
    return true;
}

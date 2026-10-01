#include "stack.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "core/checksum.h"
#include "core/util.h"
#include "net/proto.h"
#include "tcp/tcp.h"

void stack_config_defaults(struct stack_config *cfg)
{
    *cfg = (struct stack_config){
        .addr = 0x0a000002,
        .mtu = 1500,
        .sndbuf = 1u << 20,
        .rcvbuf = 1u << 20,
        .rto_min_us = 200000,
        .rto_max_us = 60000000,
        .rto_init_us = 1000000,
        .msl_ms = 30000,
        .delack_ms = 40,
        .max_conns = 1u << 20,
        .max_retries = 15,
        .syn_retries = 6,
        .ephemeral_lo = 32768,
        .ephemeral_hi = 60999,
        .sack = true,
        .timestamps = true,
        .wscale = true,
        .cc = &cc_cubic,
        .seed = 0x5eed,
    };
}

struct us_stack *stack_create(const struct stack_config *cfg, const struct netif *nif,
                              uint64_t now_ns)
{
    struct us_stack *st = calloc(1, sizeof(*st));
    if (!st)
        return NULL;
    st->txbuf = aligned_alloc(64, STACK_TXBUF_SIZE);
    st->tcp.mask = 1023;
    st->tcp.buckets = calloc(st->tcp.mask + 1, sizeof(*st->tcp.buckets));
    if (!st->txbuf || !st->tcp.buckets) {
        free(st->txbuf);
        free(st->tcp.buckets);
        free(st);
        return NULL;
    }
    st->cfg = *cfg;
    st->nif = *nif;
    st->now_ns = now_ns;
    st->rng = cfg->seed;
    st->hash_key = (struct siphash_key){ splitmix64(&st->rng), splitmix64(&st->rng) };
    st->isn_key = (struct siphash_key){ splitmix64(&st->rng), splitmix64(&st->rng) };
    st->ip_id = (uint16_t)splitmix64(&st->rng);
    uint32_t span = (uint32_t)(cfg->ephemeral_hi - cfg->ephemeral_lo) + 1;
    st->next_port = (uint16_t)(cfg->ephemeral_lo + splitmix64(&st->rng) % span);
    wheel_init(&st->wheel, now_ns / NSEC_PER_MSEC);
    return st;
}

void stack_destroy(struct us_stack *st)
{
    if (!st)
        return;
    tcp_table_free(st);
    free(st->tcp.buckets);
    free(st->txbuf);
    free(st);
}

void ip_output(struct us_stack *st, uint8_t *pkt, size_t l4len, uint8_t proto, uint32_t daddr,
               const struct netif_tx *meta)
{
    struct ipv4_hdr *ip = (struct ipv4_hdr *)pkt;
    size_t total = IPV4_HDR_LEN + l4len;

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)total);
    ip->id = htons(st->ip_id++);
    ip->frag_off = htons(IP_DF);
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->saddr = htonl(st->cfg.addr);
    ip->daddr = htonl(daddr);
    ip->csum = csum_compute(ip, IPV4_HDR_LEN);

    st->stats.tx_packets++;
    st->stats.tx_bytes += total;
    st->nif.tx(st->nif.ctx, pkt, total, meta);
}

void stack_input(struct us_stack *st, const uint8_t *pkt, size_t len, bool csum_ok)
{
    st->stats.rx_packets++;
    st->stats.rx_bytes += len;

    if (unlikely(len < IPV4_HDR_LEN)) {
        st->stats.ip_bad++;
        return;
    }
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)pkt;
    if (unlikely((ip->ver_ihl >> 4) != 4)) {
        st->stats.ip_not_v4++;
        return;
    }
    size_t ihl = (size_t)(ip->ver_ihl & 0x0f) * 4;
    size_t total = ntohs(ip->total_len);
    if (unlikely(ihl < IPV4_HDR_LEN || total < ihl || total > len ||
                 csum_compute(ip, ihl) != 0)) {
        st->stats.ip_bad++;
        return;
    }
    if (unlikely(ntohl(ip->daddr) != st->cfg.addr)) {
        st->stats.ip_not_ours++;
        return;
    }
    if (unlikely(ntohs(ip->frag_off) & (IP_MF | IP_OFFMASK))) {
        st->stats.ip_frag_dropped++;
        return;
    }

    const uint8_t *l4 = pkt + ihl;
    size_t l4len = total - ihl;
    switch (ip->proto) {
    case IPPROTO_TCP_:
        tcp_input(st, pkt, l4, l4len, csum_ok);
        break;
    case IPPROTO_UDP_:
        udp_input(st, pkt, l4, l4len, csum_ok);
        break;
    case IPPROTO_ICMP_:
        icmp_input(st, pkt, total, l4, l4len);
        break;
    default:
        st->stats.ip_unknown_proto++;
        break;
    }
}

void stack_flush(struct us_stack *st)
{
    while (st->dirty) {
        struct tcp_conn *c = st->dirty;
        st->dirty = c->dirty_next;
        c->dirty = false;
        if (!c->dead)
            tcp_output(c);
    }
    tcp_reap(st);
}

void stack_advance(struct us_stack *st, uint64_t now_ns)
{
    if (now_ns > st->now_ns)
        st->now_ns = now_ns;
    wheel_advance(&st->wheel, st->now_ns / NSEC_PER_MSEC);
    stack_flush(st);
}

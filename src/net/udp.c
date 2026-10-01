#include <arpa/inet.h>
#include <string.h>

#include "core/checksum.h"
#include "net/proto.h"
#include "stack.h"

static struct udp_bind *find_bind(struct us_stack *st, uint16_t port)
{
    for (uint32_t i = 0; i < st->udp_count; i++)
        if (st->udp[i].port == port)
            return &st->udp[i];
    return NULL;
}

int us_udp_bind(struct us_stack *st, uint16_t port, us_udp_fn fn, void *ctx)
{
    if (find_bind(st, port) || st->udp_count == STACK_MAX_UDP_BINDS)
        return -1;
    st->udp[st->udp_count++] = (struct udp_bind){ port, fn, ctx };
    return 0;
}

int us_udp_send(struct us_stack *st, uint16_t sport, uint32_t daddr, uint16_t dport,
                const void *data, size_t len)
{
    if (len > STACK_MAX_SEGMENT)
        return -1;
    uint8_t *out = st->txbuf;
    struct udp_hdr *h = (struct udp_hdr *)(out + IPV4_HDR_LEN);
    uint16_t ulen = (uint16_t)(UDP_HDR_LEN + len);
    *h = (struct udp_hdr){ htons(sport), htons(dport), htons(ulen), 0 };
    memcpy(h + 1, data, len);

    uint64_t sum = csum_pseudo(st->cfg.addr, daddr, IPPROTO_UDP_, ulen);
    uint16_t csum = csum_fold(csum_add(sum, h, ulen));
    h->csum = csum ? csum : 0xffff;
    ip_output(st, out, ulen, IPPROTO_UDP_, daddr, NULL);
    return 0;
}

void udp_input(struct us_stack *st, const uint8_t *ip, const uint8_t *l4, size_t len,
               bool csum_ok)
{
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *)ip;
    const struct udp_hdr *h = (const struct udp_hdr *)l4;
    if (len < UDP_HDR_LEN || ntohs(h->len) < UDP_HDR_LEN || ntohs(h->len) > len) {
        st->stats.udp_bad++;
        return;
    }
    uint16_t ulen = ntohs(h->len);
    uint32_t saddr = ntohl(iph->saddr);
    if (h->csum && !csum_ok) {
        uint64_t sum = csum_pseudo(saddr, st->cfg.addr, IPPROTO_UDP_, ulen);
        if (csum_fold(csum_add(sum, l4, ulen)) != 0) {
            st->stats.udp_bad++;
            return;
        }
    }

    st->stats.udp_rx++;
    struct udp_bind *b = find_bind(st, ntohs(h->dport));
    if (!b) {
        st->stats.udp_no_port++;
        icmp_send_port_unreach(st, ip, (size_t)ntohs(iph->total_len));
        return;
    }
    b->fn(st, b->ctx, saddr, ntohs(h->sport), b->port, l4 + UDP_HDR_LEN, ulen - UDP_HDR_LEN);
}

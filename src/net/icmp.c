#include <arpa/inet.h>
#include <string.h>

#include "core/checksum.h"
#include "net/proto.h"
#include "stack.h"

static uint16_t first_word(const void *p)
{
    uint16_t w;
    memcpy(&w, p, sizeof(w));
    return w;
}

void icmp_input(struct us_stack *st, const uint8_t *ip, size_t iplen, const uint8_t *l4,
                size_t len)
{
    (void)iplen;
    if (len < ICMP_HDR_LEN || len > STACK_TXBUF_SIZE - IPV4_HDR_LEN ||
        csum_compute(l4, len) != 0) {
        st->stats.icmp_bad++;
        return;
    }
    const struct icmp_hdr *req = (const struct icmp_hdr *)l4;
    if (req->type != ICMP_ECHO_REQUEST || req->code != 0)
        return;

    uint8_t *out = st->txbuf;
    struct icmp_hdr *rep = (struct icmp_hdr *)(out + IPV4_HDR_LEN);
    memcpy(rep, l4, len);
    uint16_t before = first_word(rep);
    rep->type = ICMP_ECHO_REPLY;
    rep->csum = csum_replace16(rep->csum, before, first_word(rep));

    const struct ipv4_hdr *iph = (const struct ipv4_hdr *)ip;
    st->stats.icmp_echo++;
    ip_output(st, out, len, IPPROTO_ICMP_, ntohl(iph->saddr), NULL);
}

void icmp_send_port_unreach(struct us_stack *st, const uint8_t *ip, size_t iplen)
{
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *)ip;
    size_t ihl = (size_t)(iph->ver_ihl & 0x0f) * 4;
    size_t quoted = ihl + 8 < iplen ? ihl + 8 : iplen;

    uint8_t *out = st->txbuf;
    struct icmp_hdr *h = (struct icmp_hdr *)(out + IPV4_HDR_LEN);
    *h = (struct icmp_hdr){ .type = ICMP_DEST_UNREACH, .code = ICMP_PORT_UNREACH };
    memcpy(out + IPV4_HDR_LEN + ICMP_HDR_LEN, ip, quoted);
    h->csum = csum_compute(h, ICMP_HDR_LEN + quoted);
    ip_output(st, out, ICMP_HDR_LEN + quoted, IPPROTO_ICMP_, ntohl(iph->saddr), NULL);
}

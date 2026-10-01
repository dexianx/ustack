#pragma once

#include <stdint.h>

#define IPPROTO_ICMP_ 1
#define IPPROTO_TCP_  6
#define IPPROTO_UDP_  17

#define IPV4_HDR_LEN 20
#define TCP_HDR_LEN  20
#define UDP_HDR_LEN  8
#define ICMP_HDR_LEN 8

#define IP_DF     0x4000
#define IP_MF     0x2000
#define IP_OFFMASK 0x1fff

struct ipv4_hdr {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum;
    uint32_t saddr;
    uint32_t daddr;
};

struct tcp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t doff;
    uint8_t flags;
    uint16_t wnd;
    uint16_t csum;
    uint16_t urg;
};

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t csum;
};

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
};

_Static_assert(sizeof(struct ipv4_hdr) == IPV4_HDR_LEN, "ipv4 header layout");
_Static_assert(sizeof(struct tcp_hdr) == TCP_HDR_LEN, "tcp header layout");
_Static_assert(sizeof(struct udp_hdr) == UDP_HDR_LEN, "udp header layout");
_Static_assert(sizeof(struct icmp_hdr) == ICMP_HDR_LEN, "icmp header layout");

#define ICMP_ECHO_REPLY   0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO_REQUEST 8
#define ICMP_PORT_UNREACH 3

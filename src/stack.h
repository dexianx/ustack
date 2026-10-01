#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/siphash.h"
#include "core/timer.h"
#include "ustack.h"

struct cc_ops;
struct tcp_conn;
struct tcp_listener;

#define STACK_TXBUF_SIZE (65536 + 128)
#define STACK_MAX_UDP_BINDS 16
#define STACK_MAX_SEGMENT (65535 - 20 - 60)

struct netif_tx {
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    bool csum_partial;
};

struct netif {
    void (*tx)(void *ctx, const uint8_t *pkt, size_t len, const struct netif_tx *meta);
    void *ctx;
    bool offload;
};

struct stack_config {
    uint32_t addr;
    uint16_t mtu;
    uint32_t sndbuf;
    uint32_t rcvbuf;
    uint32_t rto_min_us;
    uint32_t rto_max_us;
    uint32_t rto_init_us;
    uint32_t msl_ms;
    uint32_t delack_ms;
    uint32_t max_conns;
    uint32_t max_retries;
    uint32_t syn_retries;
    uint16_t ephemeral_lo;
    uint16_t ephemeral_hi;
    bool sack;
    bool timestamps;
    bool wscale;
    const struct cc_ops *cc;
    uint64_t seed;
};

struct stack_stats {
    uint64_t rx_packets, rx_bytes, tx_packets, tx_bytes;
    uint64_t ip_bad, ip_not_v4, ip_not_ours, ip_frag_dropped, ip_unknown_proto;
    uint64_t icmp_bad, icmp_echo;
    uint64_t udp_rx, udp_bad, udp_no_port;
    uint64_t tcp_rx_segs, tcp_tx_segs, tcp_bad;
    uint64_t tcp_rst_tx, tcp_retrans, tcp_rto, tcp_tlp, tcp_recoveries;
    uint64_t tcp_ooo, tcp_dup, tcp_challenge_ack, tcp_paws;
    uint64_t tcp_opened, tcp_closed, tcp_refused, tcp_syn_dropped;
};

struct udp_bind {
    uint16_t port;
    us_udp_fn fn;
    void *ctx;
};

struct tcp_table {
    struct tcp_conn **buckets;
    uint32_t mask;
    uint32_t count;
    struct tcp_conn *cache;
    struct tcp_listener *listeners;
};

struct us_stack {
    struct stack_config cfg;
    struct netif nif;
    uint64_t now_ns;
    struct timer_wheel wheel;
    struct tcp_table tcp;
    struct udp_bind udp[STACK_MAX_UDP_BINDS];
    uint32_t udp_count;
    struct stack_stats stats;
    struct siphash_key hash_key;
    struct siphash_key isn_key;
    uint64_t rng;
    uint16_t ip_id;
    uint16_t next_port;
    struct tcp_conn *dirty;
    struct tcp_conn *dead;
    uint8_t *txbuf;
};

void stack_config_defaults(struct stack_config *cfg);
struct us_stack *stack_create(const struct stack_config *cfg, const struct netif *nif,
                              uint64_t now_ns);
void stack_destroy(struct us_stack *st);

void stack_input(struct us_stack *st, const uint8_t *pkt, size_t len, bool csum_ok);
void stack_flush(struct us_stack *st);
void stack_advance(struct us_stack *st, uint64_t now_ns);

static inline uint64_t stack_now_ms(const struct us_stack *st) { return st->now_ns / 1000000; }

void ip_output(struct us_stack *st, uint8_t *pkt, size_t l4len, uint8_t proto, uint32_t daddr,
               const struct netif_tx *meta);

void icmp_input(struct us_stack *st, const uint8_t *ip, size_t iplen, const uint8_t *l4,
                size_t len);
void icmp_send_port_unreach(struct us_stack *st, const uint8_t *ip, size_t iplen);
void udp_input(struct us_stack *st, const uint8_t *ip, const uint8_t *l4, size_t len,
               bool csum_ok);

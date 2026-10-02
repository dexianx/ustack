#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stack.h"

/* One direction of a simulated link. */
struct sim_link {
    uint64_t delay_ns;
    uint64_t jitter_ns;
    uint64_t rate_bps;
    uint32_t queue_bytes;
    double loss;
    double dup;
    double corrupt;
};

struct sim_event {
    uint64_t time;
    uint64_t order;
    int dst;
    uint32_t len;
    uint8_t *data;
};

struct sim_port {
    struct simnet *net;
    int idx;
};

struct sim_stats {
    uint64_t sent;
    uint64_t delivered;
    uint64_t lost;
    uint64_t queue_drops;
    uint64_t duplicated;
    uint64_t corrupted;
};

struct simnet {
    uint64_t now;
    uint64_t rng;
    uint64_t order;
    struct us_stack *node[2];
    struct sim_port port[2];
    struct sim_link link[2];
    uint64_t link_free[2];
    struct sim_event *heap;
    uint32_t heap_n;
    uint32_t heap_cap;
    struct sim_stats stats;
    bool trace;
    bool (*check)(struct simnet *net, void *ctx);
    void *check_ctx;
};

#define SIM_ADDR_A 0x0a000001u
#define SIM_ADDR_B 0x0a000002u

void simnet_init(struct simnet *net, uint64_t seed);
void simnet_free(struct simnet *net);
struct us_stack *simnet_attach(struct simnet *net, int idx, const struct stack_config *cfg);
void simnet_inject(struct simnet *net, int dst, const uint8_t *pkt, uint32_t len, uint64_t at);
bool simnet_step(struct simnet *net);
bool simnet_step_until(struct simnet *net, uint64_t limit);
bool simnet_run(struct simnet *net, bool (*done)(void *ctx), void *ctx, uint64_t deadline_ns);
bool simnet_check_all(struct simnet *net, char *why, size_t n);

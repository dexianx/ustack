#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WHEEL_BITS  12
#define WHEEL_SLOTS (1u << WHEEL_BITS)

struct timer;
typedef void (*timer_fn)(struct timer *t);

struct timer {
    struct timer *next;
    struct timer **pprev;
    uint64_t expires;
    timer_fn fn;
};

/* Hashed timing wheel (Varghese & Lauck, scheme 6) with 1 ms ticks. */
struct timer_wheel {
    struct timer *slots[WHEEL_SLOTS];
    uint64_t tick;
    uint32_t armed;
};

void wheel_init(struct timer_wheel *w, uint64_t now_ms);
void wheel_advance(struct timer_wheel *w, uint64_t now_ms);
uint64_t wheel_next_expiry(const struct timer_wheel *w);

void timer_init(struct timer *t, timer_fn fn);
void timer_arm(struct timer_wheel *w, struct timer *t, uint64_t expires_ms);
void timer_cancel(struct timer_wheel *w, struct timer *t);

static inline bool timer_pending(const struct timer *t) { return t->pprev != NULL; }

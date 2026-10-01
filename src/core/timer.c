#include "core/timer.h"

#include <string.h>

void wheel_init(struct timer_wheel *w, uint64_t now_ms)
{
    memset(w, 0, sizeof(*w));
    w->tick = now_ms;
}

void timer_init(struct timer *t, timer_fn fn)
{
    t->next = NULL;
    t->pprev = NULL;
    t->expires = 0;
    t->fn = fn;
}

static void unlink_timer(struct timer *t)
{
    *t->pprev = t->next;
    if (t->next)
        t->next->pprev = t->pprev;
    t->next = NULL;
    t->pprev = NULL;
}

void timer_cancel(struct timer_wheel *w, struct timer *t)
{
    if (!timer_pending(t))
        return;
    unlink_timer(t);
    w->armed--;
}

void timer_arm(struct timer_wheel *w, struct timer *t, uint64_t expires_ms)
{
    timer_cancel(w, t);
    if (expires_ms <= w->tick)
        expires_ms = w->tick + 1;
    t->expires = expires_ms;
    struct timer **slot = &w->slots[expires_ms & (WHEEL_SLOTS - 1)];
    t->next = *slot;
    if (t->next)
        t->next->pprev = &t->next;
    t->pprev = slot;
    *slot = t;
    w->armed++;
}

static void run_slot(struct timer_wheel *w, uint64_t tick)
{
    struct timer **pp = &w->slots[tick & (WHEEL_SLOTS - 1)];
    while (*pp) {
        struct timer *t = *pp;
        if (t->expires > tick) {
            pp = &t->next;
            continue;
        }
        unlink_timer(t);
        w->armed--;
        t->fn(t);
    }
}

void wheel_advance(struct timer_wheel *w, uint64_t now_ms)
{
    while (w->tick < now_ms) {
        if (!w->armed) {
            w->tick = now_ms;
            return;
        }
        w->tick++;
        run_slot(w, w->tick);
    }
}

uint64_t wheel_next_expiry(const struct timer_wheel *w)
{
    uint64_t best = UINT64_MAX;
    if (!w->armed)
        return best;
    for (uint32_t i = 0; i < WHEEL_SLOTS; i++)
        for (const struct timer *t = w->slots[i]; t; t = t->next)
            if (t->expires < best)
                best = t->expires;
    return best;
}

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t seq_t;

static inline bool seq_lt(seq_t a, seq_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(seq_t a, seq_t b) { return (int32_t)(a - b) <= 0; }
static inline bool seq_gt(seq_t a, seq_t b) { return (int32_t)(a - b) > 0; }
static inline bool seq_ge(seq_t a, seq_t b) { return (int32_t)(a - b) >= 0; }
static inline seq_t seq_max(seq_t a, seq_t b) { return seq_gt(a, b) ? a : b; }
static inline seq_t seq_min(seq_t a, seq_t b) { return seq_lt(a, b) ? a : b; }

static inline bool seq_in(seq_t s, seq_t lo, seq_t hi)
{
    return seq_le(lo, s) && seq_lt(s, hi);
}

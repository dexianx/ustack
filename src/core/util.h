#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define container_of(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#define NSEC_PER_USEC 1000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_SEC  1000000000ull

static inline uint32_t round_up_pow2(uint32_t v)
{
    if (v <= 1)
        return 1;
    return 1u << (32 - __builtin_clz(v - 1));
}

static inline uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static inline double rand_unit(uint64_t *state)
{
    return (double)(splitmix64(state) >> 11) * 0x1.0p-53;
}

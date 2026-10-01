#include "core/siphash.h"

#include <string.h>

static inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

static inline uint64_t load_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

#define SIPROUND(v0, v1, v2, v3)                                  \
    do {                                                          \
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32); \
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;                    \
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;                    \
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32); \
    } while (0)

uint64_t siphash24(const struct siphash_key *key, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint64_t v0 = key->k0 ^ 0x736f6d6570736575ull;
    uint64_t v1 = key->k1 ^ 0x646f72616e646f6dull;
    uint64_t v2 = key->k0 ^ 0x6c7967656e657261ull;
    uint64_t v3 = key->k1 ^ 0x7465646279746573ull;

    size_t blocks = len / 8;
    for (size_t i = 0; i < blocks; i++, p += 8) {
        uint64_t m = load_le64(p);
        v3 ^= m;
        SIPROUND(v0, v1, v2, v3);
        SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }

    uint8_t tail[8] = { 0 };
    memcpy(tail, p, len & 7);
    tail[7] = (uint8_t)len;
    uint64_t m = load_le64(tail);
    v3 ^= m;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    v0 ^= m;

    v2 ^= 0xff;
    for (int i = 0; i < 4; i++)
        SIPROUND(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

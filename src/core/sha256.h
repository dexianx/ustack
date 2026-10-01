#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32

struct sha256 {
    uint32_t h[8];
    uint64_t total;
    uint8_t block[64];
    uint32_t used;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST_LEN]);
void sha256_hex(const uint8_t digest[SHA256_DIGEST_LEN], char out[2 * SHA256_DIGEST_LEN + 1]);

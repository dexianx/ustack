#pragma once

#include <stddef.h>
#include <stdint.h>

struct siphash_key {
    uint64_t k0;
    uint64_t k1;
};

uint64_t siphash24(const struct siphash_key *key, const void *data, size_t len);

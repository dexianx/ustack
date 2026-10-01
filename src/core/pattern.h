#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Deterministic test stream: byte i = table[i mod PATTERN_PERIOD]. The prime
 * period keeps any misplaced segment from lining up with an identical copy.
 */
#define PATTERN_PERIOD 1048573u

void pattern_init(void);
void pattern_fill(uint64_t offset, uint8_t *out, size_t len);

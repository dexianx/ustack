#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Internet checksum (RFC 1071). Sums are kept in native byte order: the
 * one's-complement sum is byte-order independent, so a native-order result
 * stored with a native store lands in memory as the correct network-order
 * checksum.
 */
uint64_t csum_add(uint64_t sum, const void *buf, size_t len);
uint64_t csum_pseudo(uint32_t saddr, uint32_t daddr, uint8_t proto, uint16_t len);
uint16_t csum_fold(uint64_t sum);
uint16_t csum_compute(const void *buf, size_t len);
uint16_t csum_replace16(uint16_t csum, uint16_t old_word, uint16_t new_word);

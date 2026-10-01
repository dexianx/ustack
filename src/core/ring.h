#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Byte ring with a power-of-two logical capacity `cap` and a physical
 * allocation `size` that grows on demand up to `cap`. Offsets passed to the
 * *_at functions are relative to the head, so callers can address data beyond
 * `len` (used to place out-of-order TCP payload directly at its position).
 */
struct ring {
    uint8_t *buf;
    uint32_t cap;
    uint32_t size;
    uint32_t head;
    uint32_t len;
};

void ring_init(struct ring *r, uint32_t cap);
void ring_free(struct ring *r);
bool ring_reserve(struct ring *r, uint32_t need);

static inline uint32_t ring_space(const struct ring *r) { return r->cap - r->len; }

void ring_write_at(struct ring *r, uint32_t off, const void *src, uint32_t n);
void ring_read_at(const struct ring *r, uint32_t off, void *dst, uint32_t n);
uint32_t ring_append(struct ring *r, const void *src, uint32_t n);
uint32_t ring_read(struct ring *r, void *dst, uint32_t n);
void ring_consume(struct ring *r, uint32_t n);
void ring_release_if_empty(struct ring *r);

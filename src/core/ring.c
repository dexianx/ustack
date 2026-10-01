#include "core/ring.h"

#include <stdlib.h>
#include <string.h>

#include "core/util.h"

#define RING_MIN_ALLOC 4096u

void ring_init(struct ring *r, uint32_t cap)
{
    *r = (struct ring){ .cap = round_up_pow2(cap) };
}

void ring_free(struct ring *r)
{
    free(r->buf);
    *r = (struct ring){ .cap = r->cap };
}

bool ring_reserve(struct ring *r, uint32_t need)
{
    if (need <= r->size)
        return true;
    if (need > r->cap)
        return false;
    uint32_t size = MIN(MAX(round_up_pow2(need), MAX(r->size * 2, RING_MIN_ALLOC)), r->cap);
    uint8_t *buf = malloc(size);
    if (!buf)
        return false;
    if (r->size)
        ring_read_at(r, 0, buf, r->size);
    free(r->buf);
    r->buf = buf;
    r->size = size;
    r->head = 0;
    return true;
}

void ring_write_at(struct ring *r, uint32_t off, const void *src, uint32_t n)
{
    uint32_t pos = (r->head + off) & (r->size - 1);
    uint32_t first = MIN(n, r->size - pos);
    memcpy(r->buf + pos, src, first);
    memcpy(r->buf, (const uint8_t *)src + first, n - first);
}

void ring_read_at(const struct ring *r, uint32_t off, void *dst, uint32_t n)
{
    uint32_t pos = (r->head + off) & (r->size - 1);
    uint32_t first = MIN(n, r->size - pos);
    memcpy(dst, r->buf + pos, first);
    memcpy((uint8_t *)dst + first, r->buf, n - first);
}

uint32_t ring_append(struct ring *r, const void *src, uint32_t n)
{
    n = MIN(n, ring_space(r));
    if (!n || !ring_reserve(r, r->len + n))
        return 0;
    ring_write_at(r, r->len, src, n);
    r->len += n;
    return n;
}

uint32_t ring_read(struct ring *r, void *dst, uint32_t n)
{
    n = MIN(n, r->len);
    if (!n)
        return 0;
    ring_read_at(r, 0, dst, n);
    ring_consume(r, n);
    return n;
}

void ring_consume(struct ring *r, uint32_t n)
{
    if (!n)
        return;
    r->head = (r->head + n) & (r->size - 1);
    r->len -= n;
}

void ring_release_if_empty(struct ring *r)
{
    if (!r->len && r->buf)
        ring_free(r);
}

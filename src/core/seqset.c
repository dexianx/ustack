#include "core/seqset.h"

#include <stdlib.h>
#include <string.h>

void seqset_init(struct seqset *s, uint32_t max_ranges)
{
    *s = (struct seqset){ .max = max_ranges };
}

void seqset_free(struct seqset *s)
{
    free(s->r);
    seqset_init(s, s->max);
}

static bool reserve_one(struct seqset *s)
{
    if (s->n >= s->max)
        return false;
    if (s->n < s->cap)
        return true;
    uint32_t cap = s->cap ? s->cap * 2 : 8;
    struct seq_range *r = realloc(s->r, cap * sizeof(*r));
    if (!r)
        return false;
    s->r = r;
    s->cap = cap;
    return true;
}

bool seqset_add(struct seqset *s, seq_t start, seq_t end)
{
    if (!seq_lt(start, end))
        return true;

    uint32_t lo = 0;
    while (lo < s->n && seq_lt(s->r[lo].end, start))
        lo++;
    uint32_t hi = lo;
    while (hi < s->n && seq_le(s->r[hi].start, end))
        hi++;

    if (lo == hi) {
        if (!reserve_one(s))
            return false;
        memmove(&s->r[lo + 1], &s->r[lo], (s->n - lo) * sizeof(*s->r));
        s->r[lo] = (struct seq_range){ start, end };
        s->n++;
        return true;
    }

    s->r[lo].start = seq_min(s->r[lo].start, start);
    s->r[lo].end = seq_max(s->r[hi - 1].end, end);
    memmove(&s->r[lo + 1], &s->r[hi], (s->n - hi) * sizeof(*s->r));
    s->n -= hi - lo - 1;
    return true;
}

void seqset_trim_below(struct seqset *s, seq_t seq)
{
    uint32_t drop = 0;
    while (drop < s->n && seq_le(s->r[drop].end, seq))
        drop++;
    memmove(s->r, &s->r[drop], (s->n - drop) * sizeof(*s->r));
    s->n -= drop;
    if (s->n && seq_lt(s->r[0].start, seq))
        s->r[0].start = seq;
}

bool seqset_pop_contiguous(struct seqset *s, seq_t from, seq_t *end)
{
    if (!s->n || seq_gt(s->r[0].start, from))
        return false;
    *end = s->r[0].end;
    memmove(s->r, &s->r[1], (s->n - 1) * sizeof(*s->r));
    s->n--;
    return true;
}

const struct seq_range *seqset_find(const struct seqset *s, seq_t seq)
{
    for (uint32_t i = 0; i < s->n; i++)
        if (seq_in(seq, s->r[i].start, s->r[i].end))
            return &s->r[i];
    return NULL;
}

uint32_t seqset_bytes(const struct seqset *s)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < s->n; i++)
        total += s->r[i].end - s->r[i].start;
    return total;
}

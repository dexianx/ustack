#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/seq.h"

struct seq_range {
    seq_t start;
    seq_t end;
};

/* Sorted, disjoint, non-adjacent half-open ranges in sequence space. */
struct seqset {
    struct seq_range *r;
    uint32_t n;
    uint32_t cap;
    uint32_t max;
};

void seqset_init(struct seqset *s, uint32_t max_ranges);
void seqset_free(struct seqset *s);
bool seqset_add(struct seqset *s, seq_t start, seq_t end);
void seqset_trim_below(struct seqset *s, seq_t seq);
bool seqset_pop_contiguous(struct seqset *s, seq_t from, seq_t *end);
const struct seq_range *seqset_find(const struct seqset *s, seq_t seq);
uint32_t seqset_bytes(const struct seqset *s);

static inline bool seqset_empty(const struct seqset *s) { return s->n == 0; }

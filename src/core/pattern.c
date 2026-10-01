#include "core/pattern.h"

#include <string.h>

#include "core/util.h"

static uint8_t table[PATTERN_PERIOD];
static bool ready;

void pattern_init(void)
{
    if (ready)
        return;
    uint64_t state = 0x75737461636bull;
    for (uint32_t i = 0; i < PATTERN_PERIOD; i += 8) {
        uint64_t w = splitmix64(&state);
        for (uint32_t b = 0; b < 8 && i + b < PATTERN_PERIOD; b++)
            table[i + b] = (uint8_t)(w >> (8 * b));
    }
    ready = true;
}

void pattern_fill(uint64_t offset, uint8_t *out, size_t len)
{
    size_t pos = (size_t)(offset % PATTERN_PERIOD);
    while (len) {
        size_t take = MIN(len, PATTERN_PERIOD - pos);
        memcpy(out, table + pos, take);
        out += take;
        len -= take;
        pos = 0;
    }
}

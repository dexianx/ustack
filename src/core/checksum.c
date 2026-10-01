#include "core/checksum.h"

#include <arpa/inet.h>
#include <string.h>

static inline uint64_t add_carry(uint64_t sum, uint64_t v)
{
    sum += v;
    return sum + (sum < v);
}

uint64_t csum_add(uint64_t sum, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len >= 32) {
        uint64_t w[4];
        memcpy(w, p, sizeof(w));
        sum = add_carry(sum, w[0]);
        sum = add_carry(sum, w[1]);
        sum = add_carry(sum, w[2]);
        sum = add_carry(sum, w[3]);
        p += 32;
        len -= 32;
    }
    while (len >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        sum = add_carry(sum, w);
        p += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t w;
        memcpy(&w, p, 4);
        sum = add_carry(sum, w);
        p += 4;
        len -= 4;
    }
    if (len >= 2) {
        uint16_t w;
        memcpy(&w, p, 2);
        sum = add_carry(sum, w);
        p += 2;
        len -= 2;
    }
    if (len) {
        uint8_t tail[2] = { *p, 0 };
        uint16_t w;
        memcpy(&w, tail, 2);
        sum = add_carry(sum, w);
    }
    return sum;
}

uint64_t csum_pseudo(uint32_t saddr, uint32_t daddr, uint8_t proto, uint16_t len)
{
    struct {
        uint32_t saddr, daddr;
        uint8_t zero, proto;
        uint16_t len;
    } ph = { htonl(saddr), htonl(daddr), 0, proto, htons(len) };
    return csum_add(0, &ph, sizeof(ph));
}

uint16_t csum_fold(uint64_t sum)
{
    sum = (sum & 0xffffffffu) + (sum >> 32);
    sum = (sum & 0xffffffffu) + (sum >> 32);
    sum = (sum & 0xffffu) + (sum >> 16);
    sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t csum_compute(const void *buf, size_t len)
{
    return csum_fold(csum_add(0, buf, len));
}

/* RFC 1624 eqn. 3: HC' = ~(~HC + ~m + m') */
uint16_t csum_replace16(uint16_t csum, uint16_t old_word, uint16_t new_word)
{
    uint32_t sum = (uint16_t)~csum;
    sum += (uint16_t)~old_word;
    sum += new_word;
    sum = (sum & 0xffffu) + (sum >> 16);
    sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

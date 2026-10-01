#include <arpa/inet.h>
#include <stdlib.h>

#include "core/checksum.h"
#include "core/pattern.h"
#include "core/ring.h"
#include "core/seq.h"
#include "core/seqset.h"
#include "core/sha256.h"
#include "core/siphash.h"
#include "core/timer.h"
#include "core/util.h"
#include "test.h"

static uint16_t csum_reference(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (len & 1)
        sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

TEST(checksum_rfc1071_example)
{
    const uint8_t data[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    uint16_t c = csum_compute(data, sizeof(data));
    CHECK_EQ(ntohs(c), 0x220d);
}

TEST(checksum_known_ipv4_header)
{
    const uint8_t hdr[] = { 0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                            0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7 };
    CHECK_EQ(csum_compute(hdr, sizeof(hdr)), 0);
    uint8_t zeroed[sizeof(hdr)];
    memcpy(zeroed, hdr, sizeof(hdr));
    zeroed[10] = zeroed[11] = 0;
    CHECK_EQ(ntohs(csum_compute(zeroed, sizeof(zeroed))), 0xb861);
}

TEST(checksum_matches_reference_on_random_input)
{
    uint64_t rng = 42;
    uint8_t buf[2048];
    for (int iter = 0; iter < 2000; iter++) {
        size_t len = splitmix64(&rng) % sizeof(buf);
        size_t off = splitmix64(&rng) % 8;
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)splitmix64(&rng);
        if (len < off)
            off = 0;
        uint16_t got = ntohs(csum_compute(buf + off, len - off));
        if (got != csum_reference(buf + off, len - off)) {
            FAIL_AT("mismatch at len %zu off %zu", len, off);
            return;
        }
    }
}

TEST(checksum_incremental_update_rfc1624)
{
    uint64_t rng = 7;
    uint8_t buf[64];
    for (int iter = 0; iter < 1000; iter++) {
        for (size_t i = 0; i < sizeof(buf); i++)
            buf[i] = (uint8_t)splitmix64(&rng);
        uint16_t csum = csum_compute(buf, sizeof(buf));
        size_t at = (splitmix64(&rng) % (sizeof(buf) / 2)) * 2;
        uint16_t old_w, new_w = (uint16_t)splitmix64(&rng);
        memcpy(&old_w, buf + at, 2);
        memcpy(buf + at, &new_w, 2);
        if (csum_replace16(csum, old_w, new_w) != csum_compute(buf, sizeof(buf))) {
            uint16_t full = csum_compute(buf, sizeof(buf));
            uint16_t inc = csum_replace16(csum, old_w, new_w);
            if (!((full == 0x0000 && inc == 0xffff) || (full == 0xffff && inc == 0x0000))) {
                FAIL_AT("incremental mismatch at iter %d", iter);
                return;
            }
        }
    }
}

TEST(checksum_pseudo_header)
{
    const uint8_t ph[] = { 10, 0, 0, 1, 10, 0, 0, 2, 0, 6, 0, 20 };
    CHECK_EQ(csum_fold(csum_pseudo(0x0a000001, 0x0a000002, 6, 20)), csum_compute(ph, sizeof(ph)));
}

TEST(seq_compare_wraps)
{
    CHECK(seq_lt(0xfffffff0u, 0x10));
    CHECK(seq_gt(0x10, 0xfffffff0u));
    CHECK(seq_le(5, 5));
    CHECK(!seq_lt(5, 5));
    CHECK(seq_in(0, 0xffffff00u, 0x100));
    CHECK(!seq_in(0x100, 0xffffff00u, 0x100));
    CHECK_EQ(seq_max(0xfffffff0u, 0x10), 0x10);
}

TEST(ring_wraparound_and_offsets)
{
    struct ring r;
    ring_init(&r, 16);
    uint8_t in[12], out[16];
    for (int i = 0; i < 12; i++)
        in[i] = (uint8_t)i;
    CHECK_EQ(ring_append(&r, in, 12), 12);
    ring_consume(&r, 10);
    CHECK_EQ(ring_append(&r, in, 12), 12);
    CHECK_EQ(r.len, 14);
    CHECK_EQ(ring_append(&r, in, 12), 2);
    ring_read_at(&r, 2, out, 12);
    CHECK(!memcmp(out, in, 12));
    ring_write_at(&r, 0, "ab", 2);
    CHECK_EQ(ring_read(&r, out, 3), 3);
    CHECK(!memcmp(out, "ab\0", 3));
    ring_free(&r);
}

TEST(ring_grows_on_demand_preserving_wrapped_data)
{
    struct ring r;
    ring_init(&r, 1u << 20);
    CHECK_EQ(r.size, 0);
    uint8_t chunk[3000];
    memset(chunk, 7, sizeof(chunk));
    CHECK_EQ(ring_append(&r, chunk, sizeof(chunk)), sizeof(chunk));
    CHECK_EQ(r.size, 4096);
    ring_consume(&r, 2000);
    CHECK_EQ(ring_append(&r, chunk, sizeof(chunk)), sizeof(chunk));
    CHECK_EQ(ring_append(&r, chunk, sizeof(chunk)), sizeof(chunk));
    CHECK_EQ(r.size, 8192);
    uint8_t out[7000];
    CHECK_EQ(ring_read(&r, out, sizeof(out)), 7000);
    for (size_t i = 0; i < sizeof(out); i++)
        if (out[i] != 7) {
            FAIL_AT("byte %zu lost across growth", i);
            break;
        }
    ring_free(&r);
}

TEST(seqset_merges_and_trims)
{
    struct seqset s;
    seqset_init(&s, 16);
    CHECK(seqset_add(&s, 100, 200));
    CHECK(seqset_add(&s, 300, 400));
    CHECK(seqset_add(&s, 500, 600));
    CHECK_EQ(s.n, 3);
    CHECK(seqset_add(&s, 200, 300));
    CHECK_EQ(s.n, 2);
    CHECK_EQ(s.r[0].start, 100);
    CHECK_EQ(s.r[0].end, 400);
    CHECK(seqset_add(&s, 50, 700));
    CHECK_EQ(s.n, 1);
    CHECK_EQ(seqset_bytes(&s), 650);
    seqset_trim_below(&s, 120);
    CHECK_EQ(s.r[0].start, 120);
    seq_t end;
    CHECK(!seqset_pop_contiguous(&s, 100, &end));
    CHECK(seqset_pop_contiguous(&s, 120, &end));
    CHECK_EQ(end, 700);
    CHECK(seqset_empty(&s));
    seqset_free(&s);
}

TEST(seqset_across_sequence_wrap)
{
    struct seqset s;
    seqset_init(&s, 16);
    seqset_add(&s, 0xfffffff0u, 0x10);
    seqset_add(&s, 0x20, 0x30);
    seqset_add(&s, 0xffffff00u, 0xfffffff0u);
    CHECK_EQ(s.n, 2);
    CHECK_EQ(s.r[0].start, 0xffffff00u);
    CHECK_EQ(s.r[0].end, 0x10);
    CHECK(seqset_find(&s, 0x5) == &s.r[0]);
    CHECK(seqset_find(&s, 0x15) == NULL);
    seqset_free(&s);
}

TEST(seqset_respects_range_limit)
{
    struct seqset s;
    seqset_init(&s, 2);
    CHECK(seqset_add(&s, 0, 1));
    CHECK(seqset_add(&s, 10, 11));
    CHECK(!seqset_add(&s, 20, 21));
    CHECK(seqset_add(&s, 1, 10));
    CHECK_EQ(s.n, 1);
    seqset_free(&s);
}

static int fired[8];

static void count_fire(struct timer *t)
{
    fired[t->expires % 8]++;
}

TEST(timer_wheel_fires_in_order_and_cancels)
{
    struct timer_wheel w;
    struct timer a, b, c, far;
    memset(fired, 0, sizeof(fired));
    wheel_init(&w, 1000);
    timer_init(&a, count_fire);
    timer_init(&b, count_fire);
    timer_init(&c, count_fire);
    timer_init(&far, count_fire);
    timer_arm(&w, &a, 1001);
    timer_arm(&w, &b, 1002);
    timer_arm(&w, &c, 1003);
    timer_arm(&w, &far, 1000 + WHEEL_SLOTS + 1);
    timer_cancel(&w, &b);
    CHECK_EQ(w.armed, 3);
    CHECK_EQ(wheel_next_expiry(&w), 1001);
    wheel_advance(&w, 1003);
    CHECK_EQ(fired[1001 % 8], 1);
    CHECK_EQ(fired[1002 % 8], 0);
    CHECK_EQ(fired[1003 % 8], 1);
    CHECK(timer_pending(&far));
    wheel_advance(&w, 1000 + WHEEL_SLOTS);
    CHECK(timer_pending(&far));
    wheel_advance(&w, 1000 + WHEEL_SLOTS + 1);
    CHECK(!timer_pending(&far));
    CHECK_EQ(w.armed, 0);
}

TEST(timer_arm_in_past_fires_next_tick)
{
    struct timer_wheel w;
    struct timer t;
    memset(fired, 0, sizeof(fired));
    wheel_init(&w, 50);
    timer_init(&t, count_fire);
    timer_arm(&w, &t, 10);
    CHECK_EQ(t.expires, 51);
    wheel_advance(&w, 51);
    CHECK_EQ(fired[51 % 8], 1);
}

TEST(siphash_reference_vectors)
{
    struct siphash_key key = { 0x0706050403020100ull, 0x0f0e0d0c0b0a0908ull };
    uint8_t msg[16];
    for (int i = 0; i < 16; i++)
        msg[i] = (uint8_t)i;
    CHECK(siphash24(&key, msg, 0) == 0x726fdb47dd0e0e31ull);
    CHECK(siphash24(&key, msg, 1) == 0x74f839c593dc67fdull);
    CHECK(siphash24(&key, msg, 15) == 0xa129ca6149be45e5ull);
}

static void sha_hex_of(const void *data, size_t len, char *hex)
{
    struct sha256 s;
    uint8_t d[SHA256_DIGEST_LEN];
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, d);
    sha256_hex(d, hex);
}

TEST(sha256_nist_vectors)
{
    char hex[65];
    sha_hex_of("", 0, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha_hex_of("abc", 3, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_hex_of(m, strlen(m), hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(sha256_streaming_equals_one_shot)
{
    uint8_t data[1000];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7);
    char one[65], two[65];
    sha_hex_of(data, sizeof(data), one);
    struct sha256 s;
    uint8_t d[SHA256_DIGEST_LEN];
    sha256_init(&s);
    for (size_t off = 0; off < sizeof(data); off += 37)
        sha256_update(&s, data + off, MIN((size_t)37, sizeof(data) - off));
    sha256_final(&s, d);
    sha256_hex(d, two);
    CHECK_STR(one, two);
}

TEST(pattern_is_offset_addressable)
{
    pattern_init();
    uint8_t whole[4096], part[100];
    pattern_fill(PATTERN_PERIOD - 2000, whole, sizeof(whole));
    pattern_fill(PATTERN_PERIOD - 2000 + 1950, part, sizeof(part));
    CHECK(!memcmp(whole + 1950, part, sizeof(part)));
}

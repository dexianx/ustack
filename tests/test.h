#pragma once

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct test_case {
    const char *name;
    void (*fn)(void);
    struct test_case *next;
};

void test_register(struct test_case *t);
extern int test_failures;

#define TEST(name)                                                         \
    static void test_##name(void);                                         \
    static struct test_case test_case_##name = { #name, test_##name, 0 };  \
    __attribute__((constructor)) static void test_reg_##name(void)         \
    {                                                                      \
        test_register(&test_case_##name);                                  \
    }                                                                      \
    static void test_##name(void)

#define FAIL_AT(...)                                                       \
    do {                                                                   \
        fprintf(stderr, "    %s:%d: ", __FILE__, __LINE__);                \
        fprintf(stderr, __VA_ARGS__);                                      \
        fputc('\n', stderr);                                               \
        test_failures++;                                                   \
    } while (0)

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond))                                                       \
            FAIL_AT("CHECK(%s)", #cond);                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        long long va_ = (long long)(a), vb_ = (long long)(b);              \
        if (va_ != vb_)                                                    \
            FAIL_AT("%s == %s (%lld vs %lld)", #a, #b, va_, vb_);          \
    } while (0)

#define CHECK_STR(a, b)                                                    \
    do {                                                                   \
        if (strcmp((a), (b)))                                              \
            FAIL_AT("%s == \"%s\" (got \"%s\")", #a, (b), (a));            \
    } while (0)

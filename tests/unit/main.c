#include <stdio.h>
#include <string.h>

#include "test.h"

static struct test_case *head;
static struct test_case **tail = &head;
int test_failures;

void test_register(struct test_case *t)
{
    *tail = t;
    tail = &t->next;
}

int main(int argc, char **argv)
{
    int run = 0, failed = 0;
    for (struct test_case *t = head; t; t = t->next) {
        if (argc > 1 && !strstr(t->name, argv[1]))
            continue;
        int before = test_failures;
        t->fn();
        run++;
        if (test_failures != before) {
            failed++;
            fprintf(stderr, "FAIL %s\n", t->name);
        }
    }
    printf("unit: %d/%d tests passed\n", run - failed, run);
    return failed ? 1 : 0;
}

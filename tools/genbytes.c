#include <stdio.h>
#include <stdlib.h>

#include "core/pattern.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s BYTES\n", argv[0]);
        return 2;
    }
    unsigned long long total = strtoull(argv[1], NULL, 10);
    static unsigned char buf[1 << 16];
    pattern_init();
    for (unsigned long long off = 0; off < total;) {
        size_t n = total - off < sizeof(buf) ? (size_t)(total - off) : sizeof(buf);
        pattern_fill(off, buf, n);
        if (fwrite(buf, 1, n, stdout) != n)
            return 1;
        off += n;
    }
    return 0;
}

#include "rle.h"

#include <string.h>

#define MAX_LITERAL 128
#define MAX_REPEAT 129

static size_t repeats(const uint8_t *src, size_t pixels, size_t at)
{
    size_t n = 1;
    while (at + n < pixels && n < MAX_REPEAT && memcmp(src + 2 * at, src + 2 * (at + n), 2) == 0) n++;
    return n;
}

size_t rle_encode(const uint8_t *src, size_t pixels, uint8_t *out)
{
    size_t used = 0, i = 0;
    while (i < pixels) {
        size_t run = repeats(src, pixels, i);
        if (run >= 2) {
            out[used++] = (uint8_t)(126 + run);
            out[used++] = src[2 * i];
            out[used++] = src[2 * i + 1];
            i += run;
            continue;
        }
        size_t start = i; /* different pixels until the next repeat of at least two */
        while (i < pixels && i - start < MAX_LITERAL && (i + 1 >= pixels || repeats(src, pixels, i) < 2)) i++;
        out[used++] = (uint8_t)(i - start - 1);
        memcpy(out + used, src + 2 * start, 2 * (i - start));
        used += 2 * (i - start);
    }
    return used;
}

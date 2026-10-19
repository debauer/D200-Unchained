// Run-length coding on the PC: a decoder with the rules of device/src/rle.rs checks the encoder's output.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rle.h"

static size_t decode(const uint8_t *src, size_t n, uint8_t *out, size_t cap) {
    size_t i = 0, used = 0;
    while (i < n) {
        int c = src[i++];
        size_t count = c < 128 ? (size_t)c + 1 : (size_t)c - 126;
        assert(used + 2 * count <= cap);
        if (c < 128) { memcpy(out + used, src + i, 2 * count); i += 2 * count; }
        else { for (size_t k = 0; k < count; k++) memcpy(out + used + 2 * k, src + i, 2); i += 2; }
        used += 2 * count;
        assert(i <= n);
    }
    return used;
}
static size_t round_trip(const uint8_t *px, size_t pixels) {
    uint8_t *coded = malloc(RLE_MAX_SIZE(pixels)), *back = malloc(pixels * 2);
    size_t n = rle_encode(px, pixels, coded);
    assert(n <= RLE_MAX_SIZE(pixels));
    assert(decode(coded, n, back, pixels * 2) == pixels * 2 && memcmp(px, back, pixels * 2) == 0);
    free(coded); free(back);
    return n;
}
int main(void) {
    uint8_t a[2000];
    memset(a, 0x7A, sizeof a);
    assert(round_trip(a, 1000) == 8 * 3);              /* 1000 = 7 * 129 + 97: eight runs */
    for (size_t i = 0; i < 400; i++) { a[2 * i] = i; a[2 * i + 1] = i >> 8; }
    assert(round_trip(a, 400) == 400 * 2 + 4);         /* all different: one control byte per 128 pixels */
    round_trip(a, 1);
    round_trip(a, 2);
    srand(1);
    for (int round = 0; round < 500; round++) {       /* random mixtures of runs and noise */
        size_t pixels = 1 + (size_t)rand() % 1000, i = 0;
        uint8_t *px = malloc(pixels * 2);
        while (i < pixels) {
            size_t run = 1 + (size_t)rand() % 300;
            int noisy = rand() % 2;
            uint8_t lo = rand(), hi = rand() % 3;
            for (size_t k = 0; k < run && i < pixels; k++, i++) { px[2 * i] = noisy ? (uint8_t)rand() : lo; px[2 * i + 1] = noisy ? (uint8_t)(rand() % 3) : hi; }
        }
        round_trip(px, pixels);
        free(px);
    }
    puts("rle ok");
}

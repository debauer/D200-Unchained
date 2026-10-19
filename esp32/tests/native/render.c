// Renderer smoke test on the PC: the text lands where it should, the background stays, the clock draws.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"

static uint8_t *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    assert(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return b;
}

static uint16_t px(const uint8_t *img, int w, int x, int y)
{
    return (uint16_t)(img[2 * (y * w + x)] | img[2 * (y * w + x) + 1] << 8);
}

/* Number of pixels in rows [y0, y1) that differ from `bg`. */
static int inked(const uint8_t *img, int w, int y0, int y1, uint16_t bg)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < w; x++) n += px(img, w, x, y) != bg;
    return n;
}

int main(int argc, char **argv)
{
    assert(argc == 3 && render_init(slurp(argv[1]), slurp(argv[2])));
    const int w = 196, h = 196;
    uint8_t *img = malloc((size_t)w * h * 2);
    const uint16_t bg = (uint16_t)(((10 & 0xF8) << 8) | ((13 & 0xFC) << 3) | (18 >> 3));

    assert(render_key(img, w, h, (rgb_t){10, 13, 18}, NULL, "CPU 42%", (rgb_t){255, 255, 255}, "bottom", 100));
    assert(px(img, w, 0, 0) == bg && px(img, w, w - 1, h - 1) == bg); /* corners untouched */
    assert(inked(img, w, h / 2, h, bg) > 100 && inked(img, w, 0, h / 3, bg) == 0); /* text at the bottom only */

    assert(render_key(img, w, h, (rgb_t){10, 13, 18}, NULL, "top", (rgb_t){255, 255, 255}, "top", 100));
    assert(inked(img, w, 0, h / 3, bg) > 50 && inked(img, w, h / 2, h, bg) == 0);

    assert(render_key(img, w, h, (rgb_t){10, 13, 18}, NULL, "   ", (rgb_t){255, 255, 255}, "center", 100));
    assert(inked(img, w, 0, h, bg) == 0); /* blank text draws nothing */

    /* a text that does not fit three lines is shortened with "..." instead of overflowing the key */
    assert(render_key(img, w, h, (rgb_t){10, 13, 18}, NULL,
                      "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen",
                      (rgb_t){255, 255, 255}, "center", 100));
    assert(inked(img, w, 0, 20, bg) == 0 && inked(img, w, h - 20, h, bg) == 0);

    uint8_t *icon = malloc((size_t)w * h * 2);
    memset(icon, 0x55, (size_t)w * h * 2);
    assert(render_key(img, w, h, (rgb_t){0, 0, 0}, icon, "On icon", (rgb_t){255, 255, 255}, "center", 100));
    assert(px(img, w, 2, 2) == 0x5555); /* the picture stays visible around the text */

    const int cw = 456;
    uint8_t *clock = malloc((size_t)cw * h * 2);
    assert(render_clock(clock, cw, h, (rgb_t){10, 13, 18}, 14, 41, 4, 17, 10, (rgb_t){0, 240, 255}, (rgb_t){255, 43, 214}));
    assert(inked(clock, cw, 0, h, bg) > 2000);
    assert(render_clock(clock, cw, h, (rgb_t){10, 13, 18}, -1, 0, 0, 1, 1, (rgb_t){0, 240, 255}, (rgb_t){255, 43, 214}));
    puts("render ok");
    return 0;
}

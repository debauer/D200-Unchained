#pragma once

/* Text layout and glyph drawing on top of stb_truetype; no ESP-IDF dependencies so that it also builds on a PC. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "third_party/stb_truetype.h"

#define LINE_BYTES 256

typedef struct {
    stbtt_fontinfo info;
    int ascent, descent; /* font units; descent is negative */
} gfont_t;

bool gfont_init(gfont_t *f, const uint8_t *ttf);

/* Width of the UTF-8 text `s` (len bytes) at an em size of `px` pixels. */
float text_width(const gfont_t *f, float px, const char *s, size_t len);

/* Greedy word wrap into `lines` (at most `max_lines` + 1 are produced, so that the caller can tell it does not fit).
 * Words wider than a line are broken anywhere. Returns the number of lines. */
size_t wrap_text(const gfont_t *f, float px, float max_width, const char *text, char lines[][LINE_BYTES],
                 size_t max_lines);

/* Cuts `line` so that "..." fits in `max_width`. */
void ellipsize(const gfont_t *f, float px, float max_width, char *line);

/* A coverage layer the size of the key (0-255 per pixel). */
typedef struct {
    uint8_t *a;
    int w, h;
} mask_t;

/* Draws `s` into `m` with its left edge at x and the middle between ascender and descender at y (anchor "lm"),
 * `spacing` extra pixels after each character; `alpha` scales the coverage (0-255). Coverage adds up (saturating). */
void draw_text(mask_t *m, const gfont_t *f, float px, float x, float y, const char *s, size_t len, float spacing,
               int alpha);

/* Same, centred on x (anchor "mm"). */
void draw_text_centered(mask_t *m, const gfont_t *f, float px, float cx, float cy, const char *s, size_t len,
                        float spacing, int alpha);

/* Gaussian-like blur (three box passes) of the layer. */
void blur_mask(mask_t *m, float sigma);

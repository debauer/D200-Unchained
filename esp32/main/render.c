#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glyphs.h"

#define MAX_LINES 3
#define FONT_RATIO 0.12f
#define MIN_FONT_RATIO 0.075f
#define PAD_X_RATIO 0.055f
#define PAD_Y_RATIO 0.07f
#define LINE_HEIGHT 1.15f
#define CLOCK_TIME_RATIO 0.45f
#define CLOCK_DATE_RATIO 0.13f

static gfont_t s_bold, s_medium;

bool render_init(const uint8_t *bold, const uint8_t *medium)
{
    return gfont_init(&s_bold, bold) && gfont_init(&s_medium, medium);
}

/* --- RGB565 helpers --------------------------------------------------------------------------------------- */

static uint16_t pack(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void put(uint8_t *out, int i, uint16_t v)
{
    out[2 * i] = v & 0xFF;
    out[2 * i + 1] = v >> 8;
}

static void fill(uint8_t *out, int count, rgb_t c)
{
    uint16_t v = pack(c.r, c.g, c.b);
    for (int i = 0; i < count; i++) put(out, i, v);
}

/* Blends `c` over the image where the layer has coverage. */
static void composite(uint8_t *out, const mask_t *m, rgb_t c)
{
    for (int i = 0; i < m->w * m->h; i++) {
        int a = m->a[i];
        if (!a) continue;
        uint16_t v = (uint16_t)(out[2 * i] | out[2 * i + 1] << 8);
        int r = (v >> 8) & 0xF8, g = (v >> 3) & 0xFC, b = (v << 3) & 0xF8;
        r |= r >> 5;
        g |= g >> 6;
        b |= b >> 5;
        put(out, i, pack((r * (255 - a) + c.r * a) / 255, (g * (255 - a) + c.g * a) / 255, (b * (255 - a) + c.b * a) / 255));
    }
}

static bool new_mask(mask_t *m, int w, int h)
{
    m->w = w;
    m->h = h;
    m->a = calloc((size_t)w * h, 1);
    return m->a != NULL;
}

/* --- keys with text --------------------------------------------------------------------------------------- */

/* Largest font size that fits MAX_LINES; at the minimum size the last line is cut (layout() in textrender.py). */
static size_t layout(const char *text, float width, int height, float scale, char lines[][LINE_BYTES], float *px)
{
    int size = (int)lroundf(height * FONT_RATIO * scale);
    if (size < 1) size = 1;
    int minimum = (int)lroundf(height * MIN_FONT_RATIO);
    if (minimum > size) minimum = size;
    for (;;) {
        size_t n = wrap_text(&s_bold, (float)size, width, text, lines, MAX_LINES);
        if (n <= MAX_LINES || size <= minimum) {
            *px = (float)size;
            if (n > MAX_LINES) {
                n = MAX_LINES;
                ellipsize(&s_bold, *px, width, lines[MAX_LINES - 1]);
            }
            return n;
        }
        size--;
    }
}

bool render_key(uint8_t *out, int w, int h, rgb_t color, const uint8_t *icon, const char *text, rgb_t text_color,
                const char *position, int size)
{
    if (icon) memcpy(out, icon, (size_t)w * h * 2);
    else fill(out, w * h, color);
    while (*text == ' ' || *text == '\n' || *text == '\t') text++;
    if (!*text) return true;
    char (*lines)[LINE_BYTES] = malloc((size_t)(MAX_LINES + 1) * LINE_BYTES);
    if (!lines) return false;
    float px;
    int pad_x = (int)lroundf(h * PAD_X_RATIO), pad_y = (int)lroundf(h * PAD_Y_RATIO);
    size_t n = layout(text, (float)(w - 2 * pad_x), h, size / 100.0f, lines, &px);
    float line_h = px * LINE_HEIGHT, total = line_h * n;
    float top = strcmp(position, "top") == 0 ? pad_y : strcmp(position, "center") == 0 ? (h - total) / 2 : h - pad_y - total;
    mask_t crisp, shadow = {0};
    bool ok = new_mask(&crisp, w, h) && (!icon || new_mask(&shadow, w, h));
    for (size_t i = 0; ok && i < n; i++) {
        float cy = top + line_h * (i + 0.5f);
        if (icon) { /* the shadow keeps text readable over pictures */
            int dy = (int)lroundf(h * 0.01f);
            draw_text_centered(&shadow, &s_bold, px, w / 2.0f, cy + (dy < 1 ? 1 : dy), lines[i], strlen(lines[i]), 0, 204);
        }
        draw_text_centered(&crisp, &s_bold, px, w / 2.0f, cy, lines[i], strlen(lines[i]), 0, 255);
    }
    if (ok && icon) {
        int radius = (int)lroundf(h * 0.03f);
        blur_mask(&shadow, (radius < 1 ? 1 : radius) / 2.0f);
        composite(out, &shadow, (rgb_t){0, 0, 0});
    }
    if (ok) composite(out, &crisp, text_color);
    free(crisp.a);
    free(shadow.a);
    free(lines);
    return ok;
}

/* --- clock --------------------------------------------------------------------------------------------------- */

bool render_clock(uint8_t *out, int w, int h, rgb_t color, int hour, int minute, int weekday, int day, int month,
                  rgb_t time_color, rgb_t date_color)
{
    static const char *const days[] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
    char time_text[8], date_text[16];
    if (hour < 0) { /* the clock is not set yet */
        snprintf(time_text, sizeof time_text, "--:--");
        snprintf(date_text, sizeof date_text, "---");
    } else {
        snprintf(time_text, sizeof time_text, "%02d:%02d", hour % 24, minute % 60);
        snprintf(date_text, sizeof date_text, "%s %02d.%02d.", days[(weekday % 7 + 7) % 7], day % 100, month % 100);
    }
    fill(out, w * h, color);
    float time_px = (float)lroundf(h * CLOCK_TIME_RATIO), date_px = (float)lroundf(h * CLOCK_DATE_RATIO);
    float gap = h * 0.03f, block = time_px + gap + date_px;
    float time_y = (h - block) / 2 + time_px / 2;
    float date_y = time_y + time_px / 2 + gap + date_px / 2;
    mask_t m; /* one layer, cleared between the three drawing steps */
    bool ok = new_mask(&m, w, h);
    if (ok) {
        const size_t bytes = (size_t)w * h;
        draw_text_centered(&m, &s_bold, time_px, w / 2.0f, time_y, time_text, strlen(time_text), time_px * 0.08f, 200);
        blur_mask(&m, h * 0.03f);
        composite(out, &m, time_color); /* glow */
        memset(m.a, 0, bytes);
        draw_text_centered(&m, &s_bold, time_px, w / 2.0f, time_y, time_text, strlen(time_text), time_px * 0.08f, 255);
        composite(out, &m, time_color);
        memset(m.a, 0, bytes);
        draw_text_centered(&m, &s_medium, date_px, w / 2.0f, date_y, date_text, strlen(date_text), date_px * 0.2f, 255);
        composite(out, &m, date_color);
    }
    free(m.a);
    return ok;
}

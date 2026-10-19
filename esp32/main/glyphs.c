#include "glyphs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

bool gfont_init(gfont_t *f, const uint8_t *ttf)
{
    int line_gap;
    if (!stbtt_InitFont(&f->info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) return false;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &line_gap);
    return true;
}

/* Next code point of UTF-8 text; returns the number of bytes used (1 for invalid input). */
static size_t next_cp(const char *s, size_t len, int *cp)
{
    unsigned char c = (unsigned char)s[0];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (n > len) n = 1;
    if (n == 1) {
        *cp = c < 0x80 ? c : 0xFFFD;
        return 1;
    }
    int v = c & (0xFF >> (n + 1));
    for (size_t i = 1; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | ((unsigned char)s[i] & 0x3F);
    }
    *cp = v;
    return n;
}

static float advance(const gfont_t *f, float scale, int cp)
{
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
    return adv * scale;
}

float text_width(const gfont_t *f, float px, const char *s, size_t len)
{
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, px), w = 0;
    for (size_t i = 0; i < len;) {
        int cp;
        i += next_cp(s + i, len - i, &cp);
        w += advance(f, scale, cp);
    }
    return w;
}

/* --- wrapping (mirrors wrap() in render/textrender.py) ------------------------------------------------------ */

typedef struct {
    char (*lines)[LINE_BYTES];
    size_t count, max;
    char current[LINE_BYTES];
    size_t len;
} wrapper_t;

static bool push_line(wrapper_t *w)
{
    if (w->count > w->max) return false; /* more than the caller wants to know about */
    memcpy(w->lines[w->count], w->current, w->len);
    w->lines[w->count][w->len] = 0;
    w->count++;
    w->len = 0;
    return true;
}

static bool append(wrapper_t *w, const char *s, size_t n)
{
    if (w->len + n >= LINE_BYTES) return false;
    memcpy(w->current + w->len, s, n);
    w->len += n;
    return true;
}

size_t wrap_text(const gfont_t *f, float px, float max_width, const char *text, char lines[][LINE_BYTES],
                 size_t max_lines)
{
    wrapper_t w = {.lines = lines, .max = max_lines};
    const char *p = text;
    bool more = true;
    while (more) { /* one paragraph per round */
        const char *end = strchr(p, '\n');
        size_t plen = end ? (size_t)(end - p) : strlen(p);
        more = end != NULL;
        size_t i = 0;
        w.len = 0;
        while (i < plen) {
            while (i < plen && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r')) i++;
            size_t start = i;
            while (i < plen && !(p[i] == ' ' || p[i] == '\t' || p[i] == '\r')) i++;
            if (i == start) break;
            const char *word = p + start;
            size_t wlen = i - start;
            size_t before = w.len;
            if ((w.len ? append(&w, " ", 1) : true) && append(&w, word, wlen) &&
                text_width(f, px, w.current, w.len) <= max_width) {
                continue; /* the word fits on this line */
            }
            w.len = before;
            if (w.len && !push_line(&w)) return w.count;
            for (size_t k = 0; k < wlen;) { /* break the word anywhere */
                int cp;
                size_t n = next_cp(word + k, wlen - k, &cp);
                if (w.len && text_width(f, px, w.current, w.len) + advance(f, stbtt_ScaleForMappingEmToPixels(&f->info, px), cp) > max_width) {
                    if (!push_line(&w)) return w.count;
                }
                if (!append(&w, word + k, n)) return w.count;
                k += n;
            }
        }
        if (w.len && !push_line(&w)) return w.count;
        p += plen + 1;
    }
    return w.count;
}

void ellipsize(const gfont_t *f, float px, float max_width, char *line)
{
    size_t len = strlen(line);
    char probe[LINE_BYTES + 4];
    for (;;) {
        memcpy(probe, line, len);
        memcpy(probe + len, "...", 4);
        if (len == 0 || text_width(f, px, probe, len + 3) <= max_width) break;
        len--;
        while (len && ((unsigned char)line[len] & 0xC0) == 0x80) len--; /* not in the middle of a character */
        while (len && line[len - 1] == ' ') len--;
    }
    memcpy(line, probe, len + 4);
}

/* --- drawing -------------------------------------------------------------------------------------------- */

void draw_text(mask_t *m, const gfont_t *f, float px, float x, float y, const char *s, size_t len, float spacing,
               int alpha)
{
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, px);
    float baseline = y + (f->ascent + f->descent) * scale / 2; /* anchor "m": halfway between ascender and descender */
    int cap = (int)(px * 2) + 4;
    uint8_t *tmp = malloc((size_t)cap * cap);
    if (!tmp) return;
    for (size_t i = 0; i < len;) {
        int cp;
        i += next_cp(s + i, len - i, &cp);
        int x0, y0, x1, y1;
        float fx = x - floorf(x);
        stbtt_GetCodepointBitmapBoxSubpixel(&f->info, cp, scale, scale, fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= cap && gh <= cap) {
            memset(tmp, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&f->info, tmp, gw, gh, gw, scale, scale, fx, 0, cp);
            int ox = (int)floorf(x) + x0, oy = (int)floorf(baseline) + y0;
            for (int row = 0; row < gh; row++) {
                int py = oy + row;
                if (py < 0 || py >= m->h) continue;
                for (int col = 0; col < gw; col++) {
                    int pxx = ox + col;
                    if (pxx < 0 || pxx >= m->w) continue;
                    int v = tmp[row * gw + col] * alpha / 255;
                    int sum = m->a[py * m->w + pxx] + v;
                    m->a[py * m->w + pxx] = sum > 255 ? 255 : (uint8_t)sum;
                }
            }
        }
        x += advance(f, scale, cp) + spacing;
    }
    free(tmp);
}

void draw_text_centered(mask_t *m, const gfont_t *f, float px, float cx, float cy, const char *s, size_t len,
                        float spacing, int alpha)
{
    float chars = 0;
    for (size_t i = 0; i < len;) {
        int cp;
        i += next_cp(s + i, len - i, &cp);
        chars++;
    }
    float width = text_width(f, px, s, len) + (chars > 1 ? spacing * (chars - 1) : 0);
    draw_text(m, f, px, cx - width / 2, cy, s, len, spacing, alpha);
}

/* --- blur ----------------------------------------------------------------------------------------------- */

static void box_pass(uint8_t *line, uint8_t *tmp, int n, int stride, int r)
{
    int window = 2 * r + 1, sum = 0;
    for (int i = -r; i <= r; i++) sum += (i >= 0 && i < n) ? line[i * stride] : 0;
    for (int i = 0; i < n; i++) {
        tmp[i] = (uint8_t)(sum / window);
        int out = i - r, in = i + r + 1;
        sum += (in < n ? line[in * stride] : 0) - (out >= 0 ? line[out * stride] : 0);
    }
    for (int i = 0; i < n; i++) line[i * stride] = tmp[i];
}

void blur_mask(mask_t *m, float sigma)
{
    if (sigma < 0.5f) return;
    int r = (int)(sqrtf(12 * sigma * sigma / 3 + 1) - 1) / 2; /* three box passes approximate a Gaussian */
    if (r < 1) r = 1;
    int longest = m->w > m->h ? m->w : m->h;
    uint8_t *tmp = malloc((size_t)longest);
    if (!tmp) return;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < m->h; y++) box_pass(m->a + y * m->w, tmp, m->w, 1, r);
        for (int x = 0; x < m->w; x++) box_pass(m->a + x, tmp, m->h, m->w, r);
    }
    free(tmp);
}

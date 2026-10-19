#pragma once

/* Key images drawn on the ESP32: keys whose text changes (placeholders) and the clock in the info window. Mirrors
 * host/src/d200_deck/render/textrender.py. No ESP-IDF dependencies. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} rgb_t;

/* The two fonts (TrueType data, must stay valid): bold for text and time, medium for the date. */
bool render_init(const uint8_t *bold, const uint8_t *medium);

/* RGB565 (little endian, w*h*2 bytes) into `out`: `color` or the RGB565 `icon` (w*h*2 bytes, may be NULL) as the
 * background, then `text` at "top", "center" or "bottom" (`size` in percent of the automatic size). Over an icon the
 * text gets a shadow. False when out of memory. */
bool render_key(uint8_t *out, int w, int h, rgb_t color, const uint8_t *icon, const char *text, rgb_t text_color,
                const char *position, int size);

/* The info-window clock: time with glow in `time_color`, date in `date_color`. `weekday` 0 = Monday; a negative
 * `hour` draws "--:--" (not set yet). */
bool render_clock(uint8_t *out, int w, int h, rgb_t color, int hour, int minute, int weekday, int day, int month,
                  rgb_t time_color, rgb_t date_color);

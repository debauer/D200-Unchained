#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adb.h"
#include "esp_err.h"

#define DEVICE_MAX_KEYS 16

typedef struct {
    uint16_t index, code, x, y, width, height;
} device_key_t;

typedef struct {
    bool connected;
    uint16_t screen_width, screen_height;
    uint8_t key_count;
    device_key_t keys[DEVICE_MAX_KEYS];
    int brightness; /* 0-255, applies while the display is on */
    bool display_on;
    char page[33];
} device_state_t;

void device_init(void);
void device_snapshot(device_state_t *out);

/* The window of key `index`; false for an unknown key. */
bool device_key_window(unsigned index, device_key_t *out);

/* Backlight state (level 0-255 while on); the device task sends it to deckd, subscribers get a `brightness` event. */
void device_set_backlight(int level, bool display_on);

/* Shows page `id` (it must exist): full redraw and a `page` event. */
void device_set_page(const char *id);

/* JSON for GET /api/info (needs `size` bytes, truncated otherwise); returns the length. */
size_t device_info_json(char *out, size_t size);

/* Body of the SSE event `name` ("status", "brightness", "page", "values") in its current state. */
void device_event_json(const char *name, char *out, size_t size);

/* Something changed: the device task redraws the key (or everything) at its next turn. */
void device_request_redraw(int key); /* key < 0: everything */

/* Runs against a bootstrapped D200: hello, initial brightness, then forwards key presses as events until the
 * link breaks. Returns when it does. */
esp_err_t device_run(adb_t *a);

#pragma once

#include "cJSON.h"
#include "deckd.h"
#include "render.h"
#include "device.h"

/* Draws the keys in the bit set `keys` of `state->page` from the configuration (first clearing the whole screen
 * when `clear`): the stored image of a key, else a
 * fill with its colour (key, else page, else `background`). Text, placeholders and the clock follow with the
 * renderer. */
/* Asks for a redraw of what differs between two configurations on `page`: the whole page when something shared
 * changed (background, the page's colour, the page missing), else only the keys whose look changed. */
void draw_request_changes(const cJSON *before, const cJSON *after, const char *page);

/* A colour setting of a key: the key's own, else the page's, else (for "color") the configuration's `background`,
 * else `fallback`. */
rgb_t draw_color(const cJSON *config, const cJSON *page, const cJSON *key, const char *field, rgb_t fallback);

esp_err_t draw_page(deckd_t *d, const device_state_t *state, uint32_t keys, bool clear);

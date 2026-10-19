#pragma once

#include "deckd.h"
#include "device.h"

/* Draws the keys in the bit set `keys` of `state->page` from the configuration (first clearing the whole screen
 * when `clear`): the stored image of a key, else a
 * fill with its colour (key, else page, else `background`). Text, placeholders and the clock follow with the
 * renderer. */
esp_err_t draw_page(deckd_t *d, const device_state_t *state, uint32_t keys, bool clear);

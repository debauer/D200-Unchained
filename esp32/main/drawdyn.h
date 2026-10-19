#pragma once

#include "deckd.h"
#include "device.h"
#include "cJSON.h"

/* Keys the firmware draws itself because they change: text with {placeholders}, the info window's clock while
 * it has neither an image nor text, and keys whose colour rule matches (host/src/d200_deck/render/dynamic.py). */

bool drawdyn_is_dynamic(const cJSON *config, const cJSON *key, const char *page_id, const device_key_t *window); /* key may be NULL */

esp_err_t drawdyn_key(deckd_t *d, const cJSON *config, const cJSON *page, const char *page_id, const cJSON *key,
                      const device_key_t *window);

/* Asks the device task to redraw the dynamic keys of the shown page and those with colour rules (every minute, when
 * the clock gets set, when a value changes). */
void drawdyn_request_all(void);

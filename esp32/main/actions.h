#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Key actions the ESP32 host can run: backlight/display and page navigation (docs/web-api.md, `actions`). */

/* Runs the action of key `index` on `page` (a key press on the device, or POST .../press in the web UI's TEST
 * mode). Reports it as an `action` event. False if the key has no action or its type is not supported. */
bool actions_run(unsigned index, const char *page);

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Key actions the ESP32 host can run: backlight/display, page navigation and MQTT messages (docs/web-api.md, `actions`). */

/* Runs the action of key `index` on `page` (a key press on the device, or POST .../press in the web UI's TEST
 * mode, with `test`: an `mqtt` message is only reported, never sent). Reports it as an `action` event. False if the key
 * has no action, its type is not supported or nothing was done. */
bool actions_run(unsigned index, const char *page, bool test);

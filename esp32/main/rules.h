#pragma once

#include "cJSON.h"

/* Colour rules of a key (docs/mqtt.md): {"variable": "light", "value": "ON", "color": "ffcc00", "label_color": "000000"}. */

/* The first rule of `key` whose variable currently has its value (compared after trimming spaces), or NULL.
 * `variables` is the configuration's variable list; a variable without a value yet never matches. */
const cJSON *rules_match(const cJSON *variables, const cJSON *key);

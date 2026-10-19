#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

/* `{name}` placeholders in key text (host/src/d200_deck/config/placeholders.py): `{{` and `}}` are literal braces. */

bool placeholders_present(const char *text);

/* Replaces the placeholders: {time} and {date} from the clock ("--:--" and "---" until it is synced), names of
 * the configuration's `variables` by "?" (this host has no source for their type), unknown names stay as written. */
void placeholders_fill(const char *text, const cJSON *variables, char *out, size_t size);

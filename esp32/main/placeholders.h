#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

/* `{name}` placeholders in key text (host/src/d200_deck/config/placeholders.py): `{{` and `}}` are literal braces. */

bool placeholders_present(const char *text);

typedef enum { PLACEHOLDER_UNKNOWN, PLACEHOLDER_VALUE, PLACEHOLDER_PENDING } placeholder_t;

/* The value of {name}: time and date from the clock ("--:--" and "---" until it is synced), a variable of type
 * mqtt from its latest message ("..." and PLACEHOLDER_PENDING before the first one), any other variable "?"
 * (this host has no source for its type). PLACEHOLDER_UNKNOWN: not a placeholder name. */
placeholder_t placeholders_value(const cJSON *variables, const char *name, char *out, size_t size);

/* The `values` object of the API (docs/web-api.md) for the configuration's `variables`, as JSON text; free() it. */
char *placeholders_values_json(const cJSON *variables);

/* Replaces the placeholders in `text` by their values; unknown names stay as written. */
void placeholders_fill(const char *text, const cJSON *variables, char *out, size_t size);

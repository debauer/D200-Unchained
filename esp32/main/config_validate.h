#pragma once

#include "cJSON.h"

/* Validates and normalizes v1/v2 JSON. On error returns NULL and sets a static error message. */
cJSON *config_validate(const char *text, const char **error);

/* Field validation shared by the page/tree validator. Unknown action/variable types are retained. */
const char *config_validate_variables(cJSON *config);
const char *config_validate_keys(cJSON *config, cJSON *page);
const char *config_validate_colors(const cJSON *object);

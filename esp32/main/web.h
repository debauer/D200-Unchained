#pragma once

#include "esp_err.h"

/* HTTP server: the shared web UI from the `www` partition plus the web API (docs/web-api.md). */
esp_err_t web_start(void);

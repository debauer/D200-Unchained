#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* HTTP server: the shared web UI from the `www` partition plus the web API (docs/web-api.md). */
esp_err_t web_start(void);

/* {"error": message} with the given status line, e.g. "404 Not Found". */
esp_err_t web_json_error(httpd_req_t *req, const char *status, const char *message);

/* Changing requests must carry the header a foreign origin cannot set without a preflight. */
bool web_mutation_allowed(httpd_req_t *req);

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
bool web_host_allowed(httpd_req_t *req);
bool web_origin_allowed(httpd_req_t *req);
esp_err_t web_guarded_request(httpd_req_t *req);
int web_receive_body(httpd_req_t *req, char *body, size_t size);
esp_err_t web_config_get(httpd_req_t *req);
esp_err_t web_config_put(httpd_req_t *req);

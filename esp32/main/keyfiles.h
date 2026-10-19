#pragma once

#include "esp_http_server.h"

/* /api/keys/<n>/{image,icon,alticon} and /api/pages/<page>/keys/<n>/... (docs/web-api.md). */
esp_err_t keyfiles_handler(httpd_req_t *req);

/* Makes sure the image directory exists. */
void keyfiles_init(void);

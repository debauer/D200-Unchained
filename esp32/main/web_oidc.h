#pragma once

#include "oidc.h"
#include "esp_http_server.h"

esp_err_t web_oidc_init(void);
esp_err_t web_oidc_start(httpd_req_t *req);
esp_err_t web_oidc_callback(httpd_req_t *req);

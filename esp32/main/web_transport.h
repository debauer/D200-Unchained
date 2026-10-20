#pragma once

#include "esp_http_server.h"

esp_err_t web_transport_start(httpd_config_t *config, httpd_handle_t *server);
/* Preserve HTTPS's send override; bound stream writes without bypassing TLS. */
esp_err_t web_stream_prepare(httpd_req_t *req);

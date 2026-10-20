#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
typedef struct test_http_client *esp_http_client_handle_t;
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef struct {
    const char *url, *cert_pem;
    size_t cert_len;
    esp_http_client_method_t method;
    int timeout_ms, max_authorization_retries, buffer_size, buffer_size_tx;
    bool disable_auto_redirect, skip_cert_common_name_check;
    esp_err_t (*crt_bundle_attach)(void *config);
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int ms);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length);
int esp_http_client_write(esp_http_client_handle_t client, const char *data, int length);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *out, int size);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

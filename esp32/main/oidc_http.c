#include "oidc.h"

#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "mbedtls/platform_util.h"
#include "web_settings.h"

static bool remaining(esp_http_client_handle_t client, int64_t deadline)
{
    int64_t ms = (deadline - esp_timer_get_time()) / 1000;
    return ms > 0 && esp_http_client_set_timeout_ms(client, ms < 5000 ? (int)ms : 5000) == ESP_OK;
}

cJSON *oidc_http(const char *url, const char *form, size_t limit)
{
    extern const uint8_t oidc_ca_start[], oidc_ca_end[];
    bool ca = oidc_ca_end - oidc_ca_start != 0;
    const esp_http_client_config_t cfg = {
        .url = url, .method = form ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 5000, .disable_auto_redirect = true, .max_authorization_retries = -1,
        .cert_pem = ca ? (const char *)oidc_ca_start : NULL,
        .cert_len = ca ? (size_t)(oidc_ca_end - oidc_ca_start + 1) : 0,
        .crt_bundle_attach = ca ? NULL : esp_crt_bundle_attach,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    char *body = malloc(limit + 1);
    cJSON *reply = NULL;
    if (!client || !body) goto done;
    int64_t deadline = esp_timer_get_time() + 15000000;
    if (esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) goto done;
    if (form && esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded") != ESP_OK) goto done;
    size_t length = form ? strlen(form) : 0, sent = 0;
    if (esp_http_client_open(client, (int)length) != ESP_OK) goto done;
    while (sent < length && remaining(client, deadline)) {
        int n = esp_http_client_write(client, form + sent, (int)(length - sent));
        if (n <= 0) goto done;
        sent += n;
    }
    if (sent != length || !remaining(client, deadline)) goto done;
    int64_t announced = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 || announced < 0 || announced > (int64_t)limit) goto done;
    size_t used = 0;
    while (remaining(client, deadline)) {
        int n = esp_http_client_read(client, body + used, (int)(limit + 1 - used < 2048 ? limit + 1 - used : 2048));
        if (n < 0) goto done;
        if (!n) break;
        used += n;
        if (used > limit) goto done;
    }
    if (esp_timer_get_time() < deadline && esp_http_client_is_complete_data_received(client)) reply = oidc_json(body, used);
done:
    if (body) { mbedtls_platform_zeroize(body, limit + 1); free(body); }
    if (client) esp_http_client_cleanup(client);
    return reply;
}

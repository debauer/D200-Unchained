/* Exercise the real bounded adapter; the SDK socket/TLS API is mocked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oidc.h"
#include "esp_http_client.h"

#if TEST_CUSTOM_CA
__asm__(".pushsection .rodata\n.global oidc_ca_start\noidc_ca_start:\n.ascii \"test CA\"\n.global oidc_ca_end\noidc_ca_end:\n.byte 0\n.popsection\n");
#else
__asm__(".pushsection .rodata\n.global oidc_ca_start\noidc_ca_start:\n.global oidc_ca_end\noidc_ca_end:\n.byte 0\n.popsection\n");
#endif
static const char *response;
static size_t response_size, received, sent;
static int status = 200, read_delay;
static bool incomplete, fail_read;
static int64_t now, announced;
static unsigned cleaned, opened;
static struct test_http_client { esp_http_client_method_t method; } handle;
int64_t esp_timer_get_time(void) { return now; }
esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(config->disable_auto_redirect && !config->skip_cert_common_name_check && config->max_authorization_retries == -1);
    assert(config->timeout_ms == 5000);
    if (TEST_CUSTOM_CA) assert(config->cert_pem && config->cert_len > 0 && !config->crt_bundle_attach);
    else assert(!config->cert_pem && config->crt_bundle_attach == esp_crt_bundle_attach);
    handle.method = config->method;
    received = sent = 0;
    return &handle;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int ms) { (void)client; assert(ms > 0 && ms <= 5000); return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value)
{
    (void)client;
    assert((!strcmp(name, "Accept") && !strcmp(value, "application/json")) ||
           (!strcmp(name, "Content-Type") && !strcmp(value, "application/x-www-form-urlencoded")));
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length)
{
    assert((length > 0) == (client->method == HTTP_METHOD_POST));
    opened++;
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t client, const char *data, int length)
{
    (void)client; (void)data;
    int n = length < 3 ? length : 3;
    sent += n;
    return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client) { (void)client; return announced; }
int esp_http_client_get_status_code(esp_http_client_handle_t client) { (void)client; return status; }
int esp_http_client_read(esp_http_client_handle_t client, char *out, int size)
{
    (void)client;
    assert(size > 0);
    now += read_delay;
    if (fail_read) return -1;
    size_t n = response_size - received;
    if (n > 7) n = 7;
    if (n > (size_t)size) n = size;
    memcpy(out, response + received, n);
    received += n;
    return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) { (void)client; return !incomplete && received == response_size; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { (void)client; cleaned++; return ESP_OK; }

static cJSON *call(const char *data, int64_t length, const char *form)
{
    response = data;
    response_size = strlen(data);
    announced = length;
    unsigned before = cleaned;
    cJSON *reply = oidc_http("https://id.example/endpoint", form, 128);
    assert(cleaned == before + 1);
    return reply;
}
int main(void)
{
    cJSON *reply = call("{\"ok\":true}", 11, "code=secret&code_verifier=secret");
    assert(reply && sent == strlen("code=secret&code_verifier=secret"));
    cJSON_Delete(reply);
    reply = call("{\"ok\":true}", 0, NULL); /* chunked/unknown content length */
    assert(reply); cJSON_Delete(reply);
    status = 302;
    unsigned attempts = opened;
    assert(!call("{}", 2, NULL) && opened == attempts + 1); /* no redirect */
    status = 200;
    assert(!call("{\"duplicate\":1,\"duplicate\":2}", 0, NULL));
    assert(!call("{}", 1000, NULL));
    char huge[256]; memset(huge, 'x', sizeof huge - 1); huge[sizeof huge - 1] = 0;
    assert(!call(huge, 0, NULL) && received == 129);
    incomplete = true;
    assert(!call("{}", 0, NULL));
    incomplete = false;
    fail_read = true;
    assert(!call("{}", 0, NULL));
    fail_read = false;
    read_delay = 16000000;
    assert(!call("{}", 0, NULL));
    puts(TEST_CUSTOM_CA ? "OIDC bounded HTTP adapter/custom CA ok" : "OIDC bounded HTTP adapter/CA bundle ok");
}

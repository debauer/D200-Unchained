#include "web.h"
#include "web_transport.h"
#include "web_auth.h"
#include "web_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "config.h"
#include "device.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "events.h"
#include "keyfiles.h"
#include "logbuf.h"
#include "mqtt.h"

static const char *TAG = "web";
#define WWW "/www"
#define LOG_CHUNK 6144
#define INFO_MAX 6144

static const struct { const char *ext, *type; } TYPES[] = {
    {".html", "text/html; charset=utf-8"},      {".js", "text/javascript; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},        {".svg", "image/svg+xml"},
    {".woff2", "font/woff2"},                   {".txt", "text/plain; charset=utf-8"},
};

static const char *type_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    for (size_t i = 0; dot && i < sizeof TYPES / sizeof TYPES[0]; i++) {
        if (strcmp(dot, TYPES[i].ext) == 0) return TYPES[i].type;
    }
    return "application/octet-stream";
}

esp_err_t web_json_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[160];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* --- protection (docs/web-api.md): changing requests need the header a foreign origin cannot set ---------- */

bool web_mutation_allowed(httpd_req_t *req)
{
    char value[4];
    return httpd_req_get_hdr_value_str(req, "X-Deck-Request", value, sizeof value) == ESP_OK && strcmp(value, "1") == 0;
}

#define REQUIRE_HEADER(req) \
    do { if (!web_mutation_allowed(req)) return web_json_error(req, "403 Forbidden", "missing X-Deck-Request header"); } while (0)

/* --- static files --------------------------------------------------------------------------------------- */

static esp_err_t send_file(httpd_req_t *req)
{
    char path[sizeof WWW + 64];
    const char *uri = strcmp(req->uri, "/") == 0 ? "/index.html" : req->uri;
    if (strlen(uri) >= 64 || strstr(uri, "..")) return web_json_error(req, "404 Not Found", "not found");
    strlcpy(path, WWW, sizeof path);
    strlcat(path, uri, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return web_json_error(req, "404 Not Found", "not found");
    httpd_resp_set_type(req, type_of(path));
    char buf[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, sizeof buf, f)) > 0) err = httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* --- API -------------------------------------------------------------------------------------------------- */

static esp_err_t api_info(httpd_req_t *req)
{
    size_t size = INFO_MAX;
    char *body = NULL;
    for (;;) {
        char *next = realloc(body, size);
        if (!next) {
            free(body);
            return web_json_error(req, "500 Internal Server Error", "out of memory");
        }
        body = next;
        size_t needed = device_info_json(body, size);
        if (needed == SIZE_MAX) {
            free(body);
            return web_json_error(req, "500 Internal Server Error", "cannot load device info");
        }
        if (needed < size) break;
        size = needed + 1;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

/* POST /api/page {"page": "<id>"}: show a page on the device. */
static esp_err_t api_page(httpd_req_t *req)
{
    REQUIRE_HEADER(req);
    char body[96];
    if (!req->content_len || req->content_len >= sizeof body) return web_json_error(req, "400 Bad Request", "invalid page body size");
    int n = web_receive_body(req, body, sizeof body);
    if (n <= 0) return ESP_FAIL;
    cJSON *json = cJSON_Parse(body);
    const cJSON *id = json ? cJSON_GetObjectItem(json, "page") : NULL;
    if (!cJSON_IsString(id)) {
        cJSON_Delete(json);
        return web_json_error(req, "400 Bad Request", "expected {\"page\": \"<id>\"}");
    }
    cJSON *config = config_parse();
    bool known = config && config_find_page(config, id->valuestring);
    cJSON_Delete(config);
    if (!known) {
        cJSON_Delete(json);
        return web_json_error(req, "404 Not Found", "unknown page");
    }
    device_set_page(id->valuestring);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_Delete(json);
    return err;
}

/* The last log lines as text from line number `since` (default 0); X-Log-Next is the number to ask for next. When the
 * client's number is beyond ours the host restarted: the answer starts from the oldest line again. */
static esp_err_t api_log(httpd_req_t *req)
{
    uint32_t since = 0;
    char query[32], value[12];
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "since", value, sizeof value) == ESP_OK)
        since = strtoul(value, NULL, 10);
    char *text = malloc(LOG_CHUNK);
    if (!text) return web_json_error(req, "500 Internal Server Error", "out of memory");
    uint32_t next;
    size_t n = logbuf_read(since, text, LOG_CHUNK, &next);
    char header[12];
    snprintf(header, sizeof header, "%lu", (unsigned long)next);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Log-Next", header);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, n);
    free(text);
    return err;
}

/* GET /api/mqtt: the settings (never the password) and the state; PUT /api/mqtt: new settings. */
static esp_err_t api_mqtt(httpd_req_t *req)
{
    if (req->method == HTTP_PUT) {
        REQUIRE_HEADER(req);
        char body[512];
        if (!req->content_len || req->content_len >= sizeof body) return web_json_error(req, "400 Bad Request", "invalid MQTT body size");
        int n = web_receive_body(req, body, sizeof body);
        if (n <= 0) return ESP_FAIL;
        const char *problem = mqtt_update(body);
        if (problem) return web_json_error(req, "400 Bad Request", problem);
    }
    char *json = mqtt_json();
    if (!json) return web_json_error(req, "500 Internal Server Error", "out of memory");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

/* Everything else below /api that is not built yet. */
static esp_err_t api_missing(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return web_json_error(req, "404 Not Found", "not found");
    return web_json_error(req, "501 Not Implemented", "not implemented on the ESP32 host yet");
}

esp_err_t web_start(void)
{
    if (WEB_AUTH_ENABLED) {
        /* SDK debug output includes raw headers/URIs (cookies and callback codes). */
        static const char *const protocol_logs[] = {"httpd_parse", "httpd_uri", "httpd_txrx", "HTTP_CLIENT", "HTTP_HEADER", "HTTP_AUTH"};
        for (unsigned i = 0; i < sizeof protocol_logs / sizeof protocol_logs[0]; i++) esp_log_level_set(protocol_logs[i], ESP_LOG_ERROR);
    }
    esp_err_t err = web_auth_init();
    if (err != ESP_OK) return err;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 28;
    cfg.max_open_sockets = 10; /* event streams hold their socket */
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.max_req_hdr_len = 4096;
    cfg.max_uri_len = 2048;
    httpd_handle_t server;
    err = web_transport_start(&cfg, &server);
    if (err != ESP_OK) return err;
    events_start();
    keyfiles_init();
    static const httpd_uri_t routes[] = {
        {.uri = "/api/info", .method = HTTP_GET, .handler = api_info},
        {.uri = "/api/events", .method = HTTP_GET, .handler = events_handler},
        {.uri = "/api/config", .method = HTTP_GET, .handler = web_config_get},
        {.uri = "/api/config", .method = HTTP_PUT, .handler = web_config_put},
        {.uri = "/auth/*", .method = HTTP_GET, .handler = web_auth_handler},
        {.uri = "/auth/*", .method = HTTP_POST, .handler = web_auth_handler},
        {.uri = "/api/log", .method = HTTP_GET, .handler = api_log},
        {.uri = "/api/page", .method = HTTP_POST, .handler = api_page},
        {.uri = "/api/keys/*", .method = HTTP_POST, .handler = keyfiles_handler},
        {.uri = "/api/pages/*", .method = HTTP_POST, .handler = keyfiles_handler},
        {.uri = "/api/keys/*", .method = HTTP_GET, .handler = keyfiles_handler},
        {.uri = "/api/keys/*", .method = HTTP_PUT, .handler = keyfiles_handler},
        {.uri = "/api/keys/*", .method = HTTP_DELETE, .handler = keyfiles_handler},
        {.uri = "/api/pages/*", .method = HTTP_GET, .handler = keyfiles_handler},
        {.uri = "/api/pages/*", .method = HTTP_PUT, .handler = keyfiles_handler},
        {.uri = "/api/pages/*", .method = HTTP_DELETE, .handler = keyfiles_handler},
        {.uri = "/api/mqtt", .method = HTTP_GET, .handler = api_mqtt},
        {.uri = "/api/mqtt", .method = HTTP_PUT, .handler = api_mqtt},
        {.uri = "/api/*", .method = HTTP_GET, .handler = api_missing},
        {.uri = "/api/*", .method = HTTP_PUT, .handler = api_missing},
        {.uri = "/api/*", .method = HTTP_POST, .handler = api_missing},
        {.uri = "/api/*", .method = HTTP_DELETE, .handler = api_missing},
        {.uri = "/*", .method = HTTP_GET, .handler = send_file}, /* last: the wildcard matches the rest */
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        httpd_uri_t guarded = routes[i];
        guarded.handler = web_guarded_request;
        guarded.user_ctx = (void *)&routes[i];
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &guarded));
    }
    ESP_LOGI(TAG, "%s server on port %d; authentication %s", WEB_TLS ? "https" : "http", WEB_PORT,
             WEB_AUTH_ENABLED ? "enabled" : "DISABLED");
    return ESP_OK;
}

#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "config.h"
#include "config_validate.h"
#include "device.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "draw.h"
#include "events.h"
#include "keyfiles.h"
#include "logbuf.h"
#include "mqtt.h"

static const char *TAG = "web";
#define WWW "/www"
#define MAX_CONFIG 65536
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

static esp_err_t api_config_get(httpd_req_t *req)
{
    cJSON *config = config_parse();
    char *text = config ? cJSON_PrintUnformatted(config) : NULL;
    cJSON_Delete(config);
    if (!text) return web_json_error(req, "500 Internal Server Error", "cannot load the config");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t api_config_put(httpd_req_t *req)
{
    REQUIRE_HEADER(req);
    if (req->content_len == 0 || req->content_len > MAX_CONFIG) return web_json_error(req, "413 Payload Too Large", "config too large");
    char *body = malloc(req->content_len + 1);
    if (!body) return web_json_error(req, "500 Internal Server Error", "out of memory");
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = 0;
    const char *problem;
    cJSON *saved = config_validate(body, &problem);
    free(body);
    if (!saved) {
        return web_json_error(req, "400 Bad Request", problem);
    }
    cJSON *before = config_parse(); /* to see which keys change */
    body = cJSON_PrintUnformatted(saved);
    if (body && strlen(body) > MAX_CONFIG) {
        cJSON_Delete(before);
        cJSON_Delete(saved);
        free(body);
        return web_json_error(req, "413 Payload Too Large", "normalized config too large");
    }
    if (!body || !config_store(body, strlen(body))) {
        cJSON_Delete(before);
        cJSON_Delete(saved);
        ESP_LOGE(TAG, "cannot store the config");
        free(body);
        return web_json_error(req, "500 Internal Server Error", "cannot store the config");
    }
    if (saved) {
        config_purge_images(saved); /* images of deleted pages */
        mqtt_configure(saved);      /* topics of the variables */
        device_state_t state;
        device_snapshot(&state);
        const cJSON *old_level = cJSON_GetObjectItem(before, "brightness"), *level = cJSON_GetObjectItem(saved, "brightness");
        if (!cJSON_IsNumber(old_level) || old_level->valueint != level->valueint) device_set_brightness(level->valueint);
        if (!config_find_page(saved, state.page)) device_set_page("root");
        draw_request_changes(before, saved, state.page); /* only what looks different */
    }
    cJSON_Delete(saved);
    cJSON_Delete(before);
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
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 28;
    cfg.max_open_sockets = 10; /* event streams hold their socket */
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;
    events_start();
    keyfiles_init();
    static const httpd_uri_t routes[] = {
        {.uri = "/api/info", .method = HTTP_GET, .handler = api_info},
        {.uri = "/api/events", .method = HTTP_GET, .handler = events_handler},
        {.uri = "/api/config", .method = HTTP_GET, .handler = api_config_get},
        {.uri = "/api/config", .method = HTTP_PUT, .handler = api_config_put},
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
    ESP_LOGI(TAG, "http server on port %d", cfg.server_port);
    return ESP_OK;
}

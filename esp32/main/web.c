#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "device.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "events.h"
#include "logbuf.h"

static const char *TAG = "web";
#define WWW "/www"
#define CONFIG_FILE "/storage/config.json"
#define CONFIG_TMP "/storage/config.tmp"
#define MAX_CONFIG 65536
#define LOG_CHUNK 6144
#define DEFAULT_CONFIG "{\"version\":2,\"pages\":[{\"id\":\"root\",\"name\":\"Default\",\"parent\":null,\"keys\":[]}]}"

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

static esp_err_t json_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[160];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* --- protection (docs/web-api.md): changing requests need the header a foreign origin cannot set ---------- */

static bool mutation_allowed(httpd_req_t *req)
{
    char value[4];
    return httpd_req_get_hdr_value_str(req, "X-Deck-Request", value, sizeof value) == ESP_OK && value[0] == '1';
}

#define REQUIRE_HEADER(req) \
    do { if (!mutation_allowed(req)) return json_error(req, "403 Forbidden", "missing X-Deck-Request header"); } while (0)

/* --- static files --------------------------------------------------------------------------------------- */

static esp_err_t send_file(httpd_req_t *req)
{
    char path[sizeof WWW + 64];
    const char *uri = strcmp(req->uri, "/") == 0 ? "/index.html" : req->uri;
    if (strlen(uri) >= 64 || strstr(uri, "..")) return json_error(req, "404 Not Found", "not found");
    strlcpy(path, WWW, sizeof path);
    strlcat(path, uri, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return json_error(req, "404 Not Found", "not found");
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
    char body[2560];
    device_info_json(body, sizeof body);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t api_config_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    FILE *f = fopen(CONFIG_FILE, "rb");
    if (!f) return httpd_resp_sendstr(req, DEFAULT_CONFIG);
    char buf[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, sizeof buf, f)) > 0) err = httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* Basic shape only so far: a JSON object with a `pages` list. The full rules of the Linux host
 * (host/src/d200_deck/config/parse.py) follow with the key renderer. */
static const char *check_config(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) return "invalid JSON";
    const char *problem = NULL;
    if (!cJSON_IsObject(root)) problem = "config must be an object";
    else if (!cJSON_IsArray(cJSON_GetObjectItem(root, "pages")) && !cJSON_IsArray(cJSON_GetObjectItem(root, "keys")))
        problem = "config needs a `pages` list";
    cJSON_Delete(root);
    return problem;
}

static esp_err_t api_config_put(httpd_req_t *req)
{
    REQUIRE_HEADER(req);
    if (req->content_len == 0 || req->content_len > MAX_CONFIG) return json_error(req, "413 Payload Too Large", "config too large");
    char *body = malloc(req->content_len + 1);
    if (!body) return json_error(req, "500 Internal Server Error", "out of memory");
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
    const char *problem = check_config(body);
    if (problem) {
        free(body);
        return json_error(req, "400 Bad Request", problem);
    }
    FILE *f = fopen(CONFIG_TMP, "wb");
    bool ok = f && fwrite(body, 1, got, f) == got && fclose(f) == 0 && rename(CONFIG_TMP, CONFIG_FILE) == 0;
    if (!ok) {
        ESP_LOGE(TAG, "cannot store the config");
        free(body);
        return json_error(req, "500 Internal Server Error", "cannot store the config");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, got);
    free(body);
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
    if (!text) return json_error(req, "500 Internal Server Error", "out of memory");
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

/* MQTT is not part of the ESP32 host yet: report it as off. */
static esp_err_t api_mqtt(httpd_req_t *req)
{
    if (req->method == HTTP_PUT) {
        REQUIRE_HEADER(req);
        return json_error(req, "501 Not Implemented", "MQTT is not supported by this host yet");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"host\":\"\",\"port\":1883,\"username\":\"\",\"tls\":false,\"client_id\":\"\","
                                   "\"password_set\":false,\"enabled\":false,\"connected\":false,\"error\":\"\"}");
}

/* Everything else below /api that is not built yet. */
static esp_err_t api_missing(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return json_error(req, "404 Not Found", "not found");
    return json_error(req, "501 Not Implemented", "not implemented on the ESP32 host yet");
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;
    cfg.max_open_sockets = 10; /* event streams hold their socket */
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;
    events_start();
    const httpd_uri_t routes[] = {
        {.uri = "/api/info", .method = HTTP_GET, .handler = api_info},
        {.uri = "/api/events", .method = HTTP_GET, .handler = events_handler},
        {.uri = "/api/config", .method = HTTP_GET, .handler = api_config_get},
        {.uri = "/api/config", .method = HTTP_PUT, .handler = api_config_put},
        {.uri = "/api/log", .method = HTTP_GET, .handler = api_log},
        {.uri = "/api/mqtt", .method = HTTP_GET, .handler = api_mqtt},
        {.uri = "/api/mqtt", .method = HTTP_PUT, .handler = api_mqtt},
        {.uri = "/api/*", .method = HTTP_GET, .handler = api_missing},
        {.uri = "/api/*", .method = HTTP_PUT, .handler = api_missing},
        {.uri = "/api/*", .method = HTTP_POST, .handler = api_missing},
        {.uri = "/api/*", .method = HTTP_DELETE, .handler = api_missing},
        {.uri = "/*", .method = HTTP_GET, .handler = send_file}, /* last: the wildcard matches the rest */
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) httpd_register_uri_handler(server, &routes[i]);
    ESP_LOGI(TAG, "http server on port %d", cfg.server_port);
    return ESP_OK;
}

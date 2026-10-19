#include "web.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "usb_adb.h"

static const char *TAG = "web";
#define WWW "/www"

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

static esp_err_t send_file(httpd_req_t *req)
{
    char path[sizeof WWW + 64];
    const char *uri = strcmp(req->uri, "/") == 0 ? "/index.html" : req->uri;
    if (strlen(uri) >= 64 || strstr(uri, "..")) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    strlcpy(path, WWW, sizeof path);
    strlcat(path, uri, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    httpd_resp_set_type(req, type_of(path));
    char buf[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, sizeof buf, f)) > 0) err = httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* Stub until the deckd client exists: the UI shows "no device". */
static esp_err_t api_info(httpd_req_t *req)
{
    char body[256];
    snprintf(body, sizeof body,
             "{\"api\":1,\"host\":\"esp32\",\"connected\":false,\"usb_device\":%s,"
             "\"screen\":{\"width\":1280,\"height\":720},\"keys\":[],\"actions\":[\"bl_down\",\"bl_up\","
             "\"disp_off\",\"disp_on\",\"page\",\"back\"],\"variable_types\":[]}",
             usb_adb_device_present() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t info = {.uri = "/api/info", .method = HTTP_GET, .handler = api_info};
    const httpd_uri_t files = {.uri = "/*", .method = HTTP_GET, .handler = send_file};
    httpd_register_uri_handler(server, &info);
    httpd_register_uri_handler(server, &files); /* last: the wildcard matches everything left */
    ESP_LOGI(TAG, "http server on port %d", cfg.server_port);
    return ESP_OK;
}

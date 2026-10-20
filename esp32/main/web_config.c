#include "web.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "config.h"
#include "config_validate.h"
#include "device.h"
#include "draw.h"
#include "mqtt.h"

#define MAX_CONFIG 65536

esp_err_t web_config_get(httpd_req_t *req)
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

esp_err_t web_config_put(httpd_req_t *req)
{
    if (!web_mutation_allowed(req)) return web_json_error(req, "403 Forbidden", "missing X-Deck-Request header");
    if (!req->content_len || req->content_len > MAX_CONFIG) return web_json_error(req, "413 Payload Too Large", "config too large");
    char *body = malloc(req->content_len + 1);
    if (!body) return web_json_error(req, "500 Internal Server Error", "out of memory");
    if (web_receive_body(req, body, req->content_len + 1) <= 0) { free(body); return ESP_FAIL; }
    const char *problem;
    cJSON *saved = config_validate(body, &problem);
    free(body);
    if (!saved) return web_json_error(req, "400 Bad Request", problem);
    cJSON *before = config_parse();
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
        free(body);
        return web_json_error(req, "500 Internal Server Error", "cannot store the config");
    }
    config_purge_images(saved);
    mqtt_configure(saved);
    device_state_t state;
    device_snapshot(&state);
    const cJSON *old_level = cJSON_GetObjectItem(before, "brightness"), *level = cJSON_GetObjectItem(saved, "brightness");
    if (!cJSON_IsNumber(old_level) || old_level->valueint != level->valueint) device_set_brightness(level->valueint);
    if (!config_find_page(saved, state.page)) device_set_page("root");
    draw_request_changes(before, saved, state.page);
    cJSON_Delete(saved);
    cJSON_Delete(before);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

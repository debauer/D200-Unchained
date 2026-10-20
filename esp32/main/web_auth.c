#include "web_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "web.h"
#include "web_settings.h"

static auth_store_t s_store;
static SemaphoreHandle_t s_lock;
static esp_err_t status_send(httpd_req_t *req, uint64_t ticket);

bool web_auth_required(void) { return WEB_AUTH_ENABLED; }

esp_err_t web_auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    return WEB_AUTH_ENABLED ? web_auth_login_init() : ESP_OK;
}

uint64_t web_auth_ticket(httpd_req_t *req)
{
    if (!WEB_AUTH_ENABLED) return AUTH_OPEN_TICKET;
    char cookie[1024], token[AUTH_TOKEN_LENGTH + 1];
    uint8_t hash[32];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) != ESP_OK ||
        !auth_cookie_parse(cookie, WEB_COOKIE_NAME, token) ||
        mbedtls_sha256((const uint8_t *)token, AUTH_TOKEN_LENGTH, hash, 0) != 0) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t ticket = auth_store_find(&s_store, hash, esp_timer_get_time());
    xSemaphoreGive(s_lock);
    return ticket;
}

bool web_auth_ticket_valid(uint64_t ticket)
{
    if (!WEB_AUTH_ENABLED) return ticket == AUTH_OPEN_TICKET;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool valid = auth_store_get(&s_store, ticket, esp_timer_get_time()) != NULL;
    xSemaphoreGive(s_lock);
    return valid;
}

void web_auth_revoke(uint64_t ticket)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    auth_store_revoke(&s_store, ticket);
    xSemaphoreGive(s_lock);
    events_revoke(ticket);
}

bool web_auth_create(httpd_req_t *req, uint64_t previous, const char *user)
{
    uint8_t random[32], hash[32];
    char token[AUTH_TOKEN_LENGTH + 1];
    esp_fill_random(random, sizeof random);
    for (unsigned i = 0; i < sizeof random; i++) snprintf(token + 2 * i, 3, "%02x", random[i]);
    if (mbedtls_sha256((const uint8_t *)token, AUTH_TOKEN_LENGTH, hash, 0) != 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    auth_store_revoke(&s_store, previous);
    uint64_t ticket = auth_store_create(&s_store, hash, user, esp_timer_get_time(), WEB_SESSION_TTL);
    xSemaphoreGive(s_lock);
    events_revoke(previous);
    if (!ticket) return false;
    char cookie[200];
    snprintf(cookie, sizeof cookie, "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%u%s", WEB_COOKIE_NAME,
             token, WEB_SESSION_TTL, WEB_TLS ? "; Secure" : "");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    esp_err_t err = status_send(req, ticket); /* cookie stays alive until the nested response is sent. */
    if (err != ESP_OK) web_auth_revoke(ticket);
    return true; /* The response was attempted; never send a second one on the same request. */
}

static esp_err_t status_send(httpd_req_t *req, uint64_t ticket)
{
    char user[AUTH_USER_LENGTH + 1] = "";
    uint32_t remaining = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t now = esp_timer_get_time();
    const auth_session_t *s = auth_store_get(&s_store, ticket, now);
    if (s) { strcpy(user, s->user); remaining = (uint32_t)((s->expires - now + 999999) / 1000000); }
    xSemaphoreGive(s_lock);
    cJSON *o = cJSON_CreateObject();
    if (!o) return web_json_error(req, "500 Internal Server Error", "out of memory");
    cJSON_AddBoolToObject(o, "required", WEB_AUTH_ENABLED);
    cJSON_AddBoolToObject(o, "authenticated", !WEB_AUTH_ENABLED || user[0]);
    cJSON_AddNumberToObject(o, "session_ttl_seconds", WEB_SESSION_TTL);
    cJSON_AddNumberToObject(o, "expires_in", remaining);
    cJSON *methods = cJSON_AddArrayToObject(o, "methods");
    if (WEB_AUTH_ENABLED) cJSON_AddItemToArray(methods, cJSON_CreateString("password"));
    if (user[0]) cJSON_AddStringToObject(o, "username", user);
    else cJSON_AddNullToObject(o, "username");
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) return web_json_error(req, "500 Internal Server Error", "out of memory");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

esp_err_t web_auth_status(httpd_req_t *req) { return status_send(req, web_auth_ticket(req)); }

bool web_auth_guard(httpd_req_t *req)
{
    bool api = !strncmp(req->uri, "/api/", 5), auth = !strncmp(req->uri, "/auth/", 6);
    if (api || auth) httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (WEB_AUTH_ENABLED && api && !web_auth_ticket(req)) {
        httpd_resp_set_hdr(req, "Connection", "close");
        web_json_error(req, "401 Unauthorized", "authentication required");
        return false;
    }
    return true;
}

esp_err_t web_auth_handler(httpd_req_t *req)
{
    if (!strcmp(req->uri, "/auth/session") && req->method == HTTP_GET) return web_auth_status(req);
    if (!WEB_AUTH_ENABLED) return web_json_error(req, "404 Not Found", "authentication disabled");
    if (req->method != HTTP_POST) return web_json_error(req, "405 Method Not Allowed", "method not allowed");
    if (!web_mutation_allowed(req)) return web_json_error(req, "403 Forbidden", "missing X-Deck-Request header");
    if (!strcmp(req->uri, "/auth/login")) return web_auth_login(req);
    if (!strcmp(req->uri, "/auth/logout")) {
        uint64_t ticket = web_auth_ticket(req);
        if (ticket) web_auth_revoke(ticket);
        char cookie[160];
        snprintf(cookie, sizeof cookie, "%s=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0%s", WEB_COOKIE_NAME, WEB_TLS ? "; Secure" : "");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"logged_out\":true}");
    }
    return web_json_error(req, "404 Not Found", "not found");
}

#include "web_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auth_password.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "web.h"

typedef struct {
    httpd_req_t *req;
    uint64_t previous;
    char username[AUTH_USER_LENGTH + 1], password[129];
} login_t;

static QueueHandle_t s_jobs;
static auth_limiter_t s_limiter; /* Only accessed by the HTTP server task. */

static bool parse(login_t *job, char *body)
{
    /* The public login schema has one object and two strings. Bound nesting before calling cJSON. */
    bool quoted = false, object = false;
    for (const char *p = body; *p; p++) {
        if (quoted && *p == '\\' && p[1]) {
            if (p[1] == 'u' && !strncmp(p + 2, "0000", 4)) return false;
            p++;
            continue;
        }
        if (*p == '"') quoted = !quoted;
        if (!quoted && (*p == '[' || (*p == '{' && object))) return false;
        if (!quoted && *p == '{') object = true;
    }
    cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
    cJSON *user = cJSON_GetObjectItemCaseSensitive(json, "username"), *pass = cJSON_GetObjectItemCaseSensitive(json, "password");
    bool ok = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 2 && cJSON_IsString(user) && cJSON_IsString(pass) &&
              strlen(user->valuestring) <= AUTH_USER_LENGTH && strlen(pass->valuestring) <= 128;
    if (ok) { strcpy(job->username, user->valuestring); strcpy(job->password, pass->valuestring); }
    if (cJSON_IsString(pass)) mbedtls_platform_zeroize(pass->valuestring, strlen(pass->valuestring));
    cJSON_Delete(json);
    return ok;
}

static void worker(void *arg)
{
    login_t *job;
    for (;;) {
        xQueueReceive(s_jobs, &job, portMAX_DELAY);
        bool valid = auth_password_verify(job->username, job->password);
        mbedtls_platform_zeroize(job->password, sizeof job->password);
        if (!valid) web_json_error(job->req, "401 Unauthorized", "invalid username or password");
        else if (!web_auth_create(job->req, job->previous, job->username))
            web_json_error(job->req, "503 Service Unavailable", "session capacity reached");
        httpd_req_async_handler_complete(job->req);
        mbedtls_platform_zeroize(job, sizeof *job);
        free(job);
    }
}

esp_err_t web_auth_login_init(void)
{
    s_jobs = xQueueCreate(2, sizeof(login_t *));
    if (!s_jobs) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "web_login", 8192, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(s_jobs);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t web_auth_login(httpd_req_t *req)
{
    struct sockaddr_in peer;
    socklen_t size = sizeof peer;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &size) != 0 || peer.sin_family != AF_INET) return ESP_FAIL;
    unsigned retry = auth_rate_attempt(&s_limiter, peer.sin_addr.s_addr, esp_timer_get_time());
    if (retry) {
        char delay[16];
        snprintf(delay, sizeof delay, "%u", retry);
        httpd_resp_set_hdr(req, "Retry-After", delay);
        return web_json_error(req, "429 Too Many Requests", "too many login attempts");
    }
    if (!req->content_len || req->content_len >= 512) return web_json_error(req, "400 Bad Request", "invalid login body size");
    login_t *job = calloc(1, sizeof *job);
    if (!job) return web_json_error(req, "503 Service Unavailable", "login busy");
    char body[512] = {0};
    int n = web_receive_body(req, body, sizeof body);
    bool parsed = n > 0 && !memchr(body, 0, n) && parse(job, body);
    mbedtls_platform_zeroize(body, sizeof body);
    if (!parsed) {
        mbedtls_platform_zeroize(job, sizeof *job);
        free(job);
        return n > 0 ? web_json_error(req, "400 Bad Request", "expected username and password") : ESP_FAIL;
    }
    job->previous = web_auth_ticket(req);
    esp_err_t err = httpd_req_async_handler_begin(req, &job->req);
    if (err != ESP_OK || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        if (err == ESP_OK) {
            web_json_error(job->req, "503 Service Unavailable", "login busy");
            httpd_req_async_handler_complete(job->req);
        }
        mbedtls_platform_zeroize(job, sizeof *job);
        free(job);
    }
    return err;
}

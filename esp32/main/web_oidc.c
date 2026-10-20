#include "web_oidc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "auth_core.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "web.h"
#include "web_auth.h"
#include "web_settings.h"

#if defined(ESP_PLATFORM) && WEB_OIDC_ENABLED && CONFIG_HTTPD_MAX_URI_LEN < 2048
#error "OIDC requires CONFIG_HTTPD_MAX_URI_LEN=2048; update the existing sdkconfig in menuconfig"
#endif

typedef struct {
    httpd_req_t *req;
    bool callback, malformed, denied;
    uint64_t previous;
    char cookie[65], state[65], code[1025], issuer[256];
} job_t;

static QueueHandle_t s_jobs;
static oidc_transactions_t s_transactions; /* Accessed only by the OIDC worker. */
static oidc_client_t s_client = {
    .issuer = WEB_OIDC_ISSUER, .client_id = WEB_OIDC_CLIENT_ID, .scopes = WEB_OIDC_SCOPES,
    .redirect_uri = WEB_ORIGIN OIDC_CALLBACK,
};
static auth_limiter_t s_limiter; /* HTTP server task only. */
static int64_t unix_now(void) { return time(NULL); }

static void cookie(char *out, size_t size, const char *value, unsigned seconds)
{
    snprintf(out, size, "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%u%s", WEB_OIDC_COOKIE, value, seconds, WEB_TLS ? "; Secure" : "");
}

static esp_err_t redirect(httpd_req_t *req, const char *error)
{
    char clear[160], location[64];
    cookie(clear, sizeof clear, "", 0);
    snprintf(location, sizeof location, "/?auth_error=%s", error);
    httpd_resp_set_hdr(req, "Set-Cookie", clear);
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_status(req, "302 Found");
    return httpd_resp_sendstr(req, "");
}

static void start(job_t *job)
{
    if (time(NULL) < 1700000000) { web_json_error(job->req, "503 Service Unavailable", "clock not synchronized; local login is available"); return; }
    uint8_t entropy[128];
    esp_fill_random(entropy, sizeof entropy);
    oidc_transaction_t tx;
    bool created = oidc_tx_create(&s_transactions, entropy, esp_timer_get_time(), job->cookie, &tx);
    mbedtls_platform_zeroize(entropy, sizeof entropy);
    if (!created) { web_json_error(job->req, "503 Service Unavailable", "login capacity reached"); return; }
    char *url = oidc_authorization(&s_client, &tx, esp_timer_get_time());
    cJSON *json = cJSON_CreateObject();
    char *body = url && json && cJSON_AddStringToObject(json, "redirect", url) ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    free(url);
    bool failed = !body;
    if (body) {
        char value[240];
        cookie(value, sizeof value, job->cookie, OIDC_TX_SECONDS);
        httpd_resp_set_hdr(job->req, "Set-Cookie", value);
        httpd_resp_set_type(job->req, "application/json");
        failed = httpd_resp_sendstr(job->req, body) != ESP_OK;
    } else web_json_error(job->req, "502 Bad Gateway", "identity provider not reachable or invalid metadata");
    if (failed) { oidc_transaction_t removed; oidc_tx_take(&s_transactions, job->cookie, tx.state, esp_timer_get_time(), &removed); mbedtls_platform_zeroize(&removed, sizeof removed); }
    free(body);
    mbedtls_platform_zeroize(&tx, sizeof tx);
}

static void callback(job_t *job)
{
    oidc_transaction_t tx;
    if (!oidc_tx_take(&s_transactions, job->cookie, job->state, esp_timer_get_time(), &tx)) {
        redirect(job->req, "expired");
        return;
    }
    const char *error = job->malformed ? "token" : job->denied ? "denied" : NULL;
    if (!error && job->issuer[0] && strcmp(job->issuer, WEB_OIDC_ISSUER)) error = "token";
    oidc_identity_t identity;
    if (!error) error = oidc_exchange(&s_client, job->code, &tx, esp_timer_get_time(), unix_now, &identity);
    mbedtls_platform_zeroize(&tx, sizeof tx);
    if (error) { redirect(job->req, error); return; }
    char clear[160];
    cookie(clear, sizeof clear, "", 0);
    httpd_resp_set_hdr(job->req, "Set-Cookie", clear);
    if (!web_auth_create_oidc(job->req, job->previous, identity.username, identity.identity)) {
        httpd_resp_set_status(job->req, "302 Found");
        httpd_resp_set_hdr(job->req, "Location", "/?auth_error=capacity");
        httpd_resp_sendstr(job->req, "");
    }
}

static void worker(void *arg)
{
    job_t *job;
    for (;;) {
        xQueueReceive(s_jobs, &job, portMAX_DELAY);
        if (job->callback) callback(job);
        else start(job);
        httpd_req_async_handler_complete(job->req);
        mbedtls_platform_zeroize(job, sizeof *job);
        free(job);
    }
}

esp_err_t web_oidc_init(void)
{
    s_jobs = xQueueCreate(2, sizeof(job_t *));
    if (!s_jobs) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "web_oidc", 16384, NULL, 2, NULL) != pdPASS) { vQueueDelete(s_jobs); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

static esp_err_t submit(httpd_req_t *req, job_t *job)
{
    job->previous = web_auth_ticket(req);
    esp_err_t err = httpd_req_async_handler_begin(req, &job->req);
    if (err != ESP_OK || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        if (err == ESP_OK) { web_json_error(job->req, "503 Service Unavailable", "login busy"); httpd_req_async_handler_complete(job->req); }
        mbedtls_platform_zeroize(job, sizeof *job);
        free(job);
    }
    return err;
}

esp_err_t web_oidc_start(httpd_req_t *req)
{
    struct sockaddr_in peer;
    socklen_t size = sizeof peer;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &size) || peer.sin_family != AF_INET) return ESP_FAIL;
    unsigned retry = auth_rate_attempt(&s_limiter, peer.sin_addr.s_addr, esp_timer_get_time());
    if (retry) {
        char seconds[16];
        snprintf(seconds, sizeof seconds, "%u", retry);
        httpd_resp_set_hdr(req, "Retry-After", seconds);
        return web_json_error(req, "429 Too Many Requests", "too many login attempts");
    }
    if (req->content_len) {
        char body[33];
        if (req->content_len >= sizeof body) return web_json_error(req, "400 Bad Request", "unexpected start body");
        if (web_receive_body(req, body, sizeof body) <= 0) return ESP_FAIL;
        cJSON *json = oidc_json(body, strlen(body));
        bool valid = json && !cJSON_GetArraySize(json);
        cJSON_Delete(json);
        if (!valid) return web_json_error(req, "400 Bad Request", "expected empty start body");
    }
    job_t *job = calloc(1, sizeof *job);
    return job ? submit(req, job) : web_json_error(req, "503 Service Unavailable", "login busy");
}

esp_err_t web_oidc_callback(httpd_req_t *req)
{
    char cookies[1024], browser[65];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookies, sizeof cookies) != ESP_OK ||
        !auth_cookie_parse(cookies, WEB_OIDC_COOKIE, browser)) return redirect(req, "expired");
    size_t size = httpd_req_get_url_query_len(req);
    if (!size || size > 2048) return redirect(req, "expired");
    char *query = malloc(size + 1);
    job_t *job = calloc(1, sizeof *job);
    if (!query || !job) { free(query); free(job); return redirect(req, "provider"); }
    if (httpd_req_get_url_query_str(req, query, size + 1) != ESP_OK) { free(query); free(job); return redirect(req, "expired"); }
    job->callback = true;
    strcpy(job->cookie, browser);
    char error[128];
    int state = oidc_query(query, "state", job->state, sizeof job->state);
    int code = oidc_query(query, "code", job->code, sizeof job->code);
    int denied = oidc_query(query, "error", error, sizeof error);
    int issuer = oidc_query(query, "iss", job->issuer, sizeof job->issuer);
    job->malformed = state != 1 || code < 0 || denied < 0 || issuer < 0;
    job->denied = denied == 1;
    if (state != 1) job->state[0] = 0;
    if (code != 1) job->code[0] = 0;
    mbedtls_platform_zeroize(query, size + 1);
    free(query);
    return submit(req, job);
}

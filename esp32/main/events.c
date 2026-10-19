#include "events.h"

#include <stdio.h>
#include <string.h>

#include "device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "events";
#define MAX_CLIENTS 4
#define KEEPALIVE_US (15 * 1000 * 1000)

static httpd_req_t *s_clients[MAX_CLIENTS];
static SemaphoreHandle_t s_lock;

static esp_err_t send_to(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

static void drop(int i)
{
    httpd_req_async_handler_complete(s_clients[i]);
    s_clients[i] = NULL;
    ESP_LOGI(TAG, "client %d left", i);
}

static void send_all(const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i] && send_to(s_clients[i], text) != ESP_OK) drop(i);
    }
    xSemaphoreGive(s_lock);
}

void events_publish(const char *name, const char *json)
{
    char text[256];
    snprintf(text, sizeof text, "event: %s\ndata: %s\n\n", name, json);
    send_all(text);
}

static void keepalive(void *arg)
{
    send_all(": keepalive\n\n");
}

esp_err_t events_handler(httpd_req_t *req)
{
    httpd_req_t *copy;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) return err;
    httpd_resp_set_type(copy, "text/event-stream");
    httpd_resp_set_hdr(copy, "Cache-Control", "no-cache");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) {
        if (!s_clients[i]) slot = i;
    }
    if (slot < 0) {
        xSemaphoreGive(s_lock);
        httpd_resp_set_status(copy, "503 Service Unavailable");
        httpd_resp_sendstr(copy, "too many event streams");
        httpd_req_async_handler_complete(copy);
        return ESP_OK;
    }
    s_clients[slot] = copy;
    /* the state right after connecting: status, brightness, page, values */
    char json[160], text[300];
    static const char *const initial[] = {"status", "brightness", "page", "values"};
    for (size_t i = 0; i < sizeof initial / sizeof initial[0]; i++) {
        device_event_json(initial[i], json, sizeof json);
        snprintf(text, sizeof text, "event: %s\ndata: %s\n\n", initial[i], json);
        if (send_to(copy, text) != ESP_OK) {
            drop(slot);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void events_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = keepalive, .name = "sse_keepalive"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, KEEPALIVE_US));
}

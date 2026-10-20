#include "events.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "mqtt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "events";
#define MAX_CLIENTS 4
#define KEEPALIVE_US (15 * 1000 * 1000)

static httpd_req_t *s_clients[MAX_CLIENTS];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_pending;
static portMUX_TYPE s_overflow_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_overflow;

static int send_nonblocking(httpd_handle_t server, int fd, const char *buf, size_t size, int flags)
{
    return send(fd, buf, size, flags | MSG_DONTWAIT);
}

static esp_err_t send_to(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

static void drop(int i)
{
    httpd_handle_t server = s_clients[i]->handle;
    int fd = httpd_req_to_sockfd(s_clients[i]);
    shutdown(fd, SHUT_RDWR);
    httpd_req_async_handler_complete(s_clients[i]);
    httpd_sess_trigger_close(server, fd);
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

static void enqueue(char *text)
{
    if (!text) return;
    if (xQueueSend(s_pending, &text, 0) != pdTRUE) {
        free(text);
        portENTER_CRITICAL(&s_overflow_lock);
        s_overflow = true;
        portEXIT_CRITICAL(&s_overflow_lock);
    }
}

void events_publish(const char *name, const char *json)
{
    size_t size = strlen(name) + strlen(json) + 24;
    char *text = malloc(size);
    if (text) snprintf(text, size, "event: %s\ndata: %s\n\n", name, json);
    enqueue(text);
}

static void sender(void *arg)
{
    char *text;
    for (;;) {
        xQueueReceive(s_pending, &text, portMAX_DELAY);
        portENTER_CRITICAL(&s_overflow_lock);
        bool overflow = s_overflow;
        s_overflow = false;
        portEXIT_CRITICAL(&s_overflow_lock);
        if (overflow) { /* Reconnect resends a complete snapshot instead of leaving clients with missed state. */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < MAX_CLIENTS; i++) if (s_clients[i]) drop(i);
            xSemaphoreGive(s_lock);
            free(text);
            while (xQueueReceive(s_pending, &text, 0) == pdTRUE) free(text);
            ESP_LOGW(TAG, "event queue full; clients must resync");
            continue;
        }
        send_all(text);
        free(text);
    }
}

static void keepalive(void *arg)
{
    enqueue(strdup(": keepalive\n\n"));
}

/* The current body of the event `name`. free() it. */
static char *state_json(const char *name)
{
    if (strcmp(name, "values") == 0) return device_values_json();
    if (strcmp(name, "mqtt") == 0) return mqtt_json();
    char *json = malloc(160);
    if (json) device_event_json(name, json, 160);
    return json;
}

esp_err_t events_handler(httpd_req_t *req)
{
    httpd_req_t *copy;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) return err;
    httpd_resp_set_type(copy, "text/event-stream");
    httpd_resp_set_hdr(copy, "Cache-Control", "no-cache");
    err = httpd_sess_set_send_override(copy->handle, httpd_req_to_sockfd(copy), send_nonblocking);
    if (err != ESP_OK) {
        httpd_req_async_handler_complete(copy);
        return err;
    }
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
    /* the state right after connecting: status, brightness, page, values, mqtt */
    static const char *const initial[] = {"status", "brightness", "page", "values", "mqtt"};
    for (size_t i = 0; i < sizeof initial / sizeof initial[0]; i++) {
        char *json = state_json(initial[i]);
        size_t size = json ? strlen(json) + 40 : 0;
        char *text = json ? malloc(size) : NULL;
        if (text) snprintf(text, size, "event: %s\ndata: %s\n\n", initial[i], json);
        bool failed = !text || send_to(copy, text) != ESP_OK;
        free(text);
        free(json);
        if (failed) {
            drop(slot);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void events_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_pending = xQueueCreate(16, sizeof(char *));
    assert(s_lock && s_pending);
    BaseType_t task = xTaskCreate(sender, "sse_send", 8192, NULL, 4, NULL);
    assert(task == pdPASS);
}

void events_start(void)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = keepalive, .name = "sse_keepalive"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, KEEPALIVE_US));
}

/* Minimal HTTP/RTOS adapters; use real firmware sessions, login code and mbedTLS on the PC. */
#include "auth_platform.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

int64_t auth_test_now = 1000;
uint64_t auth_test_revoked;
static pthread_mutex_t responses = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t response_ready = PTHREAD_COND_INITIALIZER;
int64_t esp_timer_get_time(void) { return auth_test_now; }
void esp_fill_random(void *out, size_t size)
{
    static unsigned char next;
    for (size_t i = 0; i < size; i++) ((unsigned char *)out)[i] = next++;
}
void events_revoke(uint64_t ticket) { auth_test_revoked = ticket; }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *lock = malloc(sizeof *lock);
    assert(lock && !pthread_mutex_init(lock, NULL));
    return lock;
}
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned ticks) { (void)ticks; assert(!pthread_mutex_lock(lock)); return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t lock) { assert(!pthread_mutex_unlock(lock)); return pdTRUE; }

typedef struct { pthread_mutex_t lock; pthread_cond_t ready; void *items[2]; unsigned count, capacity; } queue_t;
QueueHandle_t xQueueCreate(unsigned capacity, unsigned size)
{
    assert(capacity <= 2 && size == sizeof(void *));
    queue_t *q = calloc(1, sizeof *q);
    assert(q && !pthread_mutex_init(&q->lock, NULL) && !pthread_cond_init(&q->ready, NULL));
    q->capacity = capacity;
    return q;
}
int xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks)
{
    (void)ticks;
    queue_t *q = queue;
    pthread_mutex_lock(&q->lock);
    bool full = q->count == q->capacity;
    if (!full) { q->items[q->count++] = *(void *const *)item; pthread_cond_signal(&q->ready); }
    pthread_mutex_unlock(&q->lock);
    return !full;
}
int xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks)
{
    queue_t *q = queue;
    pthread_mutex_lock(&q->lock);
    while (!q->count && ticks) pthread_cond_wait(&q->ready, &q->lock);
    bool found = q->count != 0;
    if (found) { *(void **)item = q->items[0]; q->count--; memmove(q->items, q->items + 1, q->count * sizeof(void *)); }
    pthread_mutex_unlock(&q->lock);
    return found;
}
void vQueueDelete(QueueHandle_t queue) { free(queue); }
static void *start(void *arg)
{
    void (**function)(void *) = arg;
    void (*run)(void *) = *function;
    free(arg);
    run(NULL);
    return NULL;
}
BaseType_t xTaskCreate(void (*function)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    void (**copy)(void *) = malloc(sizeof function);
    *copy = function;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, start, copy));
    pthread_detach(thread);
    return pdPASS;
}

static const char *header(httpd_req_t *req, const char *name)
{
    if (!strcmp(name, "Cookie")) return req->cookie;
    if (!strcmp(name, "Origin")) return req->origin;
    if (!strcmp(name, "Host")) return req->host;
    if (!strcmp(name, "X-Deck-Request")) return req->deck;
    return NULL;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name)
{
    const char *value = header(req, name);
    return value ? strlen(value) : 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *out, size_t size)
{
    const char *value = header(req, name);
    if (!value || strlen(value) >= size) return ESP_FAIL;
    strcpy(out, value);
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *req, char *out, size_t size)
{
    (void)size;
    if (req->offset == req->content_len) return 0;
    *out = req->body[req->offset++];
    return 1;
}
int httpd_req_to_sockfd(httpd_req_t *req) { return req->fd; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value)
{
    assert(req->header_count < 8);
    unsigned i = req->header_count++;
    req->header_names[i] = name;
    req->header_values[i] = value; /* Deliberately retain pointers, like ESP-IDF. */
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type) { (void)req; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    snprintf(req->response_status, sizeof req->response_status, "%s", status);
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *text)
{
    httpd_req_t *target = req->parent ? req->parent : req;
    snprintf(target->response, sizeof target->response, "%s", text);
    if (target != req || !req->response_status[0])
        snprintf(target->response_status, sizeof target->response_status, "%s", req->response_status[0] ? req->response_status : "200 OK");
    for (unsigned i = 0; i < req->header_count; i++) if (!strcmp(req->header_names[i], "Set-Cookie"))
        snprintf(target->response_cookie, sizeof target->response_cookie, "%s", req->header_values[i]);
    return ESP_OK;
}
esp_err_t httpd_req_async_handler_begin(httpd_req_t *req, httpd_req_t **copy)
{
    *copy = malloc(sizeof **copy);
    assert(*copy);
    **copy = *req;
    (*copy)->parent = req;
    return ESP_OK;
}
esp_err_t httpd_req_async_handler_complete(httpd_req_t *req)
{
    pthread_mutex_lock(&responses);
    req->parent->done = true;
    pthread_cond_signal(&response_ready);
    pthread_mutex_unlock(&responses);
    free(req);
    return ESP_OK;
}
void auth_test_wait(httpd_req_t *req)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;
    pthread_mutex_lock(&responses);
    while (!req->done) assert(!pthread_cond_timedwait(&response_ready, &responses, &deadline));
    pthread_mutex_unlock(&responses);
}
esp_err_t web_json_error(httpd_req_t *req, const char *status, const char *message)
{
    char json[256];
    snprintf(json, sizeof json, "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, json);
}
bool web_mutation_allowed(httpd_req_t *req) { return req->deck && !strcmp(req->deck, "1"); }

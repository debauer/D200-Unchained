#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "actions.h"
#include "config.h"
#include "device.h"
#include "draw.h"

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool exercised;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
int xSemaphoreTake(SemaphoreHandle_t sem, unsigned ticks)
{
    (void)ticks;
    assert(!pthread_mutex_lock(sem));
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t sem) { assert(!pthread_mutex_unlock(sem)); return pdTRUE; }
void vTaskDelay(unsigned ticks) { (void)ticks; }
void events_publish(const char *name, const char *json) { (void)name; (void)json; }
void device_event_json(const char *name, char *out, size_t size) { (void)name; snprintf(out, size, "{}"); }
cJSON *config_parse(void) { return cJSON_Parse("{\"brightness\":73}"); }
void adb_set_trace(bool trace) { (void)trace; }
bool wallclock_now(struct tm *out) { (void)out; return false; }
void drawdyn_request_all(void) {}
bool actions_run(unsigned index, const char *page, bool test) { (void)index; (void)page; (void)test; return false; }
esp_err_t draw_page(deckd_t *d, const device_state_t *s, uint32_t keys, bool clear)
{
    (void)d; (void)s; (void)keys; (void)clear;
    return ESP_OK;
}
esp_err_t deckd_open(deckd_t *d, adb_t *a) { memset(d, 0, sizeof *d); d->adb = a; return ESP_OK; }
esp_err_t deckd_request(deckd_t *d, uint8_t type, const void *data, size_t length, uint8_t *reply_type,
                        uint8_t *reply, size_t cap, size_t *reply_len)
{
    (void)d; (void)data; (void)length;
    assert(type == DECKD_HELLO && cap >= 7);
    static const uint8_t info[] = {5, 0, 0, 5, 208, 2, 0};
    memcpy(reply, info, sizeof info);
    *reply_type = DECKD_INFO;
    *reply_len = sizeof info;
    return ESP_OK;
}

static void *increments(void *arg)
{
    (void)arg;
    device_state_t s;
    for (int i = 0; i < 100; i++) assert(device_adjust_backlight(1, -1, &s));
    return NULL;
}
static void *off(void *arg)
{
    (void)arg;
    device_state_t s;
    assert(device_adjust_backlight(0, 0, &s));
    return NULL;
}
bool usb_adb_device_present(void)
{
    assert(!exercised);
    exercised = true;
    device_state_t s;
    device_set_backlight(0, true);
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, increments, NULL));
    assert(!pthread_create(&b, NULL, increments, NULL));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    device_snapshot(&s);
    assert(s.brightness == 200);
    assert(device_adjust_backlight(51, -1, &s) && s.brightness == 251);
    assert(device_adjust_backlight(51, -1, &s) && s.brightness == 255);
    for (int i = 0; i < 100; i++) {
        device_set_backlight(100, true);
        assert(!pthread_create(&a, NULL, off, NULL));
        assert(device_adjust_backlight(51, -1, &s));
        assert(!pthread_join(a, NULL));
        device_snapshot(&s);
        assert(!s.display_on && s.brightness == 151);
    }
    device_set_brightness(40);
    device_snapshot(&s);
    assert(s.brightness == 40 && !s.display_on);
    return false;
}
int main(void)
{
    device_init();
    device_state_t s;
    device_snapshot(&s);
    assert(s.brightness == 73 && !s.connected);
    assert(!device_adjust_backlight(51, 0, &s));
    device_snapshot(&s);
    assert(s.brightness == 73 && s.display_on);
    adb_t a = {0};
    assert(device_run(&a) == ESP_OK && exercised);
    device_snapshot(&s);
    assert(!s.connected);
    puts("device state ok");
}

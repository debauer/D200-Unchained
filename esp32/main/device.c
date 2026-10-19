#include "device.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "actions.h"
#include "deckd.h"
#include "draw.h"
#include "placeholders.h"
#include "drawdyn.h"
#include "wallclock.h"
#include "esp_log.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb_adb.h"

static const char *TAG = "device";
#define START_BRIGHTNESS 200
#define PING_MS 100

static device_state_t s_state = {.brightness = START_BRIGHTNESS, .display_on = true, .page = "root"};

/* The measured D200 grid (host/src/d200_deck/device/layout.py), used until deckd reports its own so that the
 * interface can be configured without the panel. Key 13 is the wide one. */
static void default_layout(device_state_t *s)
{
    static const uint16_t codes[3][5] = {{29, 15, 14, 13, 12}, {11, 10, 9, 8, 7}, {34, 33, 31, 30, 0}};
    s->screen_width = 1280;
    s->screen_height = 720;
    s->key_count = 0;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 5; col++) {
            if (!codes[row][col]) continue;
            s->keys[s->key_count] = (device_key_t){s->key_count, codes[row][col], 11 + col * 260, 3 + row * 259,
                                                   codes[row][col] == 30 ? 456 : 196, 196};
            s->key_count++;
        }
    }
}
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_redraw_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_redraw_all = true;
static uint32_t s_redraw_keys; /* bit per key index */
static volatile bool s_brightness_dirty;

void device_request_redraw(int key)
{
    portENTER_CRITICAL(&s_redraw_lock);
    if (key < 0 || key >= 32) s_redraw_all = true;
    else s_redraw_keys |= 1u << key;
    portEXIT_CRITICAL(&s_redraw_lock);
}

void device_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    default_layout(&s_state);
}

bool device_key_window(unsigned index, device_key_t *out)
{
    device_state_t s;
    device_snapshot(&s);
    for (int i = 0; i < s.key_count; i++) {
        if (s.keys[i].index == index) {
            *out = s.keys[i];
            return true;
        }
    }
    return false;
}

void device_snapshot(device_state_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

static int percent(int level)
{
    return (level * 100 + 127) / 255;
}

static size_t append(char *out, size_t size, size_t used, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t append(char *out, size_t size, size_t used, const char *fmt, ...)
{
    if (used >= size) return used;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + used, size - used, fmt, args);
    va_end(args);
    return n < 0 ? used : used + (size_t)n;
}

size_t device_info_json(char *out, size_t size)
{
    device_state_t s;
    device_snapshot(&s);
    size_t n = append(out, size, 0,
                      "{\"api\":1,\"host\":\"esp32\",\"log\":true,\"connected\":%s,\"screen\":{\"width\":%u,\"height\":%u},\"keys\":[",
                      s.connected ? "true" : "false", s.screen_width ? s.screen_width : 1280,
                      s.screen_height ? s.screen_height : 720);
    for (int i = 0; i < s.key_count; i++) {
        n = append(out, size, n, "%s{\"index\":%u,\"code\":%u,\"x\":%u,\"y\":%u,\"width\":%u,\"height\":%u}", i ? "," : "",
                   s.keys[i].index, s.keys[i].code, s.keys[i].x, s.keys[i].y, s.keys[i].width, s.keys[i].height);
    }
    char *values = device_values_json(), storage[72] = "";
    size_t total, used;
    if (config_storage(&total, &used)) snprintf(storage, sizeof storage, "\"storage\":{\"total\":%u,\"used\":%u},", (unsigned)total, (unsigned)used);
    n = append(out, size, n,
               "],%s\"actions\":[\"mqtt\",\"bl_down\",\"bl_up\",\"disp_off\",\"disp_on\",\"page\",\"back\"],"
               "\"brightness\":{\"level\":%d,\"percent\":%d,\"display_on\":%s},\"page\":\"%s\",\"values\":%s,"
               "\"variable_types\":[\"mqtt\"]}",
               storage, s.brightness, percent(s.brightness), s.display_on ? "true" : "false", s.page, values ? values : "{}");
    free(values);
    return n;
}

char *device_values_json(void)
{
    cJSON *config = config_parse();
    char *json = placeholders_values_json(config ? cJSON_GetObjectItem(config, "variables") : NULL);
    cJSON_Delete(config);
    return json;
}

void device_event_json(const char *name, char *out, size_t size)
{
    device_state_t s;
    device_snapshot(&s);
    if (strcmp(name, "status") == 0) snprintf(out, size, "{\"connected\":%s}", s.connected ? "true" : "false");
    else if (strcmp(name, "brightness") == 0)
        snprintf(out, size, "{\"level\":%d,\"percent\":%d,\"display_on\":%s}", s.brightness, percent(s.brightness),
                 s.display_on ? "true" : "false");
    else if (strcmp(name, "page") == 0) snprintf(out, size, "{\"page\":\"%s\"}", s.page);
    else snprintf(out, size, "{}");
}

void device_set_backlight(int level, bool display_on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.brightness = level;
    s_state.display_on = display_on;
    xSemaphoreGive(s_lock);
    s_brightness_dirty = true;
    char json[96];
    device_event_json("brightness", json, sizeof json);
    events_publish("brightness", json);
}

void device_set_page(const char *id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_state.page, sizeof s_state.page, "%s", id);
    xSemaphoreGive(s_lock);
    char json[64];
    device_event_json("page", json, sizeof json);
    events_publish("page", json);
    device_request_redraw(-1);
}

static void set_connected(bool connected)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.connected = connected;
    xSemaphoreGive(s_lock);
    events_publish("status", connected ? "{\"connected\":true}" : "{\"connected\":false}");
}

static uint16_t u16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static void publish_key(uint16_t index, bool pressed)
{
    char json[64];
    snprintf(json, sizeof json, "{\"index\":%u,\"pressed\":%s}", index, pressed ? "true" : "false");
    events_publish("key", json);
    if (pressed) {
        device_state_t state;
        device_snapshot(&state);
        actions_run(index, state.page, false); /* only sets state and flags: the device task acts on them */
    }
}

/* INFO payload: u16 version, u16 width, u16 height, u8 count, count * (6 x u16). */
static esp_err_t read_info(const uint8_t *p, size_t n)
{
    if (n < 7) return ESP_ERR_INVALID_SIZE;
    uint8_t count = p[6];
    if (count > DEVICE_MAX_KEYS || n < 7 + (size_t)count * 12) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.screen_width = u16(p + 2);
    s_state.screen_height = u16(p + 4);
    s_state.key_count = count;
    for (int i = 0; i < count; i++) {
        const uint8_t *k = p + 7 + i * 12;
        s_state.keys[i] = (device_key_t){u16(k), u16(k + 2), u16(k + 4), u16(k + 6), u16(k + 8), u16(k + 10)};
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}


esp_err_t device_run(adb_t *a)
{
    deckd_t *d = malloc(sizeof *d);
    if (!d) return ESP_ERR_NO_MEM;
    uint8_t type, frame[DECKD_REPLY_MAX];
    size_t n;
    esp_err_t err = deckd_open(d, a);
    if (err == ESP_OK) err = deckd_request(d, DECKD_HELLO, NULL, 0, &type, frame, sizeof frame, &n);
    if (err == ESP_OK && (type != DECKD_INFO || read_info(frame, n) != ESP_OK)) err = ESP_FAIL;
    s_brightness_dirty = true; /* sent in the loop below: a fresh connection starts from the current state */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hello failed: %s", esp_err_to_name(err));
        free(d);
        return err;
    }
    ESP_LOGI(TAG, "connected: %ux%u, %u keys", s_state.screen_width, s_state.screen_height, s_state.key_count);
    d->on_key = publish_key;
    set_connected(true);
    adb_set_trace(false); /* the ping loop would flood the log */
    /* No long wait for events: the IDF USB host does not enforce transfer timeouts, and one IN transfer
     * pending for seconds failed with a WiFi running. A ping every PING_MS keeps every transfer short; key
     * events arrive between the replies. */
    device_request_redraw(-1); /* a fresh connection: the screen still shows whatever was there before */
    long last_minute = -1;
    while (usb_adb_device_present()) {
        struct tm now;
        if (wallclock_now(&now) && time(NULL) / 60 != last_minute) { /* the clock moved on, or was just set */
            last_minute = time(NULL) / 60;
            drawdyn_request_all();
        }
        portENTER_CRITICAL(&s_redraw_lock);
        bool all = s_redraw_all;
        uint32_t keys = s_redraw_keys;
        s_redraw_all = false;
        s_redraw_keys = 0;
        portEXIT_CRITICAL(&s_redraw_lock);
        if (s_brightness_dirty) {
            s_brightness_dirty = false;
            device_state_t state;
            device_snapshot(&state);
            const uint8_t level = state.display_on ? state.brightness : 0;
            err = deckd_request(d, DECKD_SET_BRIGHTNESS, &level, 1, &type, frame, sizeof frame, &n);
            if (err != ESP_OK) break;
        }
        if (all || keys) {
            device_state_t state;
            device_snapshot(&state);
            err = draw_page(d, &state, all ? 0xFFFFFFFFu : keys, all);
            if (err != ESP_OK) break;
        }
        err = deckd_request(d, DECKD_PING, NULL, 0, &type, frame, sizeof frame, &n);
        if (err != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(PING_MS));
    }
    adb_set_trace(true);
    set_connected(false);
    free(d);
    return err;
}

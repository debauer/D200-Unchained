#include "device.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "deckd.h"
#include "esp_log.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb_adb.h"

static const char *TAG = "device";
#define START_BRIGHTNESS 200
#define PING_MS 100

static device_state_t s_state = {.brightness = START_BRIGHTNESS, .display_on = true, .page = "root"};
static SemaphoreHandle_t s_lock;
static volatile bool s_redraw = true;

void device_request_redraw(void)
{
    s_redraw = true;
}

void device_init(void)
{
    s_lock = xSemaphoreCreateMutex();
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
    n = append(out, size, n,
               "],\"actions\":[\"bl_down\",\"bl_up\",\"disp_off\",\"disp_on\",\"page\",\"back\"],"
               "\"brightness\":{\"level\":%d,\"percent\":%d,\"display_on\":%s},\"page\":\"%s\",\"values\":{},"
               "\"variable_types\":[]}",
               s.brightness, percent(s.brightness), s.display_on ? "true" : "false", s.page);
    return n;
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

static void log_refusal(const char *what, uint8_t type, const uint8_t *reply, size_t n)
{
    ESP_LOGE(TAG, "deckd answered %s with 0x%02x: %.*s", what, type, (int)(type == DECKD_ERROR ? n : 0), reply);
}

/* "rrggbb" -> r, g, b; false when `text` is not a colour. */
static bool parse_color(const cJSON *item, uint8_t rgb[3])
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) != 6) return false;
    char *end;
    unsigned long v = strtoul(item->valuestring, &end, 16);
    if (*end) return false;
    rgb[0] = v >> 16;
    rgb[1] = v >> 8;
    rgb[2] = v;
    return true;
}

static const cJSON *page_by_id(const cJSON *config, const char *id)
{
    const cJSON *page;
    cJSON_ArrayForEach(page, cJSON_GetObjectItem(config, "pages")) {
        const cJSON *pid = cJSON_GetObjectItem(page, "id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, id) == 0) return page;
    }
    return NULL;
}

static const cJSON *key_by_index(const cJSON *page, unsigned index)
{
    const cJSON *key;
    cJSON_ArrayForEach(key, cJSON_GetObjectItem(page, "keys")) {
        const cJSON *i = cJSON_GetObjectItem(key, "index");
        if (cJSON_IsNumber(i) && (unsigned)i->valueint == index) return key;
    }
    return NULL;
}

/* Clears the screen and fills every key with its colour: the key's own, else the page's, else `background`.
 * Text and images follow with the renderer. */
static esp_err_t draw_colors(deckd_t *d, const device_state_t *state)
{
    size_t length;
    char *text = config_load(&length);
    cJSON *config = text ? cJSON_Parse(text) : NULL;
    free(text);
    uint8_t background[3] = {0, 0, 0}, color[3];
    if (config) parse_color(cJSON_GetObjectItem(config, "background"), background);
    const cJSON *page = config ? page_by_id(config, state->page) : NULL;
    uint8_t type, reply[DECKD_REPLY_MAX];
    size_t n;
    esp_err_t err = deckd_request(d, DECKD_CLEAR, background, 3, &type, reply, sizeof reply, &n);
    if (err == ESP_OK && type != DECKD_OK) log_refusal("clear", type, reply, n);
    for (int i = 0; err == ESP_OK && i < state->key_count; i++) {
        memcpy(color, background, 3);
        if (page) parse_color(cJSON_GetObjectItem(page, "color"), color);
        const cJSON *key = page ? key_by_index(page, state->keys[i].index) : NULL;
        if (key) parse_color(cJSON_GetObjectItem(key, "color"), color);
        uint8_t payload[5] = {state->keys[i].index, state->keys[i].index >> 8, color[0], color[1], color[2]};
        err = deckd_request(d, DECKD_FILL_KEY, payload, sizeof payload, &type, reply, sizeof reply, &n);
        if (err == ESP_OK && type != DECKD_OK) log_refusal("fill key", type, reply, n);
    }
    ESP_LOGI(TAG, "drew %d key colours", state->key_count);
    cJSON_Delete(config);
    return err;
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
    if (err == ESP_OK) {
        const uint8_t level = START_BRIGHTNESS;
        err = deckd_request(d, DECKD_SET_BRIGHTNESS, &level, 1, &type, frame, sizeof frame, &n);
    }
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
    s_redraw = true; /* a fresh connection: the screen still shows whatever was there before */
    while (usb_adb_device_present()) {
        if (s_redraw) {
            s_redraw = false;
            device_state_t state;
            device_snapshot(&state);
            err = draw_colors(d, &state);
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

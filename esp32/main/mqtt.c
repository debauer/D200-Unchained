#include "mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "drawdyn.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "mqttsettings.h"

static const char *TAG = "mqtt";
#define MAX_VARS 16
#define NAME_SIZE 33
#define TOPIC_SIZE 129
#define PATH_SIZE 65

typedef struct {
    char name[NAME_SIZE], topic[TOPIC_SIZE], path[PATH_SIZE], value[MQTT_VALUE_SIZE];
    bool has_value;
} variable_t;

static SemaphoreHandle_t s_lock;
static mqtt_settings_t s_settings;
static esp_mqtt_client_handle_t s_client;
static variable_t s_vars[MAX_VARS];
static int s_var_count;
static bool s_connected;
static char s_error[64];

/* --- state toward the web UI ---------------------------------------------------------------------------- */

char *mqtt_json(void)
{
    cJSON *o = mqtt_settings_public(&s_settings);
    if (!o) return NULL;
    cJSON_AddBoolToObject(o, "enabled", s_settings.host[0] != 0);
    cJSON_AddBoolToObject(o, "connected", s_connected);
    cJSON_AddStringToObject(o, "error", s_error);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

static void publish_state(void)
{
    char *json = mqtt_json();
    if (json) events_publish("mqtt", json);
    free(json);
}

static void values_changed(void)
{
    char *json = device_values_json();
    if (json) events_publish("values", json);
    free(json);
    drawdyn_request_all();
}

bool mqtt_value(const char *name, char *out, size_t size)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_var_count && !found; i++) {
        if (strcmp(s_vars[i].name, name) == 0 && s_vars[i].has_value) {
            snprintf(out, size, "%s", s_vars[i].value);
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

/* --- incoming messages ---------------------------------------------------------------------------------- */

static void on_message(const char *topic, size_t topic_len, const char *data, size_t length)
{
    char *text = malloc(length + 1);
    if (!text) return;
    memcpy(text, data, length);
    text[length] = 0;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_var_count; i++) {
        variable_t *v = &s_vars[i];
        if (strlen(v->topic) != topic_len || memcmp(v->topic, topic, topic_len) != 0) continue;
        char value[MQTT_VALUE_SIZE];
        if (v->path[0]) mqtt_json_value(text, v->path, value, sizeof value);
        else mqtt_plain_value(text, length, value, sizeof value);
        if (!v->has_value || strcmp(v->value, value) != 0) {
            strcpy(v->value, value);
            v->has_value = changed = true;
        }
    }
    xSemaphoreGive(s_lock);
    free(text);
    if (changed) values_changed();
}

static void subscribe_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_var_count; i++) esp_mqtt_client_subscribe(s_client, s_vars[i].topic, 0);
    xSemaphoreGive(s_lock);
}

static const char *refusal(int code)
{
    static const char *const reasons[] = {"", "unacceptable protocol version", "client id rejected", "broker unavailable",
                                          "bad user name or password", "not authorized"};
    return code > 0 && code < 6 ? reasons[code] : "connection refused";
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        s_connected = true;
        s_error[0] = 0;
        ESP_LOGI(TAG, "connected to %s", s_settings.host);
        subscribe_all(); /* also delivers retained messages again */
        publish_state();
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_connected = false;
        publish_state();
    } else if (id == MQTT_EVENT_ERROR && e->error_handle) {
        const char *why = e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED
                              ? refusal(e->error_handle->connect_return_code)
                              : "cannot reach the broker";
        if (strcmp(s_error, why) != 0) { /* the client retries: report a problem once */
            snprintf(s_error, sizeof s_error, "%s", why);
            ESP_LOGW(TAG, "%s", why);
            publish_state();
        }
    } else if (id == MQTT_EVENT_DATA) {
        if (e->current_data_offset == 0 && e->data_len == e->total_data_len) on_message(e->topic, e->topic_len, e->data, e->data_len);
        else if (e->current_data_offset == 0) ESP_LOGW(TAG, "message of %d bytes is too long, ignored", e->total_data_len);
    }
}

/* --- connection ----------------------------------------------------------------------------------------- */

static void disconnect(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    s_error[0] = 0;
}

static void connect(void)
{
    if (!s_settings.host[0]) return;
    const esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = s_settings.host,
        .broker.address.port = (uint32_t)s_settings.port,
        .broker.address.transport = s_settings.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
        .broker.verification.crt_bundle_attach = s_settings.tls ? esp_crt_bundle_attach : NULL,
        .credentials.username = s_settings.username[0] ? s_settings.username : NULL,
        .credentials.client_id = s_settings.client_id[0] ? s_settings.client_id : NULL,
        .credentials.authentication.password = s_settings.username[0] ? s_settings.password : NULL,
        .buffer.size = 2048,
        .network.reconnect_timeout_ms = 5000,
    };
    s_client = esp_mqtt_client_init(&cfg); /* copies the strings */
    if (!s_client) {
        snprintf(s_error, sizeof s_error, "out of memory");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) snprintf(s_error, sizeof s_error, "cannot start the client");
}

void mqtt_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

void mqtt_start(void)
{
    mqtt_settings_load(&s_settings);
    connect();
}

const char *mqtt_update(const char *body)
{
    mqtt_settings_t next = s_settings;
    const char *problem = mqtt_settings_apply(&next, body);
    if (problem) return problem;
    if (!mqtt_settings_save(&next)) return "cannot store the settings";
    disconnect();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings = next;
    for (int i = 0; i < s_var_count; i++) s_vars[i].has_value = false; /* values of the old broker */
    xSemaphoreGive(s_lock);
    connect();
    publish_state();
    values_changed();
    return NULL;
}

/* --- variables -------------------------------------------------------------------------------------------- */

static void copy_field(char *out, size_t size, const cJSON *v, const char *field)
{
    const cJSON *item = cJSON_GetObjectItem(v, field);
    snprintf(out, size, "%s", cJSON_IsString(item) ? item->valuestring : "");
}

void mqtt_configure(const cJSON *config)
{
    variable_t next[MAX_VARS];
    int count = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(config, "variables")) {
        const cJSON *type = cJSON_GetObjectItem(v, "type");
        if (count == MAX_VARS || !cJSON_IsString(type) || strcmp(type->valuestring, "mqtt") != 0) continue;
        variable_t *n = &next[count];
        memset(n, 0, sizeof *n);
        copy_field(n->name, sizeof n->name, v, "name");
        copy_field(n->topic, sizeof n->topic, v, "topic");
        copy_field(n->path, sizeof n->path, v, "json_path");
        if (n->topic[0]) count++;
    }
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_var_count; i++) { /* topics nobody reads any more */
        bool used = false;
        for (int j = 0; j < count; j++) used = used || strcmp(next[j].topic, s_vars[i].topic) == 0;
        if (!used && s_connected) esp_mqtt_client_unsubscribe(s_client, s_vars[i].topic);
        changed = changed || (!used && s_vars[i].has_value);
    }
    for (int j = 0; j < count; j++) {
        bool known = false;
        for (int i = 0; i < s_var_count; i++) {
            if (strcmp(s_vars[i].name, next[j].name) != 0 || strcmp(s_vars[i].topic, next[j].topic) != 0) continue;
            known = true; /* same variable: keeps its value, a new JSON path only applies to later messages */
            strcpy(next[j].value, s_vars[i].value);
            next[j].has_value = s_vars[i].has_value;
        }
        bool subscribed = false;
        for (int i = 0; i < s_var_count; i++) subscribed = subscribed || strcmp(next[j].topic, s_vars[i].topic) == 0;
        if (s_connected && !subscribed) esp_mqtt_client_subscribe(s_client, next[j].topic, 0);
        changed = changed || !known;
    }
    memcpy(s_vars, next, (size_t)count * sizeof next[0]);
    s_var_count = count;
    xSemaphoreGive(s_lock);
    if (changed) values_changed();
}

const char *mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!topic[0]) return "topic not set";
    if (!s_client || !s_connected) return s_settings.host[0] ? "mqtt is not connected" : "mqtt is not configured";
    return esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, retain, true) < 0 ? "mqtt publish failed" : NULL;
}

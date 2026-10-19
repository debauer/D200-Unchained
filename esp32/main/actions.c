#include "actions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "device.h"
#include "esp_log.h"
#include "events.h"

static const char *TAG = "actions";
#define STEP 51 /* 20 % of 255, as on the Linux host */

static void report(unsigned index, const char *type, const char *detail)
{
    cJSON *event = cJSON_CreateObject();
    if (!event) return;
    cJSON_AddNumberToObject(event, "index", index);
    cJSON_AddStringToObject(event, "type", type);
    cJSON_AddStringToObject(event, "detail", detail);
    char *text = cJSON_PrintUnformatted(event);
    if (text) events_publish("action", text);
    free(text);
    cJSON_Delete(event);
    ESP_LOGI(TAG, "key %u: %s %s", index, type, detail);
}

static const char *page_name(const cJSON *config, const char *id, char *out, size_t size)
{
    const cJSON *page = config_find_page(config, id);
    const cJSON *name = page ? cJSON_GetObjectItem(page, "name") : NULL;
    snprintf(out, size, "%s", cJSON_IsString(name) ? name->valuestring : id);
    return out;
}

/* Returns the log line; a leading '!' means nothing happened. */
static void navigate(const cJSON *config, const char *type, const cJSON *action, char *detail, size_t size)
{
    char name[48];
    if (strcmp(type, "page") == 0) {
        const cJSON *target = cJSON_GetObjectItem(action, "target");
        if (!cJSON_IsString(target) || !config_find_page(config, target->valuestring)) {
            snprintf(detail, size, "! page not set");
            return;
        }
        device_set_page(target->valuestring);
        snprintf(detail, size, "-> %s", page_name(config, target->valuestring, name, sizeof name));
        return;
    }
    device_state_t state;
    device_snapshot(&state);
    const cJSON *current = config_find_page(config, state.page);
    const cJSON *parent = current ? cJSON_GetObjectItem(current, "parent") : NULL;
    if (!cJSON_IsString(parent) || !config_find_page(config, parent->valuestring)) {
        snprintf(detail, size, "! already on the top page");
        return;
    }
    device_set_page(parent->valuestring);
    snprintf(detail, size, "<- %s", page_name(config, parent->valuestring, name, sizeof name));
}

static void backlight(const char *type, char *detail, size_t size)
{
    device_state_t s;
    device_snapshot(&s);
    int level = s.brightness;
    bool on = s.display_on;
    if (strcmp(type, "bl_up") == 0) level = level + STEP > 255 ? 255 : level + STEP;
    else if (strcmp(type, "bl_down") == 0) level = level - STEP < 0 ? 0 : level - STEP;
    else on = strcmp(type, "disp_on") == 0;
    device_set_backlight(level, on);
    if (on) snprintf(detail, size, "backlight %d%%", (level * 100 + 127) / 255);
    else snprintf(detail, size, "display off");
}

bool actions_run(unsigned index, const char *page)
{
    cJSON *config = config_parse();
    const cJSON *p = config ? config_find_page(config, page) : NULL;
    const cJSON *key = p ? config_find_key(p, index) : NULL;
    const cJSON *action = key ? cJSON_GetObjectItem(key, "action") : NULL;
    const cJSON *type = action ? cJSON_GetObjectItem(action, "type") : NULL;
    bool ran = false;
    if (cJSON_IsString(type)) {
        char detail[96];
        const char *t = type->valuestring;
        if (strcmp(t, "page") == 0 || strcmp(t, "back") == 0) {
            navigate(config, t, action, detail, sizeof detail);
            ran = detail[0] != '!';
        } else if (strcmp(t, "bl_up") == 0 || strcmp(t, "bl_down") == 0 || strcmp(t, "disp_on") == 0 ||
                   strcmp(t, "disp_off") == 0) {
            backlight(t, detail, sizeof detail);
            ran = true;
        } else {
            snprintf(detail, sizeof detail, "! %s not supported on this host", t);
            ESP_LOGW(TAG, "key %u: action type \"%s\" is not supported on this host, ignored", index, t);
        }
        report(index, t, detail);
    }
    cJSON_Delete(config);
    return ran;
}

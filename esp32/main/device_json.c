#include "device.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "placeholders.h"

static int percent(int level) { return (level * 100 + 127) / 255; }

static size_t append(char *out, size_t size, size_t used, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t append(char *out, size_t size, size_t used, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(used < size ? out + used : NULL, used < size ? size - used : 0, fmt, args);
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
    if (!values) return SIZE_MAX;
    size_t total, used;
    if (config_storage(&total, &used)) snprintf(storage, sizeof storage, "\"storage\":{\"total\":%u,\"used\":%u},", (unsigned)total, (unsigned)used);
    n = append(out, size, n,
               "],%s\"actions\":[\"mqtt\",\"bl_down\",\"bl_up\",\"disp_off\",\"disp_on\",\"page\",\"back\"],"
               "\"brightness\":{\"level\":%d,\"percent\":%d,\"display_on\":%s},\"page\":\"%s\",\"values\":%s,"
               "\"variable_types\":[\"mqtt\"]}",
               storage, s.brightness, percent(s.brightness), s.display_on ? "true" : "false", s.page, values);
    free(values);
    return n;
}

char *device_values_json(void)
{
    cJSON *config = config_parse();
    char *json = config ? placeholders_values_json(cJSON_GetObjectItem(config, "variables")) : NULL;
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

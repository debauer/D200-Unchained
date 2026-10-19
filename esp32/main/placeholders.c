#include "placeholders.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "mqtt.h"
#include "wallclock.h"

#define NAME_MAX_LEN 32

/* Length of `{name}` at s (including the braces), 0 if it is not a placeholder. */
static size_t token_length(const char *s, char *name)
{
    if (s[0] != '{' || !islower((unsigned char)s[1])) return 0;
    size_t n = 0;
    while (n < NAME_MAX_LEN && (islower((unsigned char)s[1 + n]) || isdigit((unsigned char)s[1 + n]) || s[1 + n] == '_')) {
        name[n] = s[1 + n];
        n++;
    }
    if (s[1 + n] != '}') return 0;
    name[n] = 0;
    return n + 2;
}

bool placeholders_present(const char *text)
{
    char name[NAME_MAX_LEN + 1];
    for (const char *p = text; *p; p++) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            p++;
            continue;
        }
        if (token_length(p, name)) return true;
    }
    return false;
}

static const cJSON *find_variable(const cJSON *variables, const char *name)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, variables) {
        const cJSON *n = cJSON_GetObjectItem(v, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) return v;
    }
    return NULL;
}

static void builtin(const char *name, char *out, size_t size)
{
    static const char *const days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    struct tm t;
    bool have = wallclock_now(&t);
    if (strcmp(name, "time") == 0) {
        if (have) snprintf(out, size, "%02d:%02d", t.tm_hour, t.tm_min);
        else snprintf(out, size, "--:--");
    } else if (have) {
        snprintf(out, size, "%s %02d.%02d.", days[(t.tm_wday + 6) % 7], t.tm_mday, t.tm_mon + 1);
    } else {
        snprintf(out, size, "---");
    }
}

placeholder_t placeholders_value(const cJSON *variables, const char *name, char *out, size_t size)
{
    if (strcmp(name, "time") == 0 || strcmp(name, "date") == 0) {
        builtin(name, out, size);
        return PLACEHOLDER_VALUE;
    }
    const cJSON *v = find_variable(variables, name);
    if (!v) return PLACEHOLDER_UNKNOWN;
    const cJSON *type = cJSON_GetObjectItem(v, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "mqtt") != 0) { /* no source for this type here */
        snprintf(out, size, "?");
        return PLACEHOLDER_VALUE;
    }
    if (mqtt_value(name, out, size)) return PLACEHOLDER_VALUE;
    snprintf(out, size, "...");
    return PLACEHOLDER_PENDING;
}

char *placeholders_values_json(const cJSON *variables)
{
    cJSON *values = cJSON_CreateObject();
    char value[MQTT_VALUE_SIZE];
    static const char *const builtins[] = {"time", "date"};
    for (size_t i = 0; values && i < 2; i++) {
        placeholders_value(variables, builtins[i], value, sizeof value);
        cJSON_AddStringToObject(values, builtins[i], value);
    }
    const cJSON *v;
    cJSON_ArrayForEach(v, variables) {
        const cJSON *name = cJSON_GetObjectItem(v, "name");
        if (values && cJSON_IsString(name) && placeholders_value(variables, name->valuestring, value, sizeof value) == PLACEHOLDER_VALUE) {
            cJSON_AddStringToObject(values, name->valuestring, value);
        }
    }
    char *text = values ? cJSON_PrintUnformatted(values) : NULL;
    cJSON_Delete(values);
    return text;
}

void placeholders_fill(const char *text, const cJSON *variables, char *out, size_t size)
{
    size_t used = 0;
    char name[NAME_MAX_LEN + 1], value[MQTT_VALUE_SIZE];
    for (const char *p = text; *p && used + 1 < size;) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            out[used++] = p[0];
            p += 2;
            continue;
        }
        size_t n = token_length(p, name);
        if (n) {
            if (placeholders_value(variables, name, value, sizeof value) == PLACEHOLDER_UNKNOWN)
                n = 0; /* unknown: stays as written, so a typo is visible */
            if (n) {
                size_t len = strlen(value);
                if (used + len >= size) break;
                memcpy(out + used, value, len);
                used += len;
                p += n;
                continue;
            }
        }
        out[used++] = *p++;
    }
    out[used] = 0;
}

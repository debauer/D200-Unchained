#include "mqttjson.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* Copies `text` cut to MQTT_VALUE_MAX bytes and to the buffer, without splitting a UTF-8 character. */
static void copy_cut(char *out, size_t size, const char *text, size_t length)
{
    size_t n = length < MQTT_VALUE_MAX ? length : MQTT_VALUE_MAX;
    if (n >= size) n = size - 1;
    while (n > 0 && n < length && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    memcpy(out, text, n);
    out[n] = 0;
}

void mqtt_plain_value(const char *payload, size_t length, char *out, size_t size)
{
    while (length > 0 && isspace((unsigned char)*payload)) {
        payload++;
        length--;
    }
    while (length > 0 && isspace((unsigned char)payload[length - 1])) length--;
    copy_cut(out, size, payload, length);
}

/* The next part of a dotted path in `part`; false when the path is used up. */
static bool next_part(const char **path, char *part, size_t size)
{
    if (!**path) return false;
    const char *dot = strchr(*path, '.');
    size_t n = dot ? (size_t)(dot - *path) : strlen(*path);
    if (n >= size) n = size - 1;
    memcpy(part, *path, n);
    part[n] = 0;
    *path = dot ? dot + 1 : *path + strlen(*path);
    return true;
}

static const cJSON *step(const cJSON *node, const char *part)
{
    if (cJSON_IsArray(node)) {
        char *end;
        long i = strtol(part, &end, 10);
        return *part && !*end && i >= 0 ? cJSON_GetArrayItem(node, (int)i) : NULL;
    }
    return cJSON_IsObject(node) ? cJSON_GetObjectItemCaseSensitive(node, part) : NULL;
}

void mqtt_json_value(const char *payload, const char *path, char *out, size_t size)
{
    cJSON *root = cJSON_Parse(payload);
    const cJSON *node = root;
    char part[48];
    while (node && next_part(&path, part, sizeof part)) node = step(node, part);
    char text[40];
    if (!node || cJSON_IsObject(node) || cJSON_IsArray(node)) snprintf(out, size, "%s", MQTT_ERROR_VALUE);
    else if (cJSON_IsBool(node)) snprintf(out, size, "%s", cJSON_IsTrue(node) ? "true" : "false");
    else if (cJSON_IsNull(node)) out[0] = 0;
    else if (cJSON_IsNumber(node)) {
        double d = node->valuedouble;
        if (d == (double)(long long)d && d > -1e15 && d < 1e15) snprintf(text, sizeof text, "%lld", (long long)d);
        else snprintf(text, sizeof text, "%.15g", d);
        copy_cut(out, size, text, strlen(text));
    } else {
        copy_cut(out, size, node->valuestring, strlen(node->valuestring));
    }
    cJSON_Delete(root);
}

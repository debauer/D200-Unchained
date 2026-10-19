#include "mqttsettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_PATH "/storage/mqtt.json"
#define TMP_PATH "/storage/mqtt.tmp"
#define DEFAULT_PORT 1883

static void defaults(mqtt_settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->port = DEFAULT_PORT;
}

static char *trimmed(char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n')) text[--n] = 0;
    return text;
}

/* Sets `out` from the string field `name` of `json` when it is there. NULL, or the error text. */
static const char *text_field(const cJSON *json, const char *name, char *out, size_t size, bool trim)
{
    const cJSON *item = cJSON_GetObjectItem(json, name);
    if (!item) return NULL;
    if (!cJSON_IsString(item)) return "mqtt: settings must be strings";
    char copy[256];
    if (strlen(item->valuestring) >= sizeof copy) return "mqtt: a value is too long";
    strcpy(copy, item->valuestring);
    const char *value = trim ? trimmed(copy) : copy;
    if (strlen(value) >= size) return "mqtt: a value is too long";
    strcpy(out, value);
    return NULL;
}

const char *mqtt_settings_apply(mqtt_settings_t *s, const char *body)
{
    cJSON *json = cJSON_Parse(body);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return "mqtt settings must be a JSON object";
    }
    mqtt_settings_t next = *s;
    const char *problem = NULL;
    const cJSON *port = cJSON_GetObjectItem(json, "port"), *tls = cJSON_GetObjectItem(json, "tls");
    const cJSON *password = cJSON_GetObjectItem(json, "password");
    if (port && (!cJSON_IsNumber(port) || port->valuedouble != port->valueint || port->valueint < 1 || port->valueint > 65535))
        problem = "mqtt: port must be 1-65535";
    else if (tls && !cJSON_IsBool(tls)) problem = "mqtt: tls must be true or false";
    else if (cJSON_IsNull(password)) next.password[0] = 0;
    if (!problem && port) next.port = port->valueint;
    if (!problem && tls) next.tls = cJSON_IsTrue(tls);
    if (!problem) problem = text_field(json, "host", next.host, sizeof next.host, true);
    if (!problem) problem = text_field(json, "username", next.username, sizeof next.username, true);
    if (!problem) problem = text_field(json, "client_id", next.client_id, sizeof next.client_id, true);
    if (!problem && !cJSON_IsNull(password)) problem = text_field(json, "password", next.password, sizeof next.password, false);
    cJSON_Delete(json);
    if (!problem) *s = next;
    return problem;
}

cJSON *mqtt_settings_public(const mqtt_settings_t *s)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "host", s->host);
    cJSON_AddNumberToObject(o, "port", s->port);
    cJSON_AddStringToObject(o, "username", s->username);
    cJSON_AddBoolToObject(o, "tls", s->tls);
    cJSON_AddStringToObject(o, "client_id", s->client_id);
    cJSON_AddBoolToObject(o, "password_set", s->password[0] != 0);
    return o;
}

void mqtt_settings_load(mqtt_settings_t *s)
{
    defaults(s);
    FILE *f = fopen(FILE_PATH, "rb");
    if (!f) return;
    char text[1024];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    mqtt_settings_t loaded = *s;
    if (mqtt_settings_apply(&loaded, text) == NULL) *s = loaded;
}

bool mqtt_settings_save(const mqtt_settings_t *s)
{
    cJSON *o = mqtt_settings_public(s);
    if (!o) return false;
    cJSON_DeleteItemFromObject(o, "password_set");
    cJSON_AddStringToObject(o, "password", s->password);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    FILE *f = text ? fopen(TMP_PATH, "wb") : NULL;
    size_t length = text ? strlen(text) : 0;
    bool ok = f && fwrite(text, 1, length, f) == length;
    if (f) ok = fclose(f) == 0 && ok;
    ok = ok && rename(TMP_PATH, FILE_PATH) == 0;
    free(text);
    return ok;
}

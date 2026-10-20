#include "config_validate.h"

#include <stdbool.h>
#include <string.h>

static const cJSON *field(const cJSON *o, const char *name) { return cJSON_GetObjectItemCaseSensitive(o, name); }
static bool is_text(const cJSON *v) { return !v || cJSON_IsString(v); }
static bool integer(const cJSON *v, int low, int high)
{
    return cJSON_IsNumber(v) && v->valuedouble == v->valueint && v->valueint >= low && v->valueint <= high;
}

const char *config_validate_colors(const cJSON *object)
{
    static const char *const names[] = {"background", "color", "label_color", "accent_color"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        const cJSON *v = field(object, names[i]);
        if (!v) continue;
        if (!cJSON_IsString(v) || strlen(v->valuestring) != 6) return "colour must be six hexadecimal digits";
        for (const char *p = v->valuestring; *p; p++) {
            if (!strchr("0123456789abcdefABCDEF", *p)) return "invalid colour";
        }
    }
    return NULL;
}

static bool variable_name(const char *s)
{
    if (!(*s >= 'a' && *s <= 'z') || strlen(s) > 32 || !strcmp(s, "time") || !strcmp(s, "date")) return false;
    for (; *s; s++) if (!(*s >= 'a' && *s <= 'z') && !(*s >= '0' && *s <= '9') && *s != '_') return false;
    return true;
}

static bool string_list(const cJSON *list)
{
    if (!cJSON_IsArray(list) || !cJSON_GetArraySize(list)) return false;
    const cJSON *v;
    cJSON_ArrayForEach(v, list) if (!cJSON_IsString(v)) return false;
    return true;
}

static bool topic(const cJSON *v)
{
    return !v || (cJSON_IsString(v) && !strpbrk(v->valuestring, "+#"));
}

const char *config_validate_variables(cJSON *config)
{
    const cJSON *variables = field(config, "variables"), *v;
    if (!variables) return cJSON_AddArrayToObject(config, "variables") ? NULL : "out of memory";
    if (!cJSON_IsArray(variables)) return "variables must be a list";
    int mqtt_count = 0;
    cJSON_ArrayForEach(v, variables) {
        const cJSON *name = field(v, "name"), *type = field(v, "type"), *interval = field(v, "interval");
        if (!cJSON_IsObject(v) || !cJSON_IsString(name) || !variable_name(name->valuestring)) return "invalid variable name";
        for (const cJSON *prev = variables->child; prev != v; prev = prev->next) {
            if (!strcmp(field(prev, "name")->valuestring, name->valuestring)) return "duplicate variable name";
        }
        if (!type) type = cJSON_AddStringToObject((cJSON *)v, "type", "command");
        if (!cJSON_IsString(type) || !type->valuestring[0]) return "variable type must be a non-empty string";
        if (interval && !integer(interval, 1, 3600)) return "interval must be an integer 1-3600";
        if (!strcmp(type->valuestring, "mqtt")) {
            const cJSON *t = field(v, "topic"), *path = field(v, "json_path");
            if (++mqtt_count > 16) return "ESP32 supports at most 16 MQTT variables";
            if (!topic(t) || !is_text(path)) return "invalid MQTT topic or JSON path";
            if ((t && strlen(t->valuestring) > 128) || (path && strlen(path->valuestring) > 64)) return "MQTT topic/path too long";
        } else if (!strcmp(type->valuestring, "command")) {
            const cJSON *command = field(v, "command");
            if (command && (!cJSON_IsString(command) || !command->valuestring[0])) return "invalid variable command";
            if (!command && !string_list(field(v, "argv"))) return "command variable needs command or argv";
        }
    }
    return NULL;
}

static bool known_variable(const cJSON *config, const char *name)
{
    if (!strcmp(name, "time") || !strcmp(name, "date")) return true;
    const cJSON *v;
    cJSON_ArrayForEach(v, field(config, "variables")) if (!strcmp(field(v, "name")->valuestring, name)) return true;
    return false;
}

static const char *action_validate(const cJSON *a)
{
    if (!a || cJSON_IsNull(a)) return NULL;
    const cJSON *type = field(a, "type");
    if (!cJSON_IsObject(a) || !cJSON_IsString(type) || !type->valuestring[0]) return "action needs a type";
    if (!strcmp(type->valuestring, "command") && !string_list(field(a, "argv"))) return "action argv must be a non-empty string list";
    if (!strcmp(type->valuestring, "page") && !is_text(field(a, "target"))) return "page target must be a string";
    if (!strcmp(type->valuestring, "mqtt")) {
        const cJSON *qos = field(a, "qos"), *retain = field(a, "retain");
        if (!topic(field(a, "topic")) || !is_text(field(a, "payload"))) return "invalid MQTT action topic/payload";
        if ((qos && !integer(qos, 0, 2)) || (retain && !cJSON_IsBool(retain))) return "invalid MQTT QoS/retain";
    }
    return NULL;
}

const char *config_validate_keys(cJSON *config, cJSON *page)
{
    const cJSON *keys = field(page, "keys"), *key;
    if (!keys) return cJSON_AddArrayToObject(page, "keys") ? NULL : "out of memory";
    if (!cJSON_IsArray(keys)) return "keys must be a list";
    cJSON_ArrayForEach(key, keys) {
        const cJSON *index = field(key, "index"), *image = field(key, "image"), *size = field(key, "label_size");
        const cJSON *pos = field(key, "label_pos"), *rules = field(key, "rules");
        if (!cJSON_IsObject(key) || !integer(index, 0, 13)) return "key index must be an integer 0-13";
        for (const cJSON *prev = keys->child; prev != key; prev = prev->next) {
            if (field(prev, "index")->valueint == index->valueint) return "duplicate key index";
        }
        if (image && !cJSON_IsNull(image) && (!cJSON_IsString(image) || !image->valuestring[0] ||
            image->valuestring[0] == '.' || strpbrk(image->valuestring, "/\\") || strlen(image->valuestring) > 255))
            return "image must be a plain file name";
        if (!is_text(field(key, "label"))) return "label must be a string";
        if (size && !integer(size, 50, 250)) return "label_size must be an integer 50-250";
        if (pos && (!cJSON_IsString(pos) || (strcmp(pos->valuestring, "top") && strcmp(pos->valuestring, "center") &&
            strcmp(pos->valuestring, "bottom")))) return "invalid label position";
        const char *error = config_validate_colors(key);
        if (!error) error = action_validate(field(key, "action"));
        if (error) return error;
        if (rules && !cJSON_IsArray(rules)) return "rules must be a list";
        const cJSON *rule;
        cJSON_ArrayForEach(rule, rules) {
            const cJSON *variable = field(rule, "variable"), *value = field(rule, "value");
            if (!cJSON_IsObject(rule) || !cJSON_IsString(variable) || !cJSON_IsString(value)) return "invalid colour rule";
            if (!known_variable(config, variable->valuestring)) return "rule uses unknown variable";
            error = config_validate_colors(rule);
            if (error) return error;
        }
    }
    return NULL;
}

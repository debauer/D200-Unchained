#include "config_validate.h"

#include <stdbool.h>
#include <string.h>

static const cJSON *field(const cJSON *o, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(o, name);
}

static cJSON *page_by_id(const cJSON *config, const char *id)
{
    cJSON *page;
    cJSON_ArrayForEach(page, field(config, "pages")) {
        const cJSON *pid = field(page, "id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, id) == 0) return page;
    }
    return NULL;
}

static bool page_id(const cJSON *id)
{
    if (!cJSON_IsString(id)) return false;
    size_t n = strlen(id->valuestring);
    if (!n || n > 32) return false;
    for (const char *p = id->valuestring; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '_' && *p != '-') return false;
    }
    return true;
}

static const char *pages_validate(cJSON *config)
{
    cJSON *pages = cJSON_GetObjectItemCaseSensitive(config, "pages"), *page;
    if (!cJSON_IsArray(pages)) return "pages must be a list";
    cJSON_ArrayForEach(page, pages) {
        const cJSON *id = field(page, "id"), *parent = field(page, "parent"), *name = field(page, "name");
        if (!cJSON_IsObject(page) || !page_id(id)) return "page id must match [a-z0-9_-]{1,32}";
        if (page_by_id(config, id->valuestring) != page) return "duplicate page id";
        if (parent && !cJSON_IsNull(parent) && !cJSON_IsString(parent)) return "parent must be a page id or null";
        if (name && !cJSON_IsString(name)) return "page name must be a string";
        if (!parent && !cJSON_AddNullToObject(page, "parent")) return "out of memory";
        if (!name && !cJSON_AddStringToObject(page, "name", strcmp(id->valuestring, "root") ? id->valuestring : "Default"))
            return "out of memory";
        const char *error = config_validate_colors(page);
        if (!error) error = config_validate_keys(config, page);
        if (error) return error;
    }
    const cJSON *root = page_by_id(config, "root");
    if (!root || !cJSON_IsNull(field(root, "parent"))) return "a root page without parent is required";
    int count = cJSON_GetArraySize(pages);
    cJSON_ArrayForEach(page, pages) {
        const cJSON *current = page;
        int remaining = count;
        while (current != root) {
            const cJSON *parent = field(current, "parent");
            if (!cJSON_IsString(parent) || !(current = page_by_id(config, parent->valuestring))) return "unknown parent page";
            if (--remaining <= 0) return "page parent cycle";
        }
        const cJSON *key;
        cJSON_ArrayForEach(key, field(page, "keys")) {
            const cJSON *action = field(key, "action"), *type = field(action, "type"), *target = field(action, "target");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "page") == 0 && cJSON_IsString(target) &&
                target->valuestring[0] && !page_by_id(config, target->valuestring)) return "unknown target page";
        }
    }
    return NULL;
}

static bool integer(const cJSON *v, int low, int high)
{
    return cJSON_IsNumber(v) && v->valuedouble == v->valueint && v->valueint >= low && v->valueint <= high;
}

static const char *normalize(cJSON *config)
{
    if (!cJSON_IsObject(config)) return "config must be an object";
    const cJSON *version = field(config, "version");
    int v = version ? (integer(version, 1, 2) ? version->valueint : 0) : (field(config, "pages") ? 2 : 1);
    if (!v) return "unsupported config version";
    if (v == 1) {
        cJSON *pages = cJSON_CreateArray(), *page = cJSON_CreateObject();
        cJSON *keys = field(config, "keys") ? cJSON_Duplicate(field(config, "keys"), true) : cJSON_CreateArray();
        if (!pages || !page || !keys || !cJSON_AddStringToObject(page, "id", "root")) {
            cJSON_Delete(pages);
            cJSON_Delete(page);
            cJSON_Delete(keys);
            return "out of memory";
        }
        if (!cJSON_AddItemToObject(page, "keys", keys)) {
            cJSON_Delete(pages);
            cJSON_Delete(page);
            cJSON_Delete(keys);
            return "out of memory";
        }
        cJSON_AddItemToArray(pages, page);
        cJSON_DeleteItemFromObjectCaseSensitive(config, "pages");
        cJSON_DeleteItemFromObjectCaseSensitive(config, "keys");
        if (!cJSON_AddItemToObject(config, "pages", pages)) {
            cJSON_Delete(pages);
            return "out of memory";
        }
    } else if (!field(config, "pages")) {
        cJSON *pages = cJSON_Parse("[{\"id\":\"root\"}]");
        if (!pages) return "out of memory";
        if (!cJSON_AddItemToObject(config, "pages", pages)) {
            cJSON_Delete(pages);
            return "out of memory";
        }
    }
    cJSON_DeleteItemFromObjectCaseSensitive(config, "version");
    if (!cJSON_AddNumberToObject(config, "version", 2)) return "out of memory";
    const cJSON *brightness = field(config, "brightness");
    if (brightness && !integer(brightness, 0, 255)) return "brightness must be an integer 0-255";
    if (!brightness && !cJSON_AddNumberToObject(config, "brightness", 255)) return "out of memory";
    if (!field(config, "background") && !cJSON_AddStringToObject(config, "background", "000000")) return "out of memory";
    const char *error = config_validate_colors(config);
    if (!error) error = config_validate_variables(config);
    if (!error) error = pages_validate(config);
    return error;
}

cJSON *config_validate(const char *text, const char **error)
{
    cJSON *config = cJSON_ParseWithOpts(text, NULL, true);
    *error = config ? normalize(config) : "invalid JSON";
    if (*error) {
        cJSON_Delete(config);
        return NULL;
    }
    return config;
}

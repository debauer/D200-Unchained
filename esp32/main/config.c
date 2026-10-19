#include "config.h"

#include <dirent.h>
#include "esp_littlefs.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define FILE_PATH "/storage/config.json"
#define TMP_PATH "/storage/config.tmp"
#define DEFAULT_CONFIG "{\"version\":2,\"pages\":[{\"id\":\"root\",\"name\":\"Default\",\"parent\":null,\"keys\":[]}]}"

char *config_load(size_t *length)
{
    FILE *f = fopen(FILE_PATH, "rb");
    long size = 0;
    if (f && fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (f && size > 0 && fseek(f, 0, SEEK_SET) == 0) {
        char *text = malloc((size_t)size + 1);
        if (text && fread(text, 1, (size_t)size, f) == (size_t)size) {
            fclose(f);
            text[size] = 0;
            if (length) *length = (size_t)size;
            return text;
        }
        free(text);
    }
    if (f) fclose(f);
    if (length) *length = strlen(DEFAULT_CONFIG);
    return strdup(DEFAULT_CONFIG);
}

bool config_store(const char *text, size_t length)
{
    FILE *f = fopen(TMP_PATH, "wb");
    return f && fwrite(text, 1, length, f) == length && fclose(f) == 0 && rename(TMP_PATH, FILE_PATH) == 0;
}

cJSON *config_parse(void)
{
    size_t length;
    char *text = config_load(&length);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    return root ? root : cJSON_Parse(DEFAULT_CONFIG);
}

bool config_save(const cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    bool ok = text && config_store(text, strlen(text));
    free(text);
    return ok;
}

cJSON *config_find_page(const cJSON *root, const char *id)
{
    cJSON *page;
    cJSON_ArrayForEach(page, cJSON_GetObjectItem(root, "pages")) {
        const cJSON *pid = cJSON_GetObjectItem(page, "id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, id) == 0) return page;
    }
    return NULL;
}

cJSON *config_find_key(const cJSON *page, unsigned index)
{
    cJSON *key;
    cJSON_ArrayForEach(key, cJSON_GetObjectItem(page, "keys")) {
        const cJSON *i = cJSON_GetObjectItem(key, "index");
        if (cJSON_IsNumber(i) && (unsigned)i->valueint == index) return key;
    }
    return NULL;
}

cJSON *config_ensure_key(cJSON *page, unsigned index)
{
    cJSON *key = config_find_key(page, index);
    if (key) return key;
    cJSON *keys = cJSON_GetObjectItem(page, "keys");
    if (!cJSON_IsArray(keys)) {
        keys = cJSON_AddArrayToObject(page, "keys");
        if (!keys) return NULL;
    }
    key = cJSON_CreateObject();
    if (!key) return NULL;
    cJSON_AddNumberToObject(key, "index", index);
    cJSON_AddItemToArray(keys, key);
    return key;
}

void config_image_name(char *out, size_t size, const char *page, unsigned index, const char *kind)
{
    if (strcmp(page, "root") == 0) snprintf(out, size, "key-%u%s.rgb565", index, kind);
    else snprintf(out, size, "%s-key-%u%s.rgb565", page, index, kind);
}

void config_purge_images(const cJSON *root)
{
    DIR *dir = opendir(IMAGE_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strncmp(name, "key-", 4) == 0 || name[0] == '.') continue; /* root page, or a temporary file */
        const char *marker = strstr(name, "-key-");
        if (!marker) continue;
        char page[40];
        size_t n = (size_t)(marker - name);
        if (n == 0 || n >= sizeof page) continue;
        memcpy(page, name, n);
        page[n] = 0;
        if (config_find_page(root, page)) continue;
        char path[sizeof IMAGE_DIR + 260];
        snprintf(path, sizeof path, IMAGE_DIR "/%s", name);
        unlink(path);
    }
    closedir(dir);
}

bool config_storage(size_t *total, size_t *used)
{
    return esp_littlefs_info("storage", total, used) == ESP_OK;
}

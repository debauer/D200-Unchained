#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device.h"
#include "config.h"

static bool failure;
void device_snapshot(device_state_t *out)
{
    *out = (device_state_t){.connected = true, .screen_width = 1280, .screen_height = 720,
                           .brightness = 120, .display_on = false, .page = "root", .key_count = 1,
                           .keys = {{0, 29, 11, 3, 196, 196}}};
}
cJSON *config_parse(void) { return failure ? NULL : cJSON_CreateObject(); }
bool config_storage(size_t *total, size_t *used) { (void)total; (void)used; return false; }
char *placeholders_values_json(const cJSON *variables)
{
    (void)variables;
    cJSON *values = cJSON_CreateObject();
    for (int i = 0; i < 200; i++) {
        char name[33];
        snprintf(name, sizeof name, "v%031d", i);
        cJSON_AddStringToObject(values, name, "?");
    }
    char *text = cJSON_PrintUnformatted(values);
    cJSON_Delete(values);
    return text;
}
int main(void)
{
    char small[64];
    size_t needed = device_info_json(small, sizeof small);
    assert(needed > 6144 && small[sizeof small - 1] == 0);
    assert(device_info_json(NULL, 0) == needed);
    char *text = malloc(needed + 1);
    assert(device_info_json(text, needed + 1) == needed);
    cJSON *json = cJSON_Parse(text);
    assert(json && cJSON_GetArraySize(cJSON_GetObjectItem(json, "values")) == 200);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "brightness"), "level")->valueint == 120);
    cJSON_Delete(json);
    free(text);
    failure = true;
    assert(device_info_json(small, sizeof small) == SIZE_MAX);
    puts("device json ok");
}

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "config_validate.h"

static bool fail_write;
static int closed;
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_fclose(FILE *);
size_t __wrap_fwrite(const void *p, size_t size, size_t count, FILE *f)
{
    return __real_fwrite(p, size, fail_write ? count / 2 : count, f);
}
int __wrap_fclose(FILE *f) { closed++; return __real_fclose(f); }

static void rejects(const char *text)
{
    const char *error = NULL;
    cJSON *config = config_validate(text, &error);
    assert(!config && error);
}

static cJSON *accepts(const char *text)
{
    const char *error = NULL;
    cJSON *config = config_validate(text, &error);
    if (!config) fprintf(stderr, "unexpected error: %s\n", error);
    assert(config && !error);
    return config;
}

static void file(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "images/%s", name);
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(!fclose(f));
}

int main(void)
{
    rejects("{\"version\":3}");
    rejects("{\"version\":2,\"pages\":[]}");
    rejects("{\"pages\":[{\"id\":\"root\",\"keys\":{}}]}");
    rejects("{\"pages\":[{\"id\":\"root\"},{\"id\":\"root\"}]}");
    rejects("{\"pages\":[{\"id\":\"root\",\"parent\":\"other\"}]}");
    rejects("{\"pages\":[{\"id\":\"root\"},{\"id\":\"../bad\",\"parent\":\"root\"}]}");
    rejects("{\"pages\":[{\"id\":\"root\"},{\"id\":\"a\",\"parent\":\"b\"},{\"id\":\"b\",\"parent\":\"a\"}]}");
    rejects("{\"keys\":[{\"index\":0},{\"index\":0}]}");
    rejects("{\"keys\":[{\"index\":0.5}]}");
    rejects("{\"keys\":[{\"index\":0,\"image\":\"../secret\"}]}");
    rejects("{\"keys\":[{\"index\":0,\"color\":\"zz0000\"}]}");
    rejects("{\"brightness\":256}");
    rejects("{\"keys\":[{\"index\":0,\"label_size\":999999}]}");
    rejects("{\"keys\":[{\"index\":0,\"action\":{\"type\":\"page\",\"target\":\"missing\"}}]}");
    rejects("{\"keys\":[{\"index\":0,\"rules\":[{\"variable\":\"missing\",\"value\":\"on\"}]}]}");
    rejects("{\"variables\":[{\"name\":\"time\",\"type\":\"mqtt\"}]}");
    rejects("{} trailing");
    cJSON *config = accepts("{\"version\":1,\"keys\":[{\"index\":0,\"action\":{\"type\":\"future\",\"data\":42}}]}");
    assert(cJSON_GetObjectItem(config, "version")->valueint == 2);
    cJSON *root = config_find_page(config, "root");
    assert(root && config_find_key(root, 0));
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(config_find_key(root, 0), "action"), "data")->valueint == 42);
    cJSON_Delete(config);
    config = accepts("{\"version\":2}");
    assert(config_find_page(config, "root"));
    assert(config_save(config));
    cJSON_Delete(config);

    config = accepts("{}");
    cJSON *variables = cJSON_GetObjectItem(config, "variables");
    for (int i = 0; i < 17; i++) {
        cJSON *v = cJSON_CreateObject();
        char name[16];
        snprintf(name, sizeof name, "v%d", i);
        cJSON_AddStringToObject(v, "name", name);
        cJSON_AddStringToObject(v, "type", "mqtt");
        cJSON_AddStringToObject(v, "topic", "test/state");
        cJSON_AddItemToArray(variables, v);
        char *text = cJSON_PrintUnformatted(config);
        if (i < 16) cJSON_Delete(accepts(text));
        else rejects(text);
        free(text);
    }
    cJSON_Delete(config);

    size_t length;
    char *before = config_load(&length);
    int previous = closed;
    fail_write = true;
    assert(!config_store("replacement", 11));
    fail_write = false;
    assert(closed == previous + 1);
    char *after = config_load(NULL);
    assert(before && after && !strcmp(before, after));
    assert(access("config.tmp", F_OK) != 0);
    free(before);
    free(after);
    assert(config_store("not JSON", 8));
    assert(!config_parse());
    assert(!unlink("config.json"));
    assert(!mkdir("config.json", 0700));
    assert(!config_load(NULL));
    assert(!rmdir("config.json"));
    config = config_parse();
    assert(config && config_find_page(config, "root"));
    cJSON_Delete(config);

    assert(!mkdir("images", 0700));
    file("key-0.rgb565");
    file("my-key-room-key-0-icon.rgb565");
    file("my-key-key-1-alt.rgb565");
    file("key-room-key-0.rgb565");
    file("removed-key-1.rgb565");
    file("shared-key-2.rgb565");
    config = accepts("{\"pages\":[{\"id\":\"root\",\"keys\":[{\"index\":0,\"image\":\"shared-key-2.rgb565\"}]},"
                     "{\"id\":\"my-key-room\",\"parent\":\"root\"},{\"id\":\"my-key\",\"parent\":\"root\"}]}");
    config_purge_images(config);
    assert(!access("images/key-0.rgb565", F_OK));
    assert(!access("images/my-key-room-key-0-icon.rgb565", F_OK));
    assert(!access("images/my-key-key-1-alt.rgb565", F_OK));
    assert(!access("images/shared-key-2.rgb565", F_OK));
    assert(access("images/key-room-key-0.rgb565", F_OK) != 0);
    assert(access("images/removed-key-1.rgb565", F_OK) != 0);
    cJSON_Delete(config);
    puts("config ok");
}

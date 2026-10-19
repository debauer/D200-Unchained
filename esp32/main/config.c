#include "config.h"

#include <stdio.h>
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

#pragma once
#include <stddef.h>
#define ESP_OK 0
static inline int esp_littlefs_info(const char *label, size_t *total, size_t *used)
{
    (void)label;
    (void)total;
    (void)used;
    return -1;
}

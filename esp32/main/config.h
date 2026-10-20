#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

/* The key configuration (host README), stored as JSON in the storage partition, and the key image files. */

#ifndef IMAGE_DIR
#define IMAGE_DIR "/storage/images"
#endif

/* Current text (default only when absent); NULL on read/allocation failure. Free with free(). */
char *config_load(size_t *length);

/* Stores `text` (already checked) atomically. */
bool config_store(const char *text, size_t length);

/* Validated, normalized tree; NULL on read/parse/validation/allocation failure. Free with cJSON_Delete. */
cJSON *config_parse(void);
bool config_save(const cJSON *root);

cJSON *config_find_page(const cJSON *root, const char *id);
cJSON *config_find_key(const cJSON *page, unsigned index);
cJSON *config_ensure_key(cJSON *page, unsigned index); /* adds {"index": n} when missing */

/* File name of a key's image: kind "" (shown image), "-icon" or "-alt". The root page keeps the plain names. */
void config_image_name(char *out, size_t size, const char *page, unsigned index, const char *kind);

/* Removes the image files of pages that are no longer in `root`. */
void config_purge_images(const cJSON *root);

/* Size and use of the storage partition in bytes; false if it cannot be read. */
bool config_storage(size_t *total, size_t *used);

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The key configuration (docs / host README), stored as JSON in the storage partition. */

/* Current configuration text (a default with one empty page when none is stored). Free with free(). */
char *config_load(size_t *length);

/* Stores `text` (already checked) atomically. */
bool config_store(const char *text, size_t length);

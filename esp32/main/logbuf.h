#pragma once

#include <stddef.h>
#include <stdint.h>

/* Keeps the last log lines (everything that goes through ESP_LOG*) for GET /api/log. */

/* Hooks the log output; call first thing in app_main. The UART output stays as it is. */
void logbuf_init(void);

/* Copies the lines numbered `since` and up (older ones may be gone) as text into `out` and sets *next to the number
 * to ask for next. Returns the number of bytes. A `since` beyond the newest line (the host restarted) starts again
 * from the oldest. */
size_t logbuf_read(uint32_t since, char *out, size_t size, uint32_t *next);

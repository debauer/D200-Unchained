#pragma once

#include <stdbool.h>
#include <time.h>

/* Local time from SNTP (needs the network). */
void wallclock_start(void);

/* The local time; false until the first sync. */
bool wallclock_now(struct tm *out);

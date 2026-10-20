#pragma once
#include <stdint.h>
#ifdef TEST_AUTH_CLOCK
int64_t esp_timer_get_time(void);
#else
static inline int64_t esp_timer_get_time(void) { return 1000; }
#endif

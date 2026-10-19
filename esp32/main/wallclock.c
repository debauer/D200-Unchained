#include "wallclock.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "clock";

void wallclock_start(void)
{
    setenv("TZ", CONFIG_D200_TIMEZONE, 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started, time zone %s", CONFIG_D200_TIMEZONE);
}

bool wallclock_now(struct tm *out)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false; /* not synced: the clock still counts from 1970 */
    localtime_r(&now, out);
    return true;
}

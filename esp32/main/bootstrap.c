#include "bootstrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bootstrap";

/* deckd for the device, embedded at build time (scripts/deckd.py writes deckd_blob.S) */
extern const uint8_t deckd_start[] ;
extern const uint8_t deckd_end[] ;

#define DEVICE_BINARY "/userdata/d200/deckd"
#define DEVICE_LOG "/tmp/deckd.log"
/* The vendor app and its watcher restart each other: freeze both, kill both, then pidof tells
 * whether any survived (docs/hardware.md). */
#define VENDOR "WatcherProcess UlanziDeckKey"
#define VENDOR_PIDS "echo vendor:$(pidof " VENDOR ")"
#define STOP_ATTEMPTS 3
#define START_CHECKS 5

static bool deckd_listening(adb_t *a, char *out, size_t cap)
{
    if (adb_exec(a, "shell:netstat -tln 2>/dev/null", out, cap) != ESP_OK) return false;
    char needle[16];
    snprintf(needle, sizeof needle, ":%d ", DECKD_PORT);
    return strstr(out, needle) != NULL;
}

/* Pids the vendor app still has: the text after "vendor:" (empty when it is gone). */
static const char *vendor_pids(const char *out)
{
    const char *p = strstr(out, "vendor:");
    if (!p) return NULL;
    p += strlen("vendor:");
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Step by step, each logged: if the USB link dies on one of them, the log shows which. */
static esp_err_t stop_vendor(adb_t *a, char *out, size_t cap)
{
    for (int i = 0; i < STOP_ATTEMPTS; i++) {
        esp_err_t err = adb_exec(a, "shell:" VENDOR_PIDS, out, cap);
        const char *pids = err == ESP_OK ? vendor_pids(out) : NULL;
        if (!pids) return ESP_FAIL;
        if (!*pids) return ESP_OK;
        ESP_LOGI(TAG, "vendor app running (pids %s): freezing", pids);
        if (adb_exec(a, "shell:killall -STOP " VENDOR " 2>/dev/null; " VENDOR_PIDS, out, cap) != ESP_OK) return ESP_FAIL;
        ESP_LOGI(TAG, "frozen, killing");
        if (adb_exec(a, "shell:killall -KILL " VENDOR " 2>/dev/null; sleep 1; " VENDOR_PIDS, out, cap) != ESP_OK) {
            return ESP_FAIL;
        }
        pids = vendor_pids(out);
        if (pids && !*pids) return ESP_OK;
        ESP_LOGW(TAG, "vendor app still running (pids %s), stopping it again", pids ? pids : "?");
    }
    return ESP_FAIL;
}

/* Size of the installed deckd, or -1 when it is missing. `ls -l` prints: mode links owner group size ... */
static long installed_size(adb_t *a, char *out, size_t cap)
{
    if (adb_exec(a, "shell:ls -l " DEVICE_BINARY " 2>/dev/null", out, cap) != ESP_OK) return -1;
    if (!strstr(out, DEVICE_BINARY)) return -1;
    char *p = out;
    for (int field = 0; field < 4; field++) {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    return *p >= '0' && *p <= '9' ? strtol(p, NULL, 10) : -1;
}

esp_err_t bootstrap_deckd(adb_t *a)
{
    const size_t size = deckd_end - deckd_start;
    char *out = malloc(512);
    if (!out) return ESP_ERR_NO_MEM;
    /* Diagnosis: harmless commands first, each logged, to see which one the link does not survive. */
    static const char *const probes[] = {
        "shell:echo ok",
        "shell:echo vendor:$(pidof " VENDOR ")",
        "shell:cat /sys/class/backlight/backlight/brightness",
    };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t perr = adb_exec(a, probes[i], out, 512);
        for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        ESP_LOGI(TAG, "probe %u (%s): %s after %lld ms, output \"%s\"", (unsigned)i, probes[i],
                 perr == ESP_OK ? "ok" : "FAILED", (long long)((esp_timer_get_time() - t0) / 1000), out);
        if (perr != ESP_OK) {
            free(out);
            return perr;
        }
    }
    esp_err_t err = stop_vendor(a, out, 512);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot stop the vendor app: %s", out);
    } else if (deckd_listening(a, out, 512)) {
        ESP_LOGI(TAG, "deckd already listens on port %d", DECKD_PORT);
    } else {
        if (installed_size(a, out, 512) != (long)size) {
            ESP_LOGI(TAG, "installing deckd (%u bytes)", (unsigned)size);
            adb_exec(a, "shell:killall deckd 2>/dev/null; mkdir -p /userdata/d200", out, 512);
            err = adb_push(a, deckd_start, size, DEVICE_BINARY, 0100755);
            if (err != ESP_OK) ESP_LOGE(TAG, "push failed");
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "starting deckd");
            err = adb_exec(a, "shell:setsid " DEVICE_BINARY " </dev/null >" DEVICE_LOG " 2>&1 & sleep 1", out, 512);
        }
        err = ESP_FAIL;
        for (int i = 0; i < START_CHECKS; i++) {
            if (deckd_listening(a, out, 512)) {
                err = ESP_OK;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "deckd is listening on the device");
        } else {
            adb_exec(a, "shell:echo pid: $(pidof deckd); cat " DEVICE_LOG, out, 512);
            ESP_LOGE(TAG, "deckd did not start: %s", out);
        }
    }
    free(out);
    return err;
}

#include "bootstrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
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

/* The vendor app resets the USB gadget soon after an ADB session starts, which breaks the link; once it is
 * gone the link is stable. So this is the very first command, and it is one command: it keeps running on
 * the device even if the link dies right after it. A failed try is repeated by the caller. */
static esp_err_t stop_vendor(adb_t *a, char *out, size_t cap)
{
    for (int i = 0; i < STOP_ATTEMPTS; i++) {
        esp_err_t err = adb_exec(a, "shell:killall -STOP " VENDOR " 2>/dev/null; killall -KILL " VENDOR
                                    " 2>/dev/null; sleep 1; " VENDOR_PIDS, out, cap);
        if (err != ESP_OK) return err;
        const char *pids = vendor_pids(out);
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

/* The device's own view of what happened on the USB gadget: kernel log lines, one log line each. */
static void dump_device_log(adb_t *a)
{
    char *text = malloc(4096);
    if (!text) return;
    if (adb_exec(a, "shell:dmesg | tail -n 40", text, 4096) == ESP_OK) {
        for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) ESP_LOGI(TAG, "dmesg: %s", line);
    } else {
        ESP_LOGW(TAG, "cannot read dmesg");
    }
    free(text);
}

#ifdef CONFIG_D200_DEBUG
static void log_lines(adb_t *a, const char *label, const char *service, size_t cap)
{
    char *text = malloc(cap);
    if (!text) return;
    if (adb_exec(a, service, text, cap) == ESP_OK) {
        for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) ESP_LOGI(TAG, "%s: %s", label, line);
    } else {
        ESP_LOGW(TAG, "%s: command failed", label);
    }
    free(text);
}

/* What runs on the D200 a moment after the bootstrap, what its framebuffer, display and backlight say, and
 * who holds them. Then a white screen written straight into /dev/fb0 (no deckd): if it does not show up,
 * something other than the framebuffer decides what the display shows. */
static void diagnose(adb_t *a)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    log_lines(a, "diag ps", "shell:ps 2>&1 | grep -v -E ' \\[|COMMAND'", 3072);
    log_lines(a, "diag deckd.log", "shell:tail -n 15 " DEVICE_LOG " 2>&1", 2048);
    log_lines(a, "diag fb", "shell:for f in name virtual_size bits_per_pixel stride state blank; do "
                            "echo $f=$(cat /sys/class/graphics/fb0/$f 2>&1); done; ls -l /dev/fb0 /dev/dri/ 2>&1", 2048);
    log_lines(a, "diag drm", "shell:for c in /sys/class/drm/*; do echo $c $(cat $c/status $c/enabled 2>/dev/null); done", 2048);
    log_lines(a, "diag backlight", "shell:for f in brightness actual_brightness bl_power max_brightness; do "
                                   "echo $f=$(cat /sys/class/backlight/backlight/$f 2>&1); done", 1024);
    log_lines(a, "diag dmesg", "shell:dmesg | grep -i -E 'drm|fb0|vop|panel|display|backlight|rockchip-drm' | tail -n 20", 4096);
    log_lines(a, "diag openers", "shell:for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q -E 'fb0|dri/card' && "
                                 "echo $(cat $p/comm) $p; done", 1024);
    /* Three tries to get something onto the display, each shown for 4 s: tell which one turned the screen white. */
    ESP_LOGI(TAG, "fb test A: white screen straight into /dev/fb0 (4 s)");
    log_lines(a, "diag fb A", "shell:head -c 3686400 /dev/zero | tr '\\000' '\\377' > /dev/fb0; echo exit:$?", 512);
    vTaskDelay(pdMS_TO_TICKS(4000));
    ESP_LOGI(TAG, "fb test B: unblank, then white (4 s)");
    log_lines(a, "diag fb B", "shell:echo 0 > /sys/class/graphics/fb0/blank; head -c 3686400 /dev/zero | tr '\\000' '\\377' > /dev/fb0; echo exit:$?", 512);
    vTaskDelay(pdMS_TO_TICKS(4000));
    ESP_LOGI(TAG, "fb test C: blank, unblank, then white (4 s)");
    log_lines(a, "diag fb C", "shell:echo 4 > /sys/class/graphics/fb0/blank; sleep 1; echo 0 > /sys/class/graphics/fb0/blank; "
                              "head -c 3686400 /dev/zero | tr '\\000' '\\377' > /dev/fb0; echo exit:$?", 512);
    vTaskDelay(pdMS_TO_TICKS(4000));
}
#endif

/* A broken link makes every adb_exec return an empty answer: "not listening", "not installed". Check it after
 * each step so that the bootstrap stops instead of acting on those answers. */
#define LINK_LOST(a) ((a)->broken)

esp_err_t bootstrap_deckd(adb_t *a)
{
    const size_t size = deckd_end - deckd_start;
    char *out = malloc(512);
    if (!out) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_FAIL;
    /* Commands that scan /proc (killall, pidof) seem to break the USB link while the vendor app runs, likely
     * through a load and clock step that dips the 5 V. Pin the CPU to its lowest clock first (the governor is
     * left alone: powersave is not available), with shell builtins only (no new process). */
    err = adb_exec(a, "shell:d=/sys/devices/system/cpu/cpu0/cpufreq; read lo < $d/cpuinfo_min_freq; "
                      "echo $lo > $d/scaling_max_freq; "
                      "read g < $d/scaling_governor; read f < $d/scaling_cur_freq; echo cpu:$g:$f", out, 512);
    if (err == ESP_OK) ESP_LOGI(TAG, "%s", out);
    if (err == ESP_OK) err = stop_vendor(a, out, 512);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot stop the vendor app: %s", out);
        goto done;
    }
#ifdef CONFIG_D200_DEBUG
    dump_device_log(a);
    if (LINK_LOST(a)) goto lost;
#endif
    bool up = deckd_listening(a, out, 512);
    if (LINK_LOST(a)) goto lost;
    if (up) {
        ESP_LOGI(TAG, "deckd already listens on port %d", DECKD_PORT);
        err = ESP_OK;
#ifdef CONFIG_D200_DEBUG
        diagnose(a);
#endif
        goto done;
    }
    long installed = installed_size(a, out, 512);
    if (LINK_LOST(a)) goto lost;
    if (installed != (long)size) {
        ESP_LOGI(TAG, "installing deckd (%u bytes)", (unsigned)size);
        adb_exec(a, "shell:killall deckd 2>/dev/null; mkdir -p /userdata/d200", out, 512);
        if (LINK_LOST(a)) goto lost;
        err = adb_push(a, deckd_start, size, DEVICE_BINARY, 0100755);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "push failed");
            goto done;
        }
    }
    ESP_LOGI(TAG, "starting deckd");
    adb_exec(a, "shell:setsid " DEVICE_BINARY " </dev/null >" DEVICE_LOG " 2>&1 & sleep 1", out, 512);
    if (LINK_LOST(a)) goto lost;
    for (int i = 0; i < START_CHECKS; i++) {
        if (deckd_listening(a, out, 512)) {
            ESP_LOGI(TAG, "deckd is listening on the device");
            err = ESP_OK;
#ifdef CONFIG_D200_DEBUG
            diagnose(a);
#endif
            goto done;
        }
        if (LINK_LOST(a)) goto lost;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    adb_exec(a, "shell:echo pid: $(pidof deckd); cat " DEVICE_LOG, out, 512);
    ESP_LOGE(TAG, "deckd did not start: %s", out);
    err = ESP_FAIL;
    goto done;
lost:
    ESP_LOGW(TAG, "USB link lost during the bootstrap");
    err = ESP_FAIL;
done:
    free(out);
    return err;
}

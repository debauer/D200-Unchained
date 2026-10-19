#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "config.h"
#include "device.h"
#include "events.h"
#include "keyfiles.h"
#include "logbuf.h"
#include "mqtt.h"
#include "wallclock.h"
#include "usb_adb.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "main";

static void mount(const char *label, const char *base, bool read_only)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = base, .partition_label = label, .format_if_mount_failed = !read_only,
        .read_only = read_only,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) ESP_LOGE(TAG, "mount %s: %s", label, esp_err_to_name(err));
}

void app_main(void)
{
    logbuf_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    mount("www", "/www", true);
    mount("storage", "/storage", false);
    keyfiles_init();
    events_init();
    mqtt_init();
    device_init();
    usb_adb_start(); /* independent of WiFi */
    if (wifi_start() == ESP_OK) {
        wallclock_start();
        mqtt_start();
        cJSON *config = config_parse();
        mqtt_configure(config);
        cJSON_Delete(config);
        ESP_ERROR_CHECK(web_start());
    }
}

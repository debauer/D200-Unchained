#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
#define GOT_IP BIT0
#define FAILED BIT1
#define MAX_RETRIES 10
static int s_retries;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries++ < MAX_RETRIES) {
            ESP_LOGW(TAG, "disconnected, retry %d", s_retries);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "address " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, GOT_IP);
    }
}

esp_err_t wifi_start(void)
{
    if (strlen(CONFIG_D200_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "no SSID: set it with `idf.py menuconfig` (D200 host)");
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_D200_WIFI_SSID, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, CONFIG_D200_WIFI_PASSWORD, sizeof cfg.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_D200_WIFI_TX_POWER));
    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP | FAILED, pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & GOT_IP) ? ESP_OK : ESP_FAIL;
}

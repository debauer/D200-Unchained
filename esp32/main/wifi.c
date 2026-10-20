#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "wifi_local.h"

static const char *TAG = "wifi";
static esp_timer_handle_t s_retry;
static int s_retries;

static void retry(void *arg)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGW(TAG, "reconnect: %s", esp_err_to_name(err));
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        unsigned seconds = s_retries < 6 ? 1u << s_retries++ : 60;
        ESP_LOGW(TAG, "disconnected, retry in %u s", seconds);
        if (esp_timer_is_active(s_retry)) ESP_ERROR_CHECK(esp_timer_stop(s_retry));
        ESP_ERROR_CHECK(esp_timer_start_once(s_retry, (uint64_t)seconds * 1000000));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "address " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        if (esp_timer_is_active(s_retry)) ESP_ERROR_CHECK(esp_timer_stop(s_retry));
    }
}

esp_err_t wifi_start(void)
{
    if (strlen(WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "no SSID: put it into esp32/wifi.local (see wifi.local.example) and rebuild");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_timer_create_args_t timer = {.callback = retry, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_retry));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof cfg.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_D200_WIFI_TX_POWER));
    return ESP_OK; /* Services start now and become reachable when DHCP succeeds. */
}

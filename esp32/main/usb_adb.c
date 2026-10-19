#include "usb_adb.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "adb.h"
#include "bootstrap.h"
#include "deckd.h"
#include "device.h"
#include "usb/usb_host.h"

static const char *TAG = "adb";

#define ADB_CLASS 0xFF
#define ADB_SUBCLASS 0x42
#define ADB_PROTOCOL 0x01
#define USB_CORE 1
#define SETTLE_MS 500
#define REAPPEAR_MS 1500

static volatile bool s_present;  /* ADB interface claimed */
static volatile bool s_attached; /* a device is on the bus */
static volatile uint8_t s_new_address;
static SemaphoreHandle_t s_new_dev;
static usb_host_client_handle_t s_client;

/* --- device discovery ----------------------------------------------------------------------- */

/* Walks the raw configuration descriptor for an ADB interface with a bulk IN and OUT endpoint. */
static bool find_adb(adb_t *a, const usb_config_desc_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    bool in_adb = false;
    a->in_ep = a->out_ep = 0;
    for (size_t off = 0; off + 2 <= cfg->wTotalLength && p[off] >= 2; off += p[off]) {
        if (p[off + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (a->in_ep && a->out_ep) break;
            in_adb = p[off + 5] == ADB_CLASS && p[off + 6] == ADB_SUBCLASS && p[off + 7] == ADB_PROTOCOL;
            if (in_adb) a->iface = p[off + 2];
        } else if (p[off + 1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_adb && (p[off + 3] & 3) == 2) {
            if (p[off + 2] & 0x80) a->in_ep = p[off + 2];
            else a->out_ep = p[off + 2];
        }
    }
    return a->in_ep && a->out_ep;
}

/* One try: open the device, claim ADB, bootstrap deckd, then hold the link until the device goes. */
static esp_err_t session(adb_t *a, uint8_t address)
{
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    esp_err_t result = ESP_FAIL;
    if (usb_host_device_open(a->client, address, &a->dev) != ESP_OK) return ESP_FAIL;
    usb_host_get_device_descriptor(a->dev, &dd);
    usb_host_get_active_config_descriptor(a->dev, &cfg);
    ESP_LOGI(TAG, "device %04x:%04x", dd->idVendor, dd->idProduct);
    if (find_adb(a, cfg) && usb_host_interface_claim(a->client, a->dev, a->iface, 0) == ESP_OK) {
        s_present = true;
        ESP_LOGI(TAG, "ADB interface %u, endpoints in 0x%02x out 0x%02x", a->iface, a->in_ep, a->out_ep);
        if (adb_connect(a) == ESP_OK && bootstrap_deckd(a) == ESP_OK) {
            result = ESP_OK;
#ifdef CONFIG_D200_SELFTEST
            deckd_t *t = malloc(sizeof *t);
            if (t && deckd_selftest(t, a) != ESP_OK) ESP_LOGE(TAG, "deckd self test failed");
            free(t);
#endif
            device_run(a); /* returns when the link breaks */
            result = s_attached ? ESP_FAIL : ESP_OK; /* still attached: the link broke, start over */
        }
        usb_host_interface_release(a->client, a->dev, a->iface);
    } else {
        ESP_LOGW(TAG, "no ADB interface on this device");
    }
    usb_host_device_close(a->client, a->dev);
    return result;
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_address = msg->new_dev.address;
        s_attached = true;
        xSemaphoreGive(s_new_dev);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_present = false;
        s_attached = false;
        ESP_LOGW(TAG, "device removed");
    }
}

static void lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void client_task(void *arg)
{
    for (;;) usb_host_client_handle_events(s_client, portMAX_DELAY);
}

static void adb_task(void *arg)
{
    adb_t *a = calloc(1, sizeof *a); /* large: buffers inside */
    a->client = s_client;
    for (;;) {
        xSemaphoreTake(s_new_dev, portMAX_DELAY);
        /* The vendor app seems to reconfigure the USB gadget shortly after a host appears, which kills the
         * link. Wait until the bus has been quiet for a while; after a failure wait for the device to come
         * back by itself, and reset the port only if it does not. */
        for (int attempt = 1; s_attached; attempt++) { /* until it works or the device is gone */
            while (xSemaphoreTake(s_new_dev, pdMS_TO_TICKS(SETTLE_MS)) == pdTRUE) {
            }
            if (!s_attached) break; /* gone again: wait for the next one */
            if (session(a, s_new_address) == ESP_OK) break;
            ESP_LOGW(TAG, "attempt %d failed, waiting for the device to come back", attempt);
            s_present = false;
            if (xSemaphoreTake(s_new_dev, pdMS_TO_TICKS(REAPPEAR_MS)) != pdTRUE) {
                ESP_LOGW(TAG, "device did not come back: resetting the USB port");
                usb_host_lib_set_root_port_power(false);
                vTaskDelay(pdMS_TO_TICKS(500));
                usb_host_lib_set_root_port_power(true);
                xSemaphoreTake(s_new_dev, pdMS_TO_TICKS(8000));
            }
        }
    }
}

/* Runs on core 1: the USB interrupt is allocated on the core that installs the host, and WiFi lives on core 0. */
static void usb_init_task(void *arg)
{
    const usb_host_config_t host = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&host));
    const usb_host_client_config_t client = {
        .is_synchronous = false, .max_num_event_msg = 5, .async = {.client_event_callback = client_event},
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client, &s_client));
    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, 20, NULL, USB_CORE);
    xTaskCreatePinnedToCore(client_task, "usb_client", 4096, NULL, 19, NULL, USB_CORE);
    xTaskCreatePinnedToCore(adb_task, "adb", 6144, NULL, 5, NULL, USB_CORE);
    vTaskDelete(NULL);
}

void usb_adb_start(void)
{
    s_new_dev = xSemaphoreCreateBinary();
#ifdef CONFIG_D200_DEBUG
    static const char *const usb_tags[] = {"HCD", "USBH", "USB HOST", "HUB", "HAL"};
    for (size_t i = 0; i < sizeof usb_tags / sizeof usb_tags[0]; i++) esp_log_level_set(usb_tags[i], ESP_LOG_DEBUG);
#endif
    xTaskCreatePinnedToCore(usb_init_task, "usb_init", 4096, NULL, 5, NULL, USB_CORE);
}

bool usb_adb_device_present(void)
{
    return s_present;
}

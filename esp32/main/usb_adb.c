#include "usb_adb.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "adb";

#define ADB_CLASS 0xFF
#define ADB_SUBCLASS 0x42
#define ADB_PROTOCOL 0x01
#define XFER_TIMEOUT_MS 2000
#define RX_CHUNK 1024 /* multiple of the bulk packet size (64 full speed, 512 high speed) */

#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257
#define ADB_VERSION 0x01000000
#define ADB_MAX_DATA 4096

typedef struct {
    uint32_t command, arg0, arg1, length, check, magic;
} adb_msg_t;

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    uint8_t in_ep, out_ep;
    uint8_t iface;
    uint8_t rx[RX_CHUNK];
    size_t rx_pos, rx_len;
} adb_t;

static volatile bool s_present;
static volatile uint8_t s_new_address;
static SemaphoreHandle_t s_new_dev;
static usb_host_client_handle_t s_client;

/* --- bulk transfers (blocking; the callback runs in the client event task) ---------------- */

static void xfer_done(usb_transfer_t *x)
{
    xSemaphoreGive((SemaphoreHandle_t)x->context);
}

static esp_err_t bulk(adb_t *a, uint8_t ep, void *data, size_t len, size_t *actual)
{
    usb_transfer_t *x;
    esp_err_t err = usb_host_transfer_alloc(len, 0, &x);
    if (err != ESP_OK) return err;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!(ep & 0x80)) memcpy(x->data_buffer, data, len);
    x->device_handle = a->dev;
    x->bEndpointAddress = ep;
    x->num_bytes = len;
    x->timeout_ms = XFER_TIMEOUT_MS;
    x->callback = xfer_done;
    x->context = done;
    err = usb_host_transfer_submit(x);
    if (err == ESP_OK) {
        xSemaphoreTake(done, portMAX_DELAY);
        if (x->status != USB_TRANSFER_STATUS_COMPLETED) {
            ESP_LOGW(TAG, "transfer on ep 0x%02x: status %d", ep, x->status);
            err = ESP_FAIL;
        } else if (ep & 0x80) {
            memcpy(data, x->data_buffer, x->actual_num_bytes);
            *actual = x->actual_num_bytes;
        }
    }
    vSemaphoreDelete(done);
    usb_host_transfer_free(x);
    return err;
}

static esp_err_t rx_read(adb_t *a, uint8_t *dst, size_t n)
{
    while (n > 0) {
        if (a->rx_pos == a->rx_len) {
            a->rx_pos = 0;
            a->rx_len = 0;
            esp_err_t err = bulk(a, a->in_ep, a->rx, RX_CHUNK, &a->rx_len);
            if (err != ESP_OK) return err;
        }
        size_t take = a->rx_len - a->rx_pos < n ? a->rx_len - a->rx_pos : n;
        memcpy(dst, a->rx + a->rx_pos, take);
        a->rx_pos += take;
        dst += take;
        n -= take;
    }
    return ESP_OK;
}

/* --- ADB messages ------------------------------------------------------------------------- */

static esp_err_t adb_send(adb_t *a, uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += ((const uint8_t *)data)[i]; /* old adbd verifies it */
    adb_msg_t m = {cmd, arg0, arg1, (uint32_t)len, sum, cmd ^ 0xffffffff};
    esp_err_t err = bulk(a, a->out_ep, &m, sizeof m, NULL);
    if (err == ESP_OK && len) err = bulk(a, a->out_ep, (void *)data, len, NULL);
    return err;
}

/* Reads one message; `data` must hold ADB_MAX_DATA bytes. */
static esp_err_t adb_recv(adb_t *a, adb_msg_t *m, uint8_t *data)
{
    esp_err_t err = rx_read(a, (uint8_t *)m, sizeof *m);
    if (err != ESP_OK) return err;
    if (m->magic != (m->command ^ 0xffffffff) || m->length > ADB_MAX_DATA) {
        ESP_LOGE(TAG, "bad message: cmd 0x%08lx length %lu", (unsigned long)m->command, (unsigned long)m->length);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return m->length ? rx_read(a, data, m->length) : ESP_OK;
}

static esp_err_t adb_connect(adb_t *a, uint8_t *buf)
{
    static const char banner[] = "host::";
    esp_err_t err = adb_send(a, A_CNXN, ADB_VERSION, ADB_MAX_DATA, banner, sizeof banner);
    adb_msg_t m;
    while (err == ESP_OK) {
        err = adb_recv(a, &m, buf);
        if (err == ESP_OK && m.command == A_CNXN) {
            ESP_LOGI(TAG, "connected: version 0x%08lx, max data %lu, \"%.*s\"", (unsigned long)m.arg0,
                     (unsigned long)m.arg1, (int)m.length, buf);
            return ESP_OK;
        }
    }
    return err;
}

/* Runs `service` (e.g. "shell:echo ok") and logs what it prints until the stream closes. */
static esp_err_t adb_run(adb_t *a, const char *service, uint8_t *buf)
{
    const uint32_t local = 1;
    esp_err_t err = adb_send(a, A_OPEN, local, 0, service, strlen(service) + 1);
    adb_msg_t m;
    uint32_t remote = 0;
    while (err == ESP_OK) {
        err = adb_recv(a, &m, buf);
        if (err != ESP_OK) break;
        if (m.command == A_OKAY) {
            remote = m.arg0;
        } else if (m.command == A_WRTE) {
            ESP_LOGI(TAG, "%s -> %.*s", service, (int)m.length, buf);
            err = adb_send(a, A_OKAY, local, m.arg0, NULL, 0);
        } else if (m.command == A_CLSE) {
            return ESP_OK;
        }
    }
    (void)remote;
    return err;
}

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

static void session(adb_t *a, uint8_t address)
{
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    if (usb_host_device_open(a->client, address, &a->dev) != ESP_OK) return;
    usb_host_get_device_descriptor(a->dev, &dd);
    usb_host_get_active_config_descriptor(a->dev, &cfg);
    ESP_LOGI(TAG, "device %04x:%04x", dd->idVendor, dd->idProduct);
    if (find_adb(a, cfg) && usb_host_interface_claim(a->client, a->dev, a->iface, 0) == ESP_OK) {
        s_present = true;
        ESP_LOGI(TAG, "ADB interface %u, endpoints in 0x%02x out 0x%02x", a->iface, a->in_ep, a->out_ep);
        uint8_t *buf = malloc(ADB_MAX_DATA);
        a->rx_pos = a->rx_len = 0;
        if (buf && adb_connect(a, buf) == ESP_OK) adb_run(a, "shell:echo ok", buf);
        free(buf);
        usb_host_interface_release(a->client, a->dev, a->iface);
    } else {
        ESP_LOGW(TAG, "no ADB interface on this device");
    }
    usb_host_device_close(a->client, a->dev);
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_address = msg->new_dev.address;
        xSemaphoreGive(s_new_dev);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_present = false;
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
    adb_t *a = calloc(1, sizeof *a);
    a->client = s_client;
    for (;;) {
        xSemaphoreTake(s_new_dev, portMAX_DELAY);
        session(a, s_new_address);
    }
}

void usb_adb_start(void)
{
    s_new_dev = xSemaphoreCreateBinary();
    const usb_host_config_t host = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&host));
    const usb_host_client_config_t client = {
        .is_synchronous = false, .max_num_event_msg = 5, .async = {.client_event_callback = client_event},
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client, &s_client));
    xTaskCreate(lib_task, "usb_lib", 4096, NULL, 20, NULL);
    xTaskCreate(client_task, "usb_client", 4096, NULL, 19, NULL);
    xTaskCreate(adb_task, "adb", 6144, NULL, 5, NULL);
}

bool usb_adb_device_present(void)
{
    return s_present;
}

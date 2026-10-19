/* ADB over USB bulk transfers: messages, handshake and one-shot services. */

#include "adb.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "adb";
#define XFER_TIMEOUT_MS 10000 /* a shell command may print late */
#define ADB_VERSION 0x01000000
#define LOCAL_ID 1

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
            ESP_LOGW(TAG, "transfer on ep 0x%02x: status %d (%u of %u bytes)", ep, x->status,
                     (unsigned)x->actual_num_bytes, (unsigned)len);
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
            esp_err_t err = bulk(a, a->in_ep, a->rx, ADB_RX_CHUNK, &a->rx_len);
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

/* --- messages --------------------------------------------------------------------------------- */

esp_err_t adb_send_msg(adb_t *a, uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += ((const uint8_t *)data)[i]; /* old adbd verifies it */
    adb_msg_t m = {cmd, arg0, arg1, (uint32_t)len, sum, cmd ^ 0xffffffff};
    esp_err_t err = bulk(a, a->out_ep, &m, sizeof m, NULL);
    if (err == ESP_OK && len) err = bulk(a, a->out_ep, (void *)data, len, NULL);
    return err;
}

/* Reads one message; its payload lands in a->buf. */
esp_err_t adb_recv_msg(adb_t *a, adb_msg_t *m)
{
    esp_err_t err = rx_read(a, (uint8_t *)m, sizeof *m);
    if (err != ESP_OK) return err;
    if (m->magic != (m->command ^ 0xffffffff) || m->length > ADB_MAX_DATA) {
        ESP_LOGE(TAG, "bad message: cmd 0x%08lx length %lu", (unsigned long)m->command, (unsigned long)m->length);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return m->length ? rx_read(a, a->buf, m->length) : ESP_OK;
}

void adb_recover(adb_t *a)
{
    usb_host_endpoint_clear(a->dev, a->in_ep);
    usb_host_endpoint_clear(a->dev, a->out_ep);
    a->rx_pos = a->rx_len = 0;
}

esp_err_t adb_connect(adb_t *a)
{
    static const char banner[] = "host::";
    a->rx_pos = a->rx_len = 0;
    esp_err_t err = adb_send_msg(a, A_CNXN, ADB_VERSION, ADB_MAX_DATA, banner, sizeof banner);
    adb_msg_t m;
    while (err == ESP_OK) {
        err = adb_recv_msg(a, &m);
        if (err == ESP_OK && m.command == A_CNXN) {
            ESP_LOGI(TAG, "connected: version 0x%08lx, max data %lu, \"%.*s\"", (unsigned long)m.arg0,
                     (unsigned long)m.arg1, (int)m.length, a->buf);
            return ESP_OK;
        }
    }
    return err;
}

esp_err_t adb_exec(adb_t *a, const char *service, char *out, size_t cap)
{
    size_t used = 0;
    if (out && cap) out[0] = 0;
    esp_err_t err = adb_send_msg(a, A_OPEN, LOCAL_ID, 0, service, strlen(service) + 1);
    adb_msg_t m;
    while (err == ESP_OK) {
        err = adb_recv_msg(a, &m);
        if (err != ESP_OK) break;
        if (m.command == A_WRTE) {
            if (out && cap) {
                size_t take = m.length < cap - 1 - used ? m.length : cap - 1 - used;
                memcpy(out + used, a->buf, take);
                used += take;
                out[used] = 0;
            }
            err = adb_send_msg(a, A_OKAY, LOCAL_ID, m.arg0, NULL, 0);
        } else if (m.command == A_CLSE) {
            return ESP_OK;
        }
    }
    return err;
}

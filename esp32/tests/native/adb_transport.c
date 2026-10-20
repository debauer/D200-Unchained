#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "adb.h"

static int signal, halted, flushed, completed;
static bool timeout;
static usb_transfer_t *pending;
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &signal; }
int xSemaphoreGive(SemaphoreHandle_t sem) { (*(int *)sem)++; completed++; return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t sem, unsigned ticks)
{
    assert(ticks != portMAX_DELAY);
    if (!*(int *)sem) return pdFALSE;
    (*(int *)sem)--;
    return pdTRUE;
}
esp_err_t usb_host_transfer_alloc(size_t size, int iso, usb_transfer_t **out)
{
    (void)iso;
    *out = calloc(1, sizeof **out);
    (*out)->data_buffer = malloc(size);
    assert((*out)->data_buffer);
    return ESP_OK;
}
esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer)
{
    assert(!pending && !signal);
    transfer->actual_num_bytes = transfer->num_bytes;
    if (timeout) pending = transfer;
    else transfer->callback(transfer);
    return ESP_OK;
}
esp_err_t usb_host_endpoint_halt(usb_device_handle_t dev, uint8_t ep)
{
    (void)dev;
    assert(pending && pending->bEndpointAddress == ep);
    halted++;
    return ESP_OK;
}
esp_err_t usb_host_endpoint_flush(usb_device_handle_t dev, uint8_t ep)
{
    (void)dev; (void)ep;
    assert(halted && pending);
    flushed++;
    pending->callback(pending);
    pending = NULL;
    return ESP_OK;
}
void esp_restart(void) { abort(); }

int main(void)
{
    adb_t a = {.out_ep = 1};
    assert(adb_send_msg(&a, A_CNXN, 1, 2, NULL, 0) == ESP_OK);
    assert(completed == 1 && !a.broken && !pending && !signal);
    timeout = true;
    assert(adb_send_msg(&a, A_CNXN, 1, 2, NULL, 0) == ESP_ERR_TIMEOUT);
    assert(halted == 1 && flushed == 1 && completed == 2 && a.broken && !pending && !signal);
    free(a.out_x->data_buffer);
    free(a.out_x);
    free(a.in_x->data_buffer);
    free(a.in_x);
    puts("adb transport ok");
}

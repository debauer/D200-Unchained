#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *usb_device_handle_t;
typedef void *usb_host_client_handle_t;
typedef struct usb_transfer {
    uint8_t *data_buffer;
    size_t actual_num_bytes, num_bytes;
    int status;
    unsigned timeout_ms;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    void (*callback)(struct usb_transfer *);
    void *context;
} usb_transfer_t;
#define USB_TRANSFER_STATUS_COMPLETED 0
esp_err_t usb_host_transfer_alloc(size_t size, int iso, usb_transfer_t **out);
esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer);
esp_err_t usb_host_endpoint_halt(usb_device_handle_t dev, uint8_t ep);
esp_err_t usb_host_endpoint_flush(usb_device_handle_t dev, uint8_t ep);

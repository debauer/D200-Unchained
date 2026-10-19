#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb/usb_host.h"

#define ADB_MAX_DATA 4096
#define ADB_RX_CHUNK 1024 /* multiple of the bulk packet size (64 full speed, 512 high speed) */

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    uint8_t in_ep, out_ep;
    uint8_t iface;
    uint8_t rx[ADB_RX_CHUNK];
    size_t rx_pos, rx_len;
    uint8_t buf[ADB_MAX_DATA];  /* payload of the last message */
    uint8_t reply[8];           /* last sync reply ("OKAY"/"FAIL" + number) */
    uint32_t reply_len;         /* its length field, valid once `got_reply` */
    int got_reply;
} adb_t;

typedef struct {
    uint32_t command, arg0, arg1, length, check, magic;
} adb_msg_t;

#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257

/* Handshake after the interface is claimed. */
esp_err_t adb_connect(adb_t *a);

/* After a transfer error: clears both endpoints and drops buffered data; connect again afterwards. */
void adb_recover(adb_t *a);

/* Runs a service ("shell:...") and collects what it prints into `out` (NUL terminated, may be NULL)
 * until the stream closes. Old adbd versions return no exit code: judge the output. */
esp_err_t adb_exec(adb_t *a, const char *service, char *out, size_t cap);

/* Copies `data` to `remote` on the device (sync protocol). `mode` e.g. 0100755. */
esp_err_t adb_push(adb_t *a, const void *data, size_t len, const char *remote, uint32_t mode);

/* Used by adb_sync.c */
esp_err_t adb_send_msg(adb_t *a, uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data, size_t len);
esp_err_t adb_recv_msg(adb_t *a, adb_msg_t *m);

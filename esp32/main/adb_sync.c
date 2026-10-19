/* ADB sync protocol: copy a buffer to a file on the device. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adb.h"
#include "esp_log.h"

static const char *TAG = "adb_sync";
#define LOCAL_ID 1
#define SYNC_HEADER 8
#define CHUNK (ADB_MAX_DATA - SYNC_HEADER)

static uint32_t remote_id;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* Handles one incoming message: acknowledges data, remembers a sync reply. Returns its command. */
static esp_err_t pump(adb_t *a, uint32_t *command)
{
    adb_msg_t m;
    esp_err_t err = adb_recv_msg(a, &m);
    if (err != ESP_OK) return err;
    *command = m.command;
    if (m.command == A_OKAY) {
        remote_id = m.arg0;
    } else if (m.command == A_WRTE) {
        if (m.length >= 8 && !a->got_reply) {
            memcpy(a->reply, a->buf, 8);
            a->got_reply = 1;
            uint32_t n = a->reply[4] | a->reply[5] << 8 | a->reply[6] << 16 | (uint32_t)a->reply[7] << 24;
            if (memcmp(a->reply, "FAIL", 4) == 0) ESP_LOGE(TAG, "device: FAIL (%lu bytes of text follow)", (unsigned long)n);
        }
        err = adb_send_msg(a, A_OKAY, LOCAL_ID, m.arg0, NULL, 0);
    }
    return err;
}

/* One sync packet in a WRTE: 4-byte id, 4-byte number, payload. Waits for the stream acknowledgement. */
static esp_err_t sync_write(adb_t *a, const char *id, uint32_t number, const void *payload, size_t len, uint8_t *tmp)
{
    memcpy(tmp, id, 4);
    put_u32(tmp + 4, number);
    if (len) memcpy(tmp + SYNC_HEADER, payload, len);
    esp_err_t err = adb_send_msg(a, A_WRTE, LOCAL_ID, remote_id, tmp, SYNC_HEADER + len);
    uint32_t cmd = 0;
    while (err == ESP_OK && cmd != A_OKAY) {
        err = pump(a, &cmd);
        if (err == ESP_OK && cmd == A_CLSE) err = ESP_FAIL;
    }
    return err;
}

esp_err_t adb_push(adb_t *a, const void *data, size_t len, const char *remote, uint32_t mode)
{
    char target[160];
    int n = snprintf(target, sizeof target, "%s,%lu", remote, (unsigned long)mode);
    if (n <= 0 || n >= (int)sizeof target) return ESP_ERR_INVALID_ARG;
    uint8_t *tmp = malloc(ADB_MAX_DATA);
    if (!tmp) return ESP_ERR_NO_MEM;
    a->got_reply = 0;
    esp_err_t err = adb_send_msg(a, A_OPEN, LOCAL_ID, 0, "sync:", sizeof "sync:");
    uint32_t cmd = 0;
    while (err == ESP_OK && cmd != A_OKAY) err = pump(a, &cmd);
    if (err == ESP_OK) err = sync_write(a, "SEND", n, target, n, tmp);
    for (size_t off = 0; err == ESP_OK && off < len; off += CHUNK) {
        size_t take = len - off < CHUNK ? len - off : CHUNK;
        err = sync_write(a, "DATA", take, (const uint8_t *)data + off, take, tmp);
    }
    if (err == ESP_OK) err = sync_write(a, "DONE", 0, NULL, 0, tmp);
    while (err == ESP_OK && !a->got_reply) err = pump(a, &cmd);
    if (err == ESP_OK && memcmp(a->reply, "OKAY", 4) != 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        err = adb_send_msg(a, A_CLSE, LOCAL_ID, remote_id, NULL, 0);
        cmd = 0;
        while (err == ESP_OK && cmd != A_CLSE) err = pump(a, &cmd);
    }
    free(tmp);
    return err;
}

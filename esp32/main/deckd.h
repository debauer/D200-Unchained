#pragma once

#include <stddef.h>
#include <stdint.h>

#include "adb.h"
#include "esp_err.h"

/* deckd protocol (docs/protocol.md) over the ADB stream tcp:7200. */
enum {
    DECKD_HELLO = 0x01,
    DECKD_SET_BRIGHTNESS = 0x02,
    DECKD_CLEAR = 0x03,
    DECKD_FILL_KEY = 0x04,
    DECKD_SET_KEY_IMAGE = 0x05,
    DECKD_PING = 0x06,
    DECKD_OK = 0x80,
    DECKD_INFO = 0x81,
    DECKD_KEY_EVENT = 0x90,
    DECKD_ERROR = 0xFF,
};

#define DECKD_REPLY_MAX 1024

typedef struct {
    adb_t *adb;
    uint32_t remote;           /* remote stream id */
    int okay;                  /* the device acknowledged our last WRTE */
    int closed;
    uint8_t rx[DECKD_REPLY_MAX]; /* received stream bytes not yet parsed into frames */
    size_t rx_len;
    uint8_t tx[ADB_MAX_DATA];  /* header + first payload bytes of a frame */
    void (*on_key)(uint16_t index, bool pressed); /* KEY_EVENTs that arrive between replies */
} deckd_t;

esp_err_t deckd_open(deckd_t *d, adb_t *a);

/* Sends one command and waits for its reply (KEY_EVENTs in between are passed to `on_key`, may be NULL).
 * The reply payload lands in `reply` (cap bytes); *type and *len describe it. */
esp_err_t deckd_request(deckd_t *d, uint8_t type, const void *payload, size_t len, uint8_t *reply_type,
                        uint8_t *reply, size_t cap, size_t *reply_len);

/* Hello, ping round trips and full-size key images, timed in the log. */
esp_err_t deckd_selftest(deckd_t *d, adb_t *a);

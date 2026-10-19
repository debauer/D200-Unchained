#include "deckd.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "deckd";
#define LOCAL_ID 2
#define FRAME_HEADER 5

static void put_u16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Handles one ADB message of the stream: acknowledgements and incoming data. */
static esp_err_t pump(deckd_t *d)
{
    adb_msg_t m;
    esp_err_t err = adb_recv_msg(d->adb, &m);
    if (err != ESP_OK) return err;
    if (m.command == A_OKAY) {
        d->remote = m.arg0;
        d->okay = 1;
    } else if (m.command == A_WRTE) {
        if (d->rx_len + m.length > sizeof d->rx) {
            ESP_LOGE(TAG, "reply too large (%u bytes buffered)", (unsigned)(d->rx_len + m.length));
            return ESP_ERR_NO_MEM;
        }
        memcpy(d->rx + d->rx_len, d->adb->buf, m.length);
        d->rx_len += m.length;
        err = adb_send_msg(d->adb, A_OKAY, LOCAL_ID, m.arg0, NULL, 0);
    } else if (m.command == A_CLSE) {
        d->closed = 1;
        return ESP_FAIL;
    }
    return err;
}

esp_err_t deckd_open(deckd_t *d, adb_t *a)
{
    memset(d, 0, sizeof *d);
    d->adb = a;
    esp_err_t err = adb_send_msg(a, A_OPEN, LOCAL_ID, 0, "tcp:7200", sizeof "tcp:7200");
    while (err == ESP_OK && !d->okay) err = pump(d);
    return err;
}

static esp_err_t write_chunk(deckd_t *d, const void *data, size_t len)
{
    d->okay = 0;
    esp_err_t err = adb_send_msg(d->adb, A_WRTE, LOCAL_ID, d->remote, data, len);
    while (err == ESP_OK && !d->okay) err = pump(d);
    return err;
}

/* One frame, split into WRTEs of at most ADB_MAX_DATA bytes; the first carries the frame header. */
static esp_err_t send_frame(deckd_t *d, uint8_t type, const uint8_t *payload, size_t len)
{
    d->tx[0] = type;
    put_u32(d->tx + 1, len);
    size_t first = len < ADB_MAX_DATA - FRAME_HEADER ? len : ADB_MAX_DATA - FRAME_HEADER;
    memcpy(d->tx + FRAME_HEADER, payload, first);
    esp_err_t err = write_chunk(d, d->tx, FRAME_HEADER + first);
    for (size_t off = first; err == ESP_OK && off < len; off += ADB_MAX_DATA) {
        size_t take = len - off < ADB_MAX_DATA ? len - off : ADB_MAX_DATA;
        err = write_chunk(d, payload + off, take);
    }
    return err;
}

/* Takes the next complete frame out of the receive buffer; ESP_ERR_NOT_FOUND when there is none yet. */
static esp_err_t next_frame_buffered(deckd_t *d, uint8_t *type, uint8_t *out, size_t cap, size_t *out_len)
{
    if (d->rx_len < FRAME_HEADER) return ESP_ERR_NOT_FOUND;
    size_t len = get_u32(d->rx + 1);
    if (len > DECKD_REPLY_MAX - FRAME_HEADER || len > cap) return ESP_ERR_NO_MEM;
    if (d->rx_len < FRAME_HEADER + len) return ESP_ERR_NOT_FOUND;
    *type = d->rx[0];
    memcpy(out, d->rx + FRAME_HEADER, len);
    *out_len = len;
    d->rx_len -= FRAME_HEADER + len;
    memmove(d->rx, d->rx + FRAME_HEADER + len, d->rx_len);
    return ESP_OK;
}

/* Waits for the next complete frame in the receive buffer and removes it. */
static esp_err_t next_frame(deckd_t *d, uint8_t *type, uint8_t *out, size_t cap, size_t *out_len)
{
    for (;;) {
        esp_err_t err = next_frame_buffered(d, type, out, cap, out_len);
        if (err != ESP_ERR_NOT_FOUND) return err;
        err = pump(d);
        if (err != ESP_OK) return err;
    }
}

/* Waits for the reply to the command just sent, passing KEY_EVENTs on. */
static esp_err_t await_reply(deckd_t *d, uint8_t *reply_type, uint8_t *reply, size_t cap, size_t *reply_len)
{
    esp_err_t err = ESP_OK;
    while (err == ESP_OK) {
        err = next_frame(d, reply_type, reply, cap, reply_len);
        if (err == ESP_OK && *reply_type == DECKD_KEY_EVENT) {
            if (*reply_len >= 3 && d->on_key) d->on_key(reply[0] | reply[1] << 8, reply[2] != 0);
            continue;
        }
        break;
    }
    return err;
}

esp_err_t deckd_set_key_image_file(deckd_t *d, unsigned index, unsigned w, unsigned h, FILE *f, uint8_t *reply_type,
                                   uint8_t *reply, size_t cap, size_t *reply_len)
{
    const size_t pixels = (size_t)w * h * 2;
    d->tx[0] = DECKD_SET_KEY_IMAGE;
    put_u32(d->tx + 1, 7 + pixels);
    put_u16(d->tx + FRAME_HEADER, index);
    d->tx[FRAME_HEADER + 2] = 0; /* RGB565 */
    put_u16(d->tx + FRAME_HEADER + 3, w);
    put_u16(d->tx + FRAME_HEADER + 5, h);
    size_t used = FRAME_HEADER + 7, sent = 0;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && sent < pixels) {
        size_t want = ADB_MAX_DATA - used < pixels - sent ? ADB_MAX_DATA - used : pixels - sent;
        if (fread(d->tx + used, 1, want, f) != want) return ESP_FAIL; /* the file is shorter than announced */
        err = write_chunk(d, d->tx, used + want);
        sent += want;
        used = 0;
    }
    return err == ESP_OK ? await_reply(d, reply_type, reply, cap, reply_len) : err;
}

esp_err_t deckd_request(deckd_t *d, uint8_t type, const void *payload, size_t len, uint8_t *reply_type,
                        uint8_t *reply, size_t cap, size_t *reply_len)
{
    esp_err_t err = send_frame(d, type, payload, len);
    if (err != ESP_OK) return err;
    return await_reply(d, reply_type, reply, cap, reply_len);
}

/* --- self test ------------------------------------------------------------------------------------ */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

esp_err_t deckd_selftest(deckd_t *d, adb_t *a)
{
    uint8_t type, reply[DECKD_REPLY_MAX];
    size_t n;
    esp_err_t err = deckd_open(d, a);
    if (err == ESP_OK) err = deckd_request(d, DECKD_HELLO, NULL, 0, &type, reply, sizeof reply, &n);
    if (err != ESP_OK || type != DECKD_INFO || n < 7) {
        ESP_LOGE(TAG, "hello failed: %s (reply 0x%02x)", esp_err_to_name(err), err == ESP_OK ? type : 0);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "protocol %u, screen %ux%u, %u keys", reply[0] | reply[1] << 8, reply[2] | reply[3] << 8,
             reply[4] | reply[5] << 8, reply[6]);

    const int pings = 20;
    int64_t t0 = now_ms();
    for (int i = 0; err == ESP_OK && i < pings; i++) err = deckd_request(d, DECKD_PING, NULL, 0, &type, reply, sizeof reply, &n);
    if (err == ESP_OK) ESP_LOGI(TAG, "ping: %lld ms per round trip", (long long)((now_ms() - t0) * 10 / pings) / 10);

    /* full-size images for key 0 (196x196 RGB565): the bandwidth that matters for drawing */
    const unsigned w = 196, h = 196;
    const size_t body = 7 + w * h * 2;
    uint8_t *img = malloc(body);
    if (!img) return ESP_ERR_NO_MEM;
    static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F}; /* red, green, blue */
    for (int round = 0; err == ESP_OK && round < 6; round++) {
        put_u16(img, 0);      /* key index */
        img[2] = 0;           /* format RGB565 */
        put_u16(img + 3, w);
        put_u16(img + 5, h);
        for (size_t i = 0; i < w * h; i++) put_u16(img + 7 + 2 * i, colors[round % 3]);
        t0 = now_ms();
        err = deckd_request(d, DECKD_SET_KEY_IMAGE, img, body, &type, reply, sizeof reply, &n);
        int64_t ms = now_ms() - t0;
        if (err == ESP_OK) ESP_LOGI(TAG, "image %u bytes: %lld ms (%lld KB/s), reply 0x%02x", (unsigned)body, (long long)ms,
                                    (long long)(body * 1000 / (ms ? ms : 1) / 1024), type);
    }
    free(img);
    return err;
}

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "adb.h"
#include "deckd.h"

static struct { adb_msg_t msg; uint8_t data[ADB_MAX_DATA]; } input[32];
static unsigned count, position, acks, closes, keys;

static void reset(void) { count = position = acks = closes = keys = 0; }
static void message(uint32_t type, unsigned local, const void *data, size_t size)
{
    assert(count < 32 && size <= ADB_MAX_DATA);
    input[count].msg = (adb_msg_t){.command = type, .arg0 = 7, .arg1 = local, .length = size};
    if (size) memcpy(input[count].data, data, size);
    count++;
}
esp_err_t adb_recv_msg(adb_t *a, adb_msg_t *msg)
{
    assert(position < count); /* An extra read after CLSE is a regression, not a simulated timeout. */
    *msg = input[position].msg;
    memcpy(a->buf, input[position].data, msg->length);
    position++;
    return ESP_OK;
}
esp_err_t adb_send_msg(adb_t *a, uint32_t type, uint32_t local, uint32_t remote, const void *data, size_t size)
{
    (void)a; (void)local; (void)remote; (void)data; (void)size;
    if (type == A_OKAY) acks++;
    if (type == A_CLSE) closes++;
    return ESP_OK;
}
static void key(uint16_t index, bool pressed) { assert(index == 3 && pressed); keys++; }

static void sync_tests(void)
{
    adb_t a = {0};
    static const uint8_t reply[] = {'O', 'K', 'A', 'Y', 0, 0, 0, 0};
    for (size_t split = 1; split < sizeof reply; split++) {
        reset();
        for (int i = 0; i < 4; i++) message(A_OKAY, 1, NULL, 0);
        message(A_WRTE, 1, reply, split);
        message(A_WRTE, 1, reply + split, sizeof reply - split);
        message(A_CLSE, 1, NULL, 0);
        assert(adb_push(&a, "x", 1, "/userdata/d200/deckd", 0100755) == ESP_OK);
        assert(position == count && acks == 2 && closes == 1);
    }
    reset();
    message(A_CLSE, 1, NULL, 0);
    assert(adb_push(&a, "x", 1, "/userdata/d200/deckd", 0100755) == ESP_FAIL);
    assert(position == count);
    reset();
    for (int i = 0; i < 4; i++) message(A_OKAY, 1, NULL, 0);
    message(A_WRTE, 1, reply, 3);
    message(A_CLSE, 1, NULL, 0);
    assert(adb_push(&a, "x", 1, "/userdata/d200/deckd", 0100755) == ESP_FAIL);
    assert(position == count);
}

static void deckd_tests(void)
{
    adb_t a = {0};
    deckd_t d;
    reset();
    message(A_OKAY, 2, NULL, 0);
    assert(deckd_open(&d, &a) == ESP_OK);
    d.on_key = key;
    uint8_t events[4096];
    static const uint8_t event[] = {DECKD_KEY_EVENT, 3, 0, 0, 0, 3, 0, 1};
    for (size_t i = 0; i < sizeof events; i += sizeof event) memcpy(events + i, event, sizeof event);
    message(A_WRTE, 2, events, sizeof events);
    message(A_OKAY, 2, NULL, 0);
    message(A_WRTE, 2, events, sizeof events);
    message(A_OKAY, 2, NULL, 0);
    message(A_OKAY, 2, NULL, 0);
    uint8_t response[] = {DECKD_OK, 0, 0, 0, 0};
    message(A_WRTE, 2, response, 2);
    message(A_WRTE, 2, response + 2, sizeof response - 2);
    uint8_t payload[9000] = {0}, type, reply[DECKD_REPLY_MAX];
    size_t size;
    assert(deckd_request(&d, DECKD_SET_KEY_IMAGE, payload, sizeof payload, &type, reply, sizeof reply, &size) == ESP_OK);
    assert(type == DECKD_OK && !size && keys == 1024);
    assert(position == count && acks == 4);
    message(A_CLSE, 2, NULL, 0);
    assert(deckd_close(&d) == ESP_OK && closes == 1 && position == count);

    reset();
    message(A_OKAY, 99, NULL, 0);
    assert(deckd_open(&d, &a) == ESP_ERR_INVALID_RESPONSE);
}

int main(void)
{
    sync_tests();
    deckd_tests();
    puts("protocol ok");
}

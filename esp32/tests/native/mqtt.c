// MQTT value extraction and settings on the PC: no ESP-IDF needed.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mqttjson.h"
#include "mqttsettings.h"

static void json(const char *payload, const char *path, const char *want) {
    char out[MQTT_VALUE_MAX + 1];
    mqtt_json_value(payload, path, out, sizeof out);
    if (strcmp(out, want)) { printf("FAIL %s %s -> '%s' (want '%s')\n", payload, path, out, want); assert(0); }
}
static void plain(const char *payload, const char *want) {
    char out[MQTT_VALUE_MAX + 1];
    mqtt_plain_value(payload, strlen(payload), out, sizeof out);
    if (strcmp(out, want)) { printf("FAIL plain '%s' -> '%s'\n", payload, out); assert(0); }
}
int main(void) {
    json("{\"state\":{\"power\":\"ON\"}}", "state.power", "ON");
    json("{\"items\":[{\"on\":true},{\"on\":false}]}", "items.1.on", "false");
    json("{\"items\":[{\"on\":true}]}", "items.0.on", "true");
    json("{\"t\":21.5,\"n\":3,\"z\":null}", "t", "21.5");
    json("{\"t\":21.5,\"n\":3,\"z\":null}", "n", "3");
    json("{\"t\":21.5,\"n\":3,\"z\":null}", "z", "");
    json("{\"a\":{\"b\":1}}", "a", "!");        /* an object is no scalar */
    json("{\"a\":1}", "b", "!");                /* missing */
    json("{\"a\":[1]}", "a.x", "!");            /* not an index */
    json("{\"a\":[1]}", "a.5", "!");            /* out of range */
    json("ON", "state", "!");                   /* no JSON */
    plain("  on \n", "on");
    char cut[200], out[MQTT_VALUE_MAX + 1];
    memset(cut, 'a', sizeof cut);
    cut[sizeof cut - 1] = 0;
    mqtt_plain_value(cut, strlen(cut), out, sizeof out);
    assert(strlen(out) == MQTT_VALUE_MAX);
    const char *umlauts = "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4";
    mqtt_plain_value(umlauts, strlen(umlauts), out, sizeof out);
    assert(strlen(out) % 2 == 0 && strlen(out) <= MQTT_VALUE_MAX); /* never inside a character */

    mqtt_settings_t s = {.port = 1883};
    assert(!mqtt_settings_apply(&s, "{\"host\":\" broker \",\"username\":\"me\",\"password\":\"secret\",\"tls\":true}"));
    assert(!strcmp(s.host, "broker") && s.tls && s.port == 1883 && !strcmp(s.password, "secret"));
    assert(!mqtt_settings_apply(&s, "{\"port\":8883}") && s.port == 8883 && !strcmp(s.password, "secret")); /* kept */
    assert(!mqtt_settings_apply(&s, "{\"password\":null}") && !s.password[0]);                           /* cleared */
    assert(mqtt_settings_apply(&s, "{\"port\":0}") && s.port == 8883);                                   /* unchanged on error */
    assert(mqtt_settings_apply(&s, "{\"tls\":\"yes\"}") && mqtt_settings_apply(&s, "[]") && mqtt_settings_apply(&s, "{\"host\":5}"));
    cJSON *pub = mqtt_settings_public(&s);
    assert(!cJSON_GetObjectItem(pub, "password") && cJSON_IsFalse(cJSON_GetObjectItem(pub, "password_set")));
    cJSON_Delete(pub);
    puts("mqtt ok");
}

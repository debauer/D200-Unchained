// Placeholder filling on the PC: no ESP-IDF needed, the clock is stubbed.
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "placeholders.h"
#include "mqtt.h"
#include "rules.h"
#include "wallclock.h"
static bool synced = false;
bool wallclock_now(struct tm *out) { if (!synced) return false; memset(out, 0, sizeof *out); out->tm_hour = 14; out->tm_min = 5; out->tm_mday = 17; out->tm_mon = 9; out->tm_wday = 5; return true; }
static bool have_light = false;
bool mqtt_value(const char *name, char *out, size_t size) {
    if (strcmp(name, "light") != 0 || !have_light) return false;
    snprintf(out, size, " ON ");
    return true;
}
static void check(const char *in, const char *want, cJSON *vars) {
    char out[128]; placeholders_fill(in, vars, out, sizeof out);
    if (strcmp(out, want)) { printf("FAIL '%s' -> '%s' (want '%s')\n", in, out, want); assert(0); }
}
int main(void) {
    cJSON *vars = cJSON_Parse("[{\"name\":\"cpu\"},{\"name\":\"ram\"}]");
    assert(placeholders_present("CPU {cpu}%") && placeholders_present("{time}"));
    assert(!placeholders_present("plain") && !placeholders_present("{{literal}}") && !placeholders_present("{Upper}") && !placeholders_present("{a b}"));
    check("CPU {cpu}%", "CPU ?%", vars);
    check("{{x}} {nope}", "{x} {nope}", vars);
    check("{time} {date}", "--:-- ---", vars);
    synced = true;
    check("{time} {date}", "14:05 Fri 17.10.", vars);
    check("a{time}b", "a14:05b", vars);
    cJSON *mq = cJSON_Parse("[{\"name\":\"light\",\"type\":\"mqtt\",\"topic\":\"t\"},{\"name\":\"cpu\",\"type\":\"command\"}]");
    check("L {light}", "L ...", mq);
    have_light = true;
    check("L {light} {cpu}", "L  ON  ?", mq);
    char *json = placeholders_values_json(mq);
    assert(strstr(json, "\"light\":\" ON \"") && strstr(json, "\"cpu\":\"?\"") && strstr(json, "\"time\""));
    free(json);
    cJSON *key = cJSON_Parse("{\"rules\":[{\"variable\":\"cpu\",\"value\":\"?\",\"color\":\"111111\"},"
                             "{\"variable\":\"light\",\"value\":\"ON\",\"color\":\"ffcc00\"}]}");
    const cJSON *rule = rules_match(mq, key);
    assert(rule && strcmp(cJSON_GetObjectItem(rule, "color")->valuestring, "111111") == 0); /* first match wins */
    cJSON_DeleteItemFromArray(cJSON_GetObjectItem(key, "rules"), 0);
    rule = rules_match(mq, key);
    assert(rule && strcmp(cJSON_GetObjectItem(rule, "color")->valuestring, "ffcc00") == 0); /* values are trimmed */
    have_light = false;
    assert(rules_match(mq, key) == NULL); /* no value yet: no match */
    assert(rules_match(mq, NULL) == NULL);
    puts("placeholders ok");
}

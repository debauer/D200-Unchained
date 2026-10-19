#include "rules.h"

#include <string.h>

#include "mqtt.h"
#include "placeholders.h"

static bool same_trimmed(const char *a, const char *b)
{
    const char *ends[2] = {a + strlen(a), b + strlen(b)};
    const char **starts[2] = {&a, &b};
    for (int i = 0; i < 2; i++) {
        while (**starts[i] == ' ' || **starts[i] == '\t') (*starts[i])++;
        while (ends[i] > *starts[i] && (ends[i][-1] == ' ' || ends[i][-1] == '\t' || ends[i][-1] == '\r' || ends[i][-1] == '\n'))
            ends[i]--;
    }
    return ends[0] - a == ends[1] - b && memcmp(a, b, (size_t)(ends[0] - a)) == 0;
}

const cJSON *rules_match(const cJSON *variables, const cJSON *key)
{
    const cJSON *rules = key ? cJSON_GetObjectItem(key, "rules") : NULL, *rule;
    cJSON_ArrayForEach(rule, rules) {
        const cJSON *variable = cJSON_GetObjectItem(rule, "variable"), *value = cJSON_GetObjectItem(rule, "value");
        char current[MQTT_VALUE_SIZE];
        if (!cJSON_IsString(variable) || !cJSON_IsString(value)) continue;
        if (placeholders_value(variables, variable->valuestring, current, sizeof current) != PLACEHOLDER_VALUE) continue;
        if (same_trimmed(current, value->valuestring)) return rule;
    }
    return NULL;
}

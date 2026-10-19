#pragma once

#include <stdbool.h>

#include "cJSON.h"

/* The broker settings (host/src/d200_deck/config/mqtt.py). They live in a file of their own, because they hold a
 * password and config.json is exported and served by the web API. */

typedef struct {
    char host[65]; /* empty: MQTT is off */
    char username[65];
    char password[65];
    char client_id[33];
    int port;
    bool tls;
} mqtt_settings_t;

void mqtt_settings_load(mqtt_settings_t *s);
bool mqtt_settings_save(const mqtt_settings_t *s);

/* Applies a PUT body: missing fields stay as they are, a missing `password` keeps the stored one and `null` clears
 * it. NULL on success, else the error text; `s` is unchanged on an error. */
const char *mqtt_settings_apply(mqtt_settings_t *s, const char *body);

/* Everything but the password, plus whether one is set. */
cJSON *mqtt_settings_public(const mqtt_settings_t *s);

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "mqttjson.h"

/* MQTT (docs/mqtt.md): the broker connection, the latest message of every `mqtt` variable and publishing. Incoming
 * messages only become text values; nothing received is executed. */

#define MQTT_VALUE_SIZE (MQTT_VALUE_MAX + 1)

/* Creates the lock; call before anything draws (the device task asks for values as soon as the D200 is up). */
void mqtt_init(void);

/* Reads the broker settings and connects when a host is set. Call once, after the storage is mounted and WiFi runs. */
void mqtt_start(void);

/* Subscribes to the topics of the `mqtt` variables of `config` (every page, so values are current after a page
 * change) and forgets the values of variables that are gone. */
void mqtt_configure(const cJSON *config);

/* The latest value of the mqtt variable `name`; false before its first message. */
bool mqtt_value(const char *name, char *out, size_t size);

/* PUT /api/mqtt: applies, stores and reconnects. NULL on success, else the error text for a 400. */
const char *mqtt_update(const char *body);

/* GET /api/mqtt and the `mqtt` event: the settings without the password plus the state. free() it. */
char *mqtt_json(void);

/* Queues a message. NULL on success, else why not. */
const char *mqtt_publish(const char *topic, const char *payload, int qos, bool retain);

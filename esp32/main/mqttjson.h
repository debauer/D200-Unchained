#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MQTT_VALUE_MAX 64 /* characters of a received value that are kept */
#define MQTT_ERROR_VALUE "!"

/* The scalar at the dotted `path` ("state.power", "items.0.on") of a JSON message, as host/service/mqtt.py shows it:
 * booleans as true/false, null as "", MQTT_ERROR_VALUE when the path does not exist or leads to an object or list.
 * `out` holds at least MQTT_VALUE_MAX + 1 bytes. */
void mqtt_json_value(const char *payload, const char *path, char *out, size_t size);

/* A message without a JSON path: the text, trimmed and cut to MQTT_VALUE_MAX bytes (never inside a UTF-8 character). */
void mqtt_plain_value(const char *payload, size_t length, char *out, size_t size);

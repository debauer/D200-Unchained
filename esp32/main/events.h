#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Server-sent events for GET /api/events (docs/web-api.md). */

/* Handler for GET /api/events: keeps the connection and sends the current state first. */
esp_err_t events_handler(httpd_req_t *req);

/* Queues `event: <name>` with `data: <json>`; publishers never wait for network I/O. */
void events_publish(const char *name, const char *json);

/* Creates the lock, bounded queue and sender task; call before anything publishes. */
void events_init(void);

/* Keepalive comment every 15 s; call once. */
void events_start(void);

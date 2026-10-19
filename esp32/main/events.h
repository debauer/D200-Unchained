#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Server-sent events for GET /api/events (docs/web-api.md). */

/* Handler for GET /api/events: keeps the connection and sends the current state first. */
esp_err_t events_handler(httpd_req_t *req);

/* Sends `event: <name>` with `data: <json>` to every connected client. */
void events_publish(const char *name, const char *json);

/* Keepalive comment every 15 s; call once. */
void events_start(void);

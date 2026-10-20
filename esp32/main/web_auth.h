#pragma once

#include "auth_core.h"
#include "esp_http_server.h"

esp_err_t web_auth_init(void);
bool web_auth_required(void);
uint64_t web_auth_ticket(httpd_req_t *req);
bool web_auth_ticket_valid(uint64_t ticket);
bool web_auth_guard(httpd_req_t *req);
esp_err_t web_auth_handler(httpd_req_t *req);

/* Local login worker interface; only call session creation after authentication. */
esp_err_t web_auth_login_init(void);
esp_err_t web_auth_login(httpd_req_t *req);
bool web_auth_create(httpd_req_t *req, uint64_t previous, const char *user);
bool web_auth_create_oidc(httpd_req_t *req, uint64_t previous, const char *user, const char *identity);
void web_auth_revoke(uint64_t ticket);
esp_err_t web_auth_status(httpd_req_t *req);

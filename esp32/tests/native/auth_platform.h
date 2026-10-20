#pragma once
#include "esp_http_server.h"
#include <stdint.h>
extern int64_t auth_test_now;
extern uint64_t auth_test_revoked;
void auth_test_wait(httpd_req_t *req);

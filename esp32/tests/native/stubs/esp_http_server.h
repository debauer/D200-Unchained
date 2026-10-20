#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef void *httpd_handle_t;
typedef struct httpd_req {
    void *user_ctx;
    size_t content_len;
    int fd;
    const char *host, *body;
    size_t offset;
    int fail_after;
} httpd_req_t;
typedef struct { esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *out, size_t size);
int httpd_req_to_sockfd(httpd_req_t *req);
int httpd_req_recv(httpd_req_t *req, char *out, size_t size);

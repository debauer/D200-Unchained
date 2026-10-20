#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
typedef void *httpd_handle_t;
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_RESP_USE_STRLEN -1
typedef struct httpd_req {
    void *user_ctx;
    size_t content_len;
    int fd;
    const char *host, *body, *origin, *cookie, *uri;
    const char *deck;
    int method;
    httpd_handle_t handle;
    size_t offset;
    int fail_after;
    struct httpd_req *parent;
    const char *header_names[8], *header_values[8];
    unsigned header_count;
    char response[1024], response_cookie[256], response_status[64];
    bool done;
} httpd_req_t;
typedef struct { esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *out, size_t size);
int httpd_req_to_sockfd(httpd_req_t *req);
int httpd_req_recv(httpd_req_t *req, char *out, size_t size);
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *text);
esp_err_t httpd_req_async_handler_begin(httpd_req_t *req, httpd_req_t **copy);
esp_err_t httpd_req_async_handler_complete(httpd_req_t *req);

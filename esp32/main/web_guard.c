#include "web.h"

#include <stdio.h>
#include <string.h>
#include "lwip/sockets.h"
#include "web_settings.h"
#include "web_auth.h"

bool web_host_allowed(httpd_req_t *req)
{
    char host[160], address[INET_ADDRSTRLEN], expected[160];
    struct sockaddr_in local;
    socklen_t length = sizeof local;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK ||
        getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &length) != 0 || local.sin_family != AF_INET ||
        !inet_ntop(AF_INET, &local.sin_addr, address, sizeof address)) return false;
    unsigned port = ntohs(local.sin_port);
    const char *allowed = WEB_HOST[0] ? WEB_HOST : address;
    snprintf(expected, sizeof expected, "%s:%u", allowed, port);
    return !strcmp(host, expected) || (port == (WEB_TLS ? 443 : 80) && !strcmp(host, allowed));
}

bool web_origin_allowed(httpd_req_t *req)
{
    size_t size = httpd_req_get_hdr_value_len(req, "Origin");
    if (!size) return true; /* CLI clients do not send Origin; X-Deck-Request is still required. */
    char origin[256], expected[256], host[128];
    if (size >= sizeof origin || httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) != ESP_OK) return false;
    if (WEB_ORIGIN[0]) return !strcmp(origin, WEB_ORIGIN);
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK) return false;
    snprintf(expected, sizeof expected, "%s://%s", WEB_TLS ? "https" : "http", host);
    return !strcmp(origin, expected);
}

int web_receive_body(httpd_req_t *req, char *body, size_t size)
{
    if (!req->content_len || req->content_len >= size) return -1;
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return -1;
        got += n;
    }
    body[got] = 0;
    return (int)got;
}

esp_err_t web_guarded_request(httpd_req_t *req)
{
    if (!web_host_allowed(req)) return web_json_error(req, "403 Forbidden", "unexpected Host header");
    if (req->method != HTTP_GET && !web_origin_allowed(req)) return web_json_error(req, "403 Forbidden", "unexpected Origin");
    if (!web_auth_guard(req)) return ESP_FAIL; /* Close after 401; a changing request may have an unread body. */
    if (!strncmp(req->uri, "/auth/", 6)) httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    const httpd_uri_t *route = req->user_ctx;
    return route->handler(req);
}

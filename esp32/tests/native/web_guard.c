#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "web.h"

static int denied, dispatched;
bool web_auth_guard(httpd_req_t *req) { (void)req; return true; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value) { (void)req; (void)name; (void)value; return ESP_OK; }
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name)
{
    assert(!strcmp(name, "Origin"));
    return req->origin ? strlen(req->origin) : 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *out, size_t size)
{
    const char *value = !strcmp(name, "Host") ? req->host : req->origin;
    if (!value || strlen(value) >= size) return ESP_FAIL;
    strcpy(out, value);
    return ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *req) { return req->fd; }
int httpd_req_recv(httpd_req_t *req, char *out, size_t size)
{
    assert(size > 0);
    if (req->fail_after >= 0 && req->offset >= (size_t)req->fail_after) return 0;
    *out = req->body[req->offset++]; /* Every receive returns only one byte. */
    return 1;
}
esp_err_t web_json_error(httpd_req_t *req, const char *status, const char *message)
{
    (void)req; (void)message;
    assert(!strcmp(status, "403 Forbidden"));
    denied++;
    return ESP_OK;
}
static esp_err_t route(httpd_req_t *req) { (void)req; dispatched++; return ESP_OK; }

int main(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (struct sockaddr *)&address, sizeof address));
    socklen_t length = sizeof address;
    assert(!getsockname(fd, (struct sockaddr *)&address, &length));
    char host[64];
    snprintf(host, sizeof host, "127.0.0.1:%u", ntohs(address.sin_port));
    const httpd_uri_t target = {.handler = route};
    httpd_req_t req = {.fd = fd, .host = host, .user_ctx = (void *)&target, .uri = "/api/info"};
    assert(web_host_allowed(&req));
    assert(web_guarded_request(&req) == ESP_OK && dispatched == 1 && denied == 0);
    const char *bad[] = {NULL, "evil.example", "127.0.0.1.evil.example", "127.0.0.1:1", "127.0.0.1:80@evil.example"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        req.host = bad[i];
        assert(!web_host_allowed(&req));
        assert(web_guarded_request(&req) == ESP_OK);
    }
    assert(dispatched == 1 && denied == 5);
    req.host = host;
    char origin[96];
    snprintf(origin, sizeof origin, "http://%s", host);
    req.origin = origin;
    assert(web_origin_allowed(&req));
    req.origin = "http://evil.example";
    assert(!web_origin_allowed(&req));
    req.method = HTTP_POST;
    assert(web_guarded_request(&req) == ESP_OK && dispatched == 1 && denied == 6);
    close(fd);

    req = (httpd_req_t){.body = "{\"page\":\"lights\"}", .content_len = 17, .fail_after = -1};
    char body[64];
    assert(web_receive_body(&req, body, sizeof body) == 17 && !strcmp(body, req.body));
    req.offset = 0;
    req.fail_after = 3;
    assert(web_receive_body(&req, body, sizeof body) == -1);
    req.offset = 0;
    assert(web_receive_body(&req, body, 4) == -1 && !req.offset);
    puts("web guard ok");
}

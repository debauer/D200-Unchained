#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "auth_password.h"
#include "auth_platform.h"
#include "web.h"
#include "web_auth.h"
#include "web_settings.h"
#include "cJSON.h"

static httpd_req_t request(int fd, const char *uri, const char *body, const char *cookie)
{
    return (httpd_req_t){.fd = fd, .uri = uri, .body = body, .content_len = body ? strlen(body) : 0,
                        .cookie = cookie, .method = body ? HTTP_POST : HTTP_GET, .deck = "1"};
}
int main(void)
{
    assert(auth_password_verify("admin", "test-only-password"));
    assert(!auth_password_verify("other", "test-only-password"));
    assert(!auth_password_verify("admin", "wrong"));
    assert(web_auth_init() == ESP_OK);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(listener, (struct sockaddr *)&address, sizeof address) && !listen(listener, 1));
    socklen_t size = sizeof address;
    assert(!getsockname(listener, (struct sockaddr *)&address, &size));
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(!connect(client, (struct sockaddr *)&address, sizeof address));
    int fd = accept(listener, NULL, NULL);
    assert(fd >= 0);
    httpd_req_t req = request(fd, "/api/config", NULL, NULL);
    assert(!web_auth_guard(&req) && !strcmp(req.response_status, "401 Unauthorized"));
    req = request(fd, "/auth/session", NULL, NULL);
    assert(web_auth_handler(&req) == ESP_OK && strstr(req.response, "\"authenticated\":false"));
    req = request(fd, "/auth/login", "{\"username\":\"admin\",\"password\":\"wrong\"}", NULL);
    assert(web_auth_handler(&req) == ESP_OK);
    auth_test_wait(&req);
    assert(!strcmp(req.response_status, "401 Unauthorized") && !req.response_cookie[0]);
    req = request(fd, "/auth/login", "{\"username\":\"admin\",\"password\":\"test-only-password\"}", NULL);
    assert(web_auth_handler(&req) == ESP_OK);
    auth_test_wait(&req);
    cJSON *json = cJSON_Parse(req.response);
    assert(json && cJSON_IsTrue(cJSON_GetObjectItem(json, "authenticated")));
    assert(cJSON_GetObjectItem(json, "session_ttl_seconds")->valueint == 172800);
    cJSON_Delete(json);
    assert(strstr(req.response_cookie, "HttpOnly") && strstr(req.response_cookie, "SameSite=Lax") && strstr(req.response_cookie, "Max-Age=172800"));
    assert((strstr(req.response_cookie, "; Secure") != NULL) == WEB_TLS);
    char cookie[256];
    strcpy(cookie, req.response_cookie);
    *strchr(cookie, ';') = 0;
    req = request(fd, "/api/config", NULL, cookie);
    uint64_t ticket = web_auth_ticket(&req);
    assert(ticket && web_auth_guard(&req) && web_auth_ticket_valid(ticket));
    req = request(fd, "/auth/session", NULL, cookie);
    assert(web_auth_handler(&req) == ESP_OK && strstr(req.response, "\"username\":\"admin\""));
    auth_test_now += (int64_t)WEB_SESSION_TTL * 1000000;
    assert(!web_auth_ticket_valid(ticket));
    req = request(fd, "/api/keys/0/icon", NULL, cookie);
    assert(!web_auth_guard(&req));
    req = request(fd, "/auth/login", "{\"username\":\"admin\",\"password\":\"test-only-password\"}", NULL);
    assert(web_auth_handler(&req) == ESP_OK);
    auth_test_wait(&req);
    strcpy(cookie, req.response_cookie);
    *strchr(cookie, ';') = 0;
    req = request(fd, "/auth/logout", "{}", cookie);
    ticket = web_auth_ticket(&req);
    assert(ticket && web_auth_handler(&req) == ESP_OK);
    assert(auth_test_revoked == ticket && !web_auth_ticket_valid(ticket));
    assert(strstr(req.response_cookie, "Max-Age=0"));
    req = request(fd, "/auth/login", "{}", NULL);
    req.deck = NULL;
    assert(web_auth_handler(&req) == ESP_OK && !strcmp(req.response_status, "403 Forbidden"));
    const char *bad[] = {"{\"username\":\"admin\",\"password\":{}}", "{\"username\":\"admin\",\"password\":\"x\\u0000y\"}"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        req = request(fd, "/auth/login", bad[i], NULL);
        assert(web_auth_handler(&req) == ESP_OK && !strcmp(req.response_status, "400 Bad Request"));
    }
    auth_test_now += 60000000;
    for (int i = 0; i < 5; i++) {
        req = request(fd, "/auth/login", "{\"username\":\"admin\",\"password\":\"wrong\"}", NULL);
        assert(web_auth_handler(&req) == ESP_OK);
        auth_test_wait(&req);
        assert(!strcmp(req.response_status, "401 Unauthorized"));
    }
    req = request(fd, "/auth/login", "{}", NULL);
    assert(web_auth_handler(&req) == ESP_OK && !strcmp(req.response_status, "429 Too Many Requests"));
    close(fd); close(client); close(listener);
    puts(WEB_TLS ? "HTTPS auth cookies/API ok" : "HTTP auth cookies/API ok");
}

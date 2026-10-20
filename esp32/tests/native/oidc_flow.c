#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "auth_platform.h"
#include "oidc_provider.h"
#include "web_auth.h"
#include "web_settings.h"

static httpd_req_t request(int fd, const char *uri, const char *body, const char *cookie)
{
    return (httpd_req_t){.fd = fd, .uri = uri, .body = body, .content_len = body ? strlen(body) : 0,
                        .cookie = cookie, .method = body ? HTTP_POST : HTTP_GET, .deck = "1"};
}
static void pair(const httpd_req_t *req, const char *name, char out[256])
{
    const char *value = req->response_cookie;
    if (strncmp(value, name, strlen(name))) value = req->response_cookie2;
    assert(!strncmp(value, name, strlen(name)) && value[strlen(name)] == '=');
    snprintf(out, 256, "%s", value);
    *strchr(out, ';') = 0;
}
static void begin(int fd, char cookie[256], char state[65])
{
    auth_test_now += 60000000; /* Start endpoint's source budget is independent of replay tests. */
    httpd_req_t req = request(fd, "/auth/oidc/start", "{}", NULL);
    assert(web_auth_handler(&req) == ESP_OK);
    auth_test_wait(&req);
    assert(!strcmp(req.response_status, "200 OK"));
    pair(&req, WEB_OIDC_COOKIE, cookie);
    cJSON *json = cJSON_Parse(req.response);
    const char *url = cJSON_GetObjectItem(json, "redirect")->valuestring, *query = strchr(url, '?') + 1;
    char callback[256], method[16];
    assert(oidc_query(query, "state", state, 65) == 1);
    assert(oidc_query(query, "nonce", test_nonce, sizeof test_nonce) == 1);
    assert(oidc_query(query, "code_challenge", test_challenge, sizeof test_challenge) == 1);
    assert(oidc_query(query, "redirect_uri", callback, sizeof callback) == 1 && !strcmp(callback, WEB_ORIGIN OIDC_CALLBACK));
    assert(oidc_query(query, "code_challenge_method", method, sizeof method) == 1 && !strcmp(method, "S256"));
    assert((strstr(req.response_cookie, "; Secure") != NULL) == WEB_TLS);
    cJSON_Delete(json);
}
static httpd_req_t finish(int fd, const char *cookie, const char *state, const char *extra)
{
    char uri[2048];
    snprintf(uri, sizeof uri, OIDC_CALLBACK "?code=one-use-code&state=%s%s", state, extra ? extra : "");
    httpd_req_t req = request(fd, uri, NULL, cookie);
    assert(web_auth_handler(&req) == ESP_OK);
    if (req.asynchronous) auth_test_wait(&req);
    assert(!strcmp(req.response_status, "302 Found"));
    req.uri = NULL; /* The copied test response must not retain this function's stack URI. */
    return req;
}

static int64_t clock_now(void) { return test_unix; }

static void cache(void)
{
    oidc_client_t client = {.issuer = WEB_OIDC_ISSUER, .client_id = "deck", .scopes = "openid", .redirect_uri = WEB_ORIGIN OIDC_CALLBACK};
    oidc_transaction_t tx = { .state = "state", .nonce = "nonce", .verifier = "verifier" };
    assert(oidc_pkce(tx.verifier, test_challenge));
    oidc_identity_t identity;
    assert(!oidc_exchange(&client, "code", &tx, 1, clock_now, &identity));
    unsigned keys = test_key_requests;
    strcpy(test_kid, "k2");
    assert(!oidc_exchange(&client, "code", &tx, 2, clock_now, &identity) && test_key_requests == keys + 1);
    strcpy(test_kid, "k3");
    assert(!strcmp(oidc_exchange(&client, "code", &tx, 3, clock_now, &identity), "token") && test_key_requests == keys + 1);
    assert(!oidc_exchange(&client, "code", &tx, 30000003, clock_now, &identity) && test_key_requests == keys + 2);
    oidc_client_free(&client);
    test_bad_metadata = true;
    assert(!oidc_authorization(&client, &tx, 1));
    test_bad_metadata = false;
    strcpy(test_kid, "k1");
    test_late_keys = true;
    assert(!strcmp(oidc_exchange(&client, "code", &tx, 40000000, clock_now, &identity), "token"));
    test_late_keys = false;
    test_unix -= 1000;
    oidc_client_free(&client); /* Expiry must be checked after provider I/O, not before it. */
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_provider_init(argv[1]);
    cache();
    assert(web_auth_init() == ESP_OK);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof address;
    assert(!bind(listener, (struct sockaddr *)&address, size) && !listen(listener, 1) && !getsockname(listener, (struct sockaddr *)&address, &size));
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(!connect(client, (struct sockaddr *)&address, size));
    int fd = accept(listener, NULL, NULL);
    assert(fd >= 0);
    httpd_req_t req = request(fd, "/auth/session", NULL, NULL);
    assert(web_auth_handler(&req) == ESP_OK && strstr(req.response, "\"oidc\""));
    test_unix = 0;
    req = request(fd, "/auth/oidc/start", "{}", NULL);
    assert(web_auth_handler(&req) == ESP_OK);
    auth_test_wait(&req);
    assert(!strcmp(req.response_status, "503 Service Unavailable"));
    req = request(fd, "/auth/login", "{\"username\":\"admin\",\"password\":\"test-only-password\"}", NULL);
    assert(web_auth_handler(&req) == ESP_OK);
    auth_test_wait(&req);
    assert(!strcmp(req.response_status, "200 OK"));
    char local_session[256];
    pair(&req, WEB_COOKIE_NAME, local_session);
    req = request(fd, "/api/info", NULL, local_session);
    uint64_t previous = web_auth_ticket(&req);
    assert(previous);
    test_unix = 1700001000;
    char cookie[256], state[65];
    begin(fd, cookie, state);
    unsigned exchanges = test_exchanges;
    req = finish(fd, NULL, state, NULL);
    assert(!strcmp(req.response_location, "/?auth_error=expired") && test_exchanges == exchanges);
    req = finish(fd, cookie, "wrong-state", NULL);
    assert(!strcmp(req.response_location, "/?auth_error=expired"));
    req = finish(fd, cookie, state, NULL);
    assert(!strcmp(req.response_location, "/?auth_error=expired") && test_exchanges == exchanges);
    begin(fd, cookie, state);
    char combined[600];
    snprintf(combined, sizeof combined, "%s; %s", local_session, cookie);
    req = finish(fd, combined, state, NULL);
    assert(!strcmp(req.response_location, "/") && test_exchanges == exchanges + 1);
    assert(!web_auth_ticket_valid(previous) && auth_test_revoked == previous);
    assert(strstr(req.response_cookie, "Max-Age=0") && req.response_cookie2[0]);
    char session[256];
    pair(&req, WEB_COOKIE_NAME, session);
    req = request(fd, "/auth/session", NULL, session);
    assert(web_auth_handler(&req) == ESP_OK && strstr(req.response, "\"authenticated\":true"));
    cJSON *json = cJSON_Parse(req.response);
    char oidc_identity[65];
    strcpy(oidc_identity, cJSON_GetObjectItem(json, "identity")->valuestring);
    cJSON_Delete(json);
    req = finish(fd, cookie, state, NULL);
    assert(!strcmp(req.response_location, "/?auth_error=expired"));
    begin(fd, cookie, state);
    test_forbidden = true;
    req = finish(fd, cookie, state, NULL);
    assert(!strcmp(req.response_location, "/?auth_error=forbidden") && !req.response_cookie2[0]);
    test_forbidden = false;
    begin(fd, cookie, state);
    test_bad_nonce = true;
    req = finish(fd, cookie, state, NULL);
    assert(!strcmp(req.response_location, "/?auth_error=token"));
    test_bad_nonce = false;
    begin(fd, cookie, state);
    test_bad_signature = true;
    req = finish(fd, cookie, state, NULL);
    assert(!strcmp(req.response_location, "/?auth_error=token"));
    test_bad_signature = false;
    begin(fd, cookie, state);
    req = finish(fd, cookie, state, "&code=duplicate");
    assert(!strcmp(req.response_location, "/?auth_error=token"));
    begin(fd, cookie, state);
    req = finish(fd, cookie, state, "&state=duplicate");
    assert(!strcmp(req.response_location, "/?auth_error=expired"));
    begin(fd, cookie, state);
    req = finish(fd, cookie, state, "&iss=http%3A%2F%2Fevil.example");
    assert(!strcmp(req.response_location, "/?auth_error=token"));
    begin(fd, cookie, state);
    req = finish(fd, cookie, state, "&error=access_denied");
    assert(!strcmp(req.response_location, "/?auth_error=denied"));
    begin(fd, cookie, state);
    auth_test_now += (int64_t)OIDC_TX_SECONDS * 1000000;
    req = finish(fd, cookie, state, NULL);
    assert(!strcmp(req.response_location, "/?auth_error=expired"));
    test_down = true;
    auth_test_now += 3600000001LL;
    req = request(fd, "/auth/oidc/start", "{}", NULL);
    assert(web_auth_handler(&req) == ESP_OK);
    auth_test_wait(&req);
    assert(!strcmp(req.response_status, "502 Bad Gateway"));
    req = request(fd, "/auth/login", "{\"username\":\"admin\",\"password\":\"test-only-password\"}", NULL);
    assert(web_auth_handler(&req) == ESP_OK);
    auth_test_wait(&req);
    assert(!strcmp(req.response_status, "200 OK"));
    json = cJSON_Parse(req.response);
    assert(strcmp(oidc_identity, cJSON_GetObjectItem(json, "identity")->valuestring));
    cJSON_Delete(json);
    close(fd); close(client); close(listener);
    test_provider_free();
    puts(WEB_TLS ? "HTTPS OIDC flow, replay/role checks, rotation and local fallback ok" : "HTTP OIDC flow, replay/role checks, rotation and local fallback ok");
}

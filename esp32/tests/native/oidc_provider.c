/* Mock provider HTTP only; RSA signing/verifying and all firmware flow code are real. */
#include "oidc_provider.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "web_settings.h"

char test_nonce[65] = "nonce", test_challenge[44], test_kid[16] = "k1";
bool test_down, test_forbidden, test_bad_nonce, test_bad_signature, test_bad_metadata;
bool test_late_keys;
unsigned test_discoveries, test_key_requests, test_exchanges;
int64_t test_unix = 1700001000;
static cJSON *s_jwks;
static mbedtls_pk_context s_key;
time_t oidc_test_time(time_t *out) { if (out) *out = test_unix; return test_unix; }

static char *read_file(const char *directory, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    FILE *f = fopen(path, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long size = ftell(f);
    assert(size > 0 && !fseek(f, 0, SEEK_SET));
    char *data = malloc(size + 1);
    assert(data && fread(data, 1, size, f) == (size_t)size);
    data[size] = 0;
    fclose(f);
    return data;
}
static int rng(void *arg, unsigned char *out, size_t size)
{
    (void)arg;
    for (size_t i = 0; i < size; i++) out[i] = (unsigned char)rand();
    return 0;
}
void test_provider_init(const char *directory)
{
    char *keys = read_file(directory, "jwks.json"), *key = read_file(directory, "key.pem");
    s_jwks = cJSON_Parse(keys);
    assert(s_jwks);
    mbedtls_pk_init(&s_key);
    assert(!mbedtls_pk_parse_key(&s_key, (const unsigned char *)key, strlen(key) + 1, NULL, 0, rng, NULL));
    free(key); free(keys);
}
void test_provider_free(void) { mbedtls_pk_free(&s_key); cJSON_Delete(s_jwks); }

static char *signed_token(void)
{
    cJSON *claims = cJSON_CreateObject(), *header = cJSON_CreateObject();
    cJSON_AddStringToObject(header, "alg", "RS256");
    cJSON_AddStringToObject(header, "kid", test_kid);
    cJSON_AddStringToObject(claims, "iss", WEB_OIDC_ISSUER);
    cJSON_AddStringToObject(claims, "sub", "subject-alice");
    cJSON_AddStringToObject(claims, "preferred_username", "admin"); /* Deliberately collides with the local display name. */
    cJSON_AddStringToObject(claims, "aud", "deck");
    cJSON_AddStringToObject(claims, "nonce", test_bad_nonce ? "wrong" : test_nonce);
    cJSON_AddNumberToObject(claims, "iat", (double)test_unix - 10);
    cJSON_AddNumberToObject(claims, "exp", (double)test_unix + 600);
    cJSON *roles = cJSON_AddArrayToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(claims, "resource_access"), "deck"), "roles");
    cJSON_AddItemToArray(roles, cJSON_CreateString(test_forbidden ? "viewer" : "deck-admin"));
    char *h = cJSON_PrintUnformatted(header), *p = cJSON_PrintUnformatted(claims);
    char first[2048], second[4096];
    assert(h && p && oidc_b64_encode((uint8_t *)h, strlen(h), first, sizeof first) &&
           oidc_b64_encode((uint8_t *)p, strlen(p), second, sizeof second));
    cJSON_Delete(claims); cJSON_Delete(header); free(h); free(p);
    char *token = malloc(8192);
    assert(token);
    int size = snprintf(token, 8192, "%s.%s", first, second);
    uint8_t digest[32], signature[512];
    size_t length;
    assert(!mbedtls_sha256((const uint8_t *)token, size, digest, 0));
    assert(!mbedtls_pk_sign(&s_key, MBEDTLS_MD_SHA256, digest, sizeof digest, signature, sizeof signature, &length, rng, NULL));
    if (test_bad_signature) signature[0] ^= 1;
    token[size++] = '.';
    assert(oidc_b64_encode(signature, length, token + size, 8192 - size));
    return token;
}

cJSON *oidc_http(const char *url, const char *form, size_t limit)
{
    (void)limit;
    if (test_down) return NULL;
    if (strstr(url, "/.well-known/")) {
        test_discoveries++;
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "issuer", WEB_OIDC_ISSUER);
        cJSON_AddStringToObject(doc, "authorization_endpoint", WEB_OIDC_ISSUER "/auth");
        cJSON_AddStringToObject(doc, "token_endpoint", test_bad_metadata ? "http://evil.example/token" : WEB_OIDC_ISSUER "/token");
        cJSON_AddStringToObject(doc, "jwks_uri", WEB_OIDC_ISSUER "/certs");
        return doc;
    }
    if (!strcmp(url, WEB_OIDC_ISSUER "/certs")) {
        if (test_late_keys) test_unix += 1000;
        test_key_requests++;
        cJSON *keys = cJSON_Duplicate(s_jwks, true), *key = cJSON_GetArrayItem(cJSON_GetObjectItem(keys, "keys"), 0);
        cJSON_ReplaceItemInObject(key, "kid", cJSON_CreateString(test_kid));
        return keys;
    }
    assert(!strcmp(url, WEB_OIDC_ISSUER "/token") && form);
    test_exchanges++;
    char verifier[129], challenge[44], redirect[512], client[128], grant[64];
    assert(oidc_query(form, "code_verifier", verifier, sizeof verifier) == 1 && oidc_pkce(verifier, challenge));
    assert(!strcmp(challenge, test_challenge));
    assert(oidc_query(form, "redirect_uri", redirect, sizeof redirect) == 1 && !strcmp(redirect, WEB_ORIGIN OIDC_CALLBACK));
    assert(oidc_query(form, "client_id", client, sizeof client) == 1 && !strcmp(client, "deck"));
    assert(oidc_query(form, "grant_type", grant, sizeof grant) == 1 && !strcmp(grant, "authorization_code"));
    cJSON *reply = cJSON_CreateObject();
    char *token = signed_token();
    cJSON_AddStringToObject(reply, "id_token", token);
    cJSON_AddStringToObject(reply, "access_token", "not-needed");
    cJSON_AddStringToObject(reply, "refresh_token", "not-retained");
    free(token);
    return reply;
}

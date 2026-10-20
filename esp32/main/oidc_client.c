#include "oidc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/platform_util.h"

#define CACHE_US 3600000000ULL
#define REFRESH_US 30000000ULL
static const cJSON *field(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }

static bool endpoints(oidc_client_t *client, uint64_t now)
{
    if (client->discovered && now - client->discovery_at < CACHE_US) return true;
    char url[OIDC_MAX_URL];
    int size = snprintf(url, sizeof url, "%s/.well-known/openid-configuration", client->issuer);
    if (size < 0 || (size_t)size >= sizeof url) return false;
    cJSON *doc = oidc_http(url, NULL, 16384);
    const cJSON *issuer = field(doc, "issuer"), *authorize = field(doc, "authorization_endpoint");
    const cJSON *token = field(doc, "token_endpoint"), *keys = field(doc, "jwks_uri");
    bool valid = cJSON_IsString(issuer) && !strcmp(issuer->valuestring, client->issuer) &&
                 cJSON_IsString(authorize) && oidc_same_origin(authorize->valuestring, client->issuer) &&
                 cJSON_IsString(token) && oidc_same_origin(token->valuestring, client->issuer) &&
                 cJSON_IsString(keys) && oidc_same_origin(keys->valuestring, client->issuer);
    if (valid) {
        /* A changed endpoint invalidates the signing-key cache, even before its ordinary expiry. */
        if (strcmp(client->jwks_url, keys->valuestring)) {
            cJSON_Delete(client->jwks);
            client->jwks = NULL;
            client->keys_loaded = false;
        }
        strcpy(client->authorize, authorize->valuestring);
        strcpy(client->token, token->valuestring);
        strcpy(client->jwks_url, keys->valuestring);
        client->discovery_at = now;
        client->discovered = true;
    }
    cJSON_Delete(doc);
    return valid;
}

static char *form(const char *const pairs[][2], size_t count)
{
    char *body = calloc(1, 8192);
    if (!body) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        char *value = oidc_url_encode(pairs[i][1]);
        int n = value ? snprintf(body + used, 8192 - used, "%s%s=%s", i ? "&" : "", pairs[i][0], value) : -1;
        if (value) { mbedtls_platform_zeroize(value, strlen(value)); free(value); }
        if (n < 0 || (size_t)n >= 8192 - used) { mbedtls_platform_zeroize(body, 8192); free(body); return NULL; }
        used += n;
    }
    return body;
}

char *oidc_authorization(oidc_client_t *client, const oidc_transaction_t *tx, uint64_t now)
{
    if (!endpoints(client, now)) return NULL;
    char challenge[44];
    if (!oidc_pkce(tx->verifier, challenge)) return NULL;
    const char *const pairs[][2] = {
        {"response_type", "code"}, {"client_id", client->client_id}, {"redirect_uri", client->redirect_uri},
        {"scope", client->scopes}, {"state", tx->state}, {"nonce", tx->nonce},
        {"code_challenge", challenge}, {"code_challenge_method", "S256"},
    };
    char *query = form(pairs, sizeof pairs / sizeof pairs[0]);
    if (!query) return NULL;
    size_t size = strlen(client->authorize) + strlen(query) + 2;
    char *url = size <= 4096 ? malloc(size) : NULL;
    if (url) snprintf(url, size, "%s%c%s", client->authorize, strchr(client->authorize, '?') ? '&' : '?', query);
    free(query);
    return url;
}

static const char *key_set(oidc_client_t *client, const char *kid, uint64_t now)
{
    bool stale = !client->keys_loaded || now - client->keys_at >= CACHE_US;
    bool unknown = client->keys_loaded && !oidc_jwk(client->jwks, kid);
    bool forced = unknown && (!client->forced || now - client->forced_at >= REFRESH_US);
    if (unknown && !stale && !forced) return "token";
    if (stale || forced) {
        if (forced && !stale) { client->forced = true; client->forced_at = now; }
        cJSON *keys = oidc_http(client->jwks_url, NULL, 65536);
        const cJSON *list = field(keys, "keys");
        if (!cJSON_IsArray(list) || !cJSON_GetArraySize(list) || cJSON_GetArraySize(list) > 16) {
            cJSON_Delete(keys);
            return "provider";
        }
        cJSON_Delete(client->jwks);
        client->jwks = keys;
        client->keys_at = now;
        client->keys_loaded = true;
    }
    return oidc_jwk(client->jwks, kid) ? NULL : "token";
}

const char *oidc_exchange(oidc_client_t *client, const char *code, const oidc_transaction_t *tx,
                          uint64_t now, int64_t (*clock)(void), oidc_identity_t *out)
{
    if (clock() < 1700000000) return "clock";
    if (!code[0] || strlen(code) > 1024) return "token";
    if (!endpoints(client, now)) return "provider";
    const char *const pairs[][2] = {
        {"grant_type", "authorization_code"}, {"client_id", client->client_id}, {"code", code},
        {"redirect_uri", client->redirect_uri}, {"code_verifier", tx->verifier},
    };
    char *body = form(pairs, sizeof pairs / sizeof pairs[0]);
    if (!body) return "provider";
    cJSON *reply = oidc_http(client->token, body, 32768);
    mbedtls_platform_zeroize(body, strlen(body));
    free(body);
    const cJSON *token = field(reply, "id_token");
    const char *error = "token";
    char kid[129];
    if (!reply) error = "provider";
    else if (cJSON_IsString(token) && oidc_jwt_kid(token->valuestring, kid)) {
        error = key_set(client, kid, now);
        if (!error) error = oidc_verify(token->valuestring, client->jwks, client->issuer, client->client_id, tx->nonce, clock(), out);
    }
    static const char *const secrets[] = {"id_token", "access_token", "refresh_token"};
    for (unsigned i = 0; i < sizeof secrets / sizeof secrets[0]; i++) {
        const cJSON *secret = field(reply, secrets[i]);
        if (cJSON_IsString(secret)) mbedtls_platform_zeroize(secret->valuestring, strlen(secret->valuestring));
    }
    cJSON_Delete(reply);
    return error;
}

void oidc_client_free(oidc_client_t *client)
{
    cJSON_Delete(client->jwks);
    client->jwks = NULL;
    client->keys_loaded = client->discovered = false;
}

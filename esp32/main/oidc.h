#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define OIDC_MAX_TOKEN 16384
#define OIDC_MAX_URL 512
#define OIDC_TX_COUNT 4
#define OIDC_TX_SECONDS 600
#define OIDC_CALLBACK "/auth/oidc/callback"

typedef struct { char username[65], identity[65]; } oidc_identity_t;
typedef struct {
    char state[65], nonce[65], verifier[65];
    uint8_t cookie_hash[32];
    uint64_t expires;
} oidc_transaction_t;
typedef struct { oidc_transaction_t items[OIDC_TX_COUNT]; } oidc_transactions_t;

bool oidc_b64_encode(const uint8_t *data, size_t length, char *out, size_t capacity);
bool oidc_b64_decode(const char *text, size_t length, uint8_t *out, size_t capacity, size_t *written);
bool oidc_pkce(const char *verifier, char challenge[44]);
char *oidc_url_encode(const char *text);
/* Strict JSON: bounded depth, valid UTF-8, no duplicate object names or embedded NULs. */
cJSON *oidc_json(const char *text, size_t length);
bool oidc_same_origin(const char *url, const char *issuer);
/* 1 found, 0 absent, -1 malformed/duplicate (including encoded parameter names). */
int oidc_query(const char *query, const char *name, char *out, size_t capacity);

bool oidc_tx_create(oidc_transactions_t *store, const uint8_t entropy[128], uint64_t now,
                    char cookie[65], oidc_transaction_t *out);
bool oidc_tx_take(oidc_transactions_t *store, const char *cookie, const char *state, uint64_t now, oidc_transaction_t *out);

bool oidc_jwt_kid(const char *token, char kid[129]);
const cJSON *oidc_jwk(const cJSON *jwks, const char *kid);
/* NULL on success, otherwise a generic browser error code (token/forbidden/clock). */
const char *oidc_verify(const char *token, const cJSON *jwks, const char *issuer, const char *client_id,
                        const char *nonce, int64_t now, oidc_identity_t *out);
const char *oidc_claims(const cJSON *claims, const char *issuer, const char *client_id,
                        const char *nonce, int64_t now, oidc_identity_t *out);

/* Provider HTTP adapter: returns an owned JSON object; follows no redirects. */
cJSON *oidc_http(const char *url, const char *form, size_t limit);

typedef struct {
    const char *issuer, *client_id, *scopes, *redirect_uri;
    char authorize[OIDC_MAX_URL], token[OIDC_MAX_URL], jwks_url[OIDC_MAX_URL];
    cJSON *jwks;
    uint64_t discovery_at, keys_at, forced_at;
    bool discovered, keys_loaded, forced;
} oidc_client_t;
/* The caller serializes this client (one OIDC worker). */
char *oidc_authorization(oidc_client_t *client, const oidc_transaction_t *tx, uint64_t now);
const char *oidc_exchange(oidc_client_t *client, const char *code, const oidc_transaction_t *tx,
                          uint64_t now, int64_t (*clock)(void), oidc_identity_t *out);
void oidc_client_free(oidc_client_t *client);

#include "oidc.h"

#include <stdlib.h>
#include <string.h>
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"

static const cJSON *field(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static bool text_is(const cJSON *v, const char *text) { return cJSON_IsString(v) && !strcmp(v->valuestring, text); }

static cJSON *segment(const char *text, size_t length, size_t limit)
{
    if (!length || length > limit * 4 / 3 + 4) return NULL;
    uint8_t *bytes = malloc(limit);
    size_t size;
    cJSON *json = bytes && oidc_b64_decode(text, length, bytes, limit, &size) ? oidc_json((char *)bytes, size) : NULL;
    free(bytes);
    return json;
}

bool oidc_jwt_kid(const char *token, char kid[129])
{
    size_t size = strlen(token);
    const char *dot = strchr(token, '.');
    if (!dot || size > OIDC_MAX_TOKEN) return false;
    cJSON *header = segment(token, dot - token, 1024);
    const cJSON *id = field(header, "kid");
    const cJSON *type = field(header, "typ");
    bool valid = text_is(field(header, "alg"), "RS256") && cJSON_IsString(id) && id->valuestring[0] &&
                 strlen(id->valuestring) <= 128 && !field(header, "crit") && !field(header, "b64") &&
                 !field(header, "jku") && !field(header, "jwk") && !field(header, "x5u") &&
                 (!type || text_is(type, "JWT") || text_is(type, "application/jwt"));
    if (valid) strcpy(kid, id->valuestring);
    cJSON_Delete(header);
    return valid;
}

const cJSON *oidc_jwk(const cJSON *jwks, const char *kid)
{
    const cJSON *keys = field(jwks, "keys"), *key, *found = NULL;
    if (!cJSON_IsArray(keys) || cJSON_GetArraySize(keys) > 16) return NULL;
    cJSON_ArrayForEach(key, keys) {
        if (!text_is(field(key, "kid"), kid)) continue;
        if (found || !text_is(field(key, "kty"), "RSA")) return NULL;
        if (field(key, "alg") && !text_is(field(key, "alg"), "RS256")) return NULL;
        if (field(key, "use") && !text_is(field(key, "use"), "sig")) return NULL;
        const cJSON *ops = field(key, "key_ops");
        if (ops) {
            if (!cJSON_IsArray(ops)) return NULL;
            bool verify = false;
            const cJSON *op;
            cJSON_ArrayForEach(op, ops) if (text_is(op, "verify")) verify = true;
            if (!verify) return NULL;
        }
        found = key;
    }
    return found;
}

static bool signature(const cJSON *key, const char *token, size_t signed_length, const char *encoded)
{
    const cJSON *n = field(key, "n"), *e = field(key, "e");
    if (!cJSON_IsString(n) || !cJSON_IsString(e)) return false;
    uint8_t modulus[512], exponent[4], sig[512], hash[32];
    size_t n_size, e_size, sig_size;
    if (!oidc_b64_decode(n->valuestring, strlen(n->valuestring), modulus, sizeof modulus, &n_size) || n_size < 256 ||
        !oidc_b64_decode(e->valuestring, strlen(e->valuestring), exponent, sizeof exponent, &e_size) || !e_size ||
        !oidc_b64_decode(encoded, strlen(encoded), sig, sizeof sig, &sig_size) || sig_size != n_size ||
        !modulus[0] || !exponent[0] || mbedtls_sha256((const uint8_t *)token, signed_length, hash, 0)) return false;
    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa);
    int result = mbedtls_rsa_import_raw(&rsa, modulus, n_size, NULL, 0, NULL, 0, NULL, 0, exponent, e_size);
    if (!result) result = mbedtls_rsa_complete(&rsa);
    if (!result) result = mbedtls_rsa_check_pubkey(&rsa);
    if (!result && mbedtls_rsa_get_bitlen(&rsa) < 2048) result = -1;
    if (!result) result = mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_SHA256);
    if (!result) result = mbedtls_rsa_pkcs1_verify(&rsa, MBEDTLS_MD_SHA256, sizeof hash, hash, sig);
    mbedtls_rsa_free(&rsa);
    return result == 0;
}

const char *oidc_verify(const char *token, const cJSON *jwks, const char *issuer, const char *client_id,
                        const char *nonce, int64_t now, oidc_identity_t *out)
{
    if (now < 1700000000) return "clock";
    char kid[129];
    if (!oidc_jwt_kid(token, kid)) return "token";
    const char *first = strchr(token, '.'), *second = strchr(first + 1, '.');
    if (!second || strchr(second + 1, '.') || !second[1]) return "token";
    const cJSON *key = oidc_jwk(jwks, kid);
    if (!key || !signature(key, token, second - token, second + 1)) return "token";
    cJSON *claims = segment(first + 1, second - first - 1, 12288);
    const char *error = claims ? oidc_claims(claims, issuer, client_id, nonce, now, out) : "token";
    cJSON_Delete(claims);
    return error;
}

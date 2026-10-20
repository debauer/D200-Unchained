#include "oidc.h"

#include <stdio.h>
#include <string.h>
#include "mbedtls/sha256.h"

static const cJSON *field(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static bool text_is(const cJSON *v, const char *text) { return cJSON_IsString(v) && !strcmp(v->valuestring, text); }
static bool timestamp(const cJSON *v)
{
    return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 9007199254740991.0 &&
           v->valuedouble == (double)(int64_t)v->valuedouble;
}

const char *oidc_claims(const cJSON *claims, const char *issuer, const char *client_id,
                        const char *nonce, int64_t now, oidc_identity_t *out)
{
    const cJSON *sub = field(claims, "sub"), *aud = field(claims, "aud"), *azp = field(claims, "azp");
    const cJSON *exp = field(claims, "exp"), *iat = field(claims, "iat"), *nbf = field(claims, "nbf");
    if (!text_is(field(claims, "iss"), issuer) || !text_is(field(claims, "nonce"), nonce) ||
        !cJSON_IsString(sub) || !sub->valuestring[0] || strlen(sub->valuestring) > 255 ||
        !timestamp(exp) || !timestamp(iat) || (nbf && !timestamp(nbf))) return "token";
    for (const char *p = sub->valuestring; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p >= 127) return "token";
    if (exp->valuedouble <= (double)now - 60 || iat->valuedouble > (double)now + 60 ||
        exp->valuedouble <= iat->valuedouble || (nbf && nbf->valuedouble > (double)now + 60)) return "token";
    bool audience = text_is(aud, client_id);
    if (cJSON_IsArray(aud)) {
        if (!cJSON_GetArraySize(aud) || cJSON_GetArraySize(aud) > 16) return "token";
        const cJSON *item;
        cJSON_ArrayForEach(item, aud) {
            if (!cJSON_IsString(item)) return "token";
            audience = audience || text_is(item, client_id);
        }
    }
    if (!audience || ((azp || (cJSON_IsArray(aud) && cJSON_GetArraySize(aud) > 1)) && !text_is(azp, client_id))) return "token";
    const cJSON *client = field(field(claims, "resource_access"), client_id), *role;
    const cJSON *roles = field(client, "roles");
    bool allowed = false;
    if (cJSON_IsArray(roles)) cJSON_ArrayForEach(role, roles) if (text_is(role, "deck-admin")) allowed = true;
    if (!allowed) return "forbidden";

    /* Identity is issuer + subject, independent of mutable or colliding display names. */
    char identity[OIDC_MAX_URL + 256];
    int length = snprintf(identity, sizeof identity, "%s\n%s", issuer, sub->valuestring);
    uint8_t hash[32];
    if (length < 0 || (size_t)length >= sizeof identity ||
        mbedtls_sha256((const uint8_t *)identity, length, hash, 0)) return "token";
    for (unsigned i = 0; i < sizeof hash; i++) snprintf(out->identity + 2 * i, 3, "%02x", hash[i]);
    const cJSON *name = field(claims, "preferred_username");
    const char *display = cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : sub->valuestring;
    size_t size = strlen(display);
    if (size > 64) { size = 64; while (size && ((unsigned char)display[size] & 0xC0) == 0x80) size--; }
    memcpy(out->username, display, size);
    out->username[size] = 0;
    return NULL;
}

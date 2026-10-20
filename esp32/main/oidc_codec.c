#include "oidc.h"

#include <stdlib.h>
#include <string.h>
#include "mbedtls/sha256.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool oidc_b64_encode(const uint8_t *data, size_t length, char *out, size_t capacity)
{
    size_t needed = length / 3 * 4 + (length % 3 ? length % 3 + 1 : 0);
    if (needed >= capacity) return false;
    unsigned accumulator = 0, bits = 0;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        accumulator = accumulator << 8 | data[i];
        bits += 8;
        while (bits >= 6) { bits -= 6; out[used++] = B64[(accumulator >> bits) & 63]; }
    }
    if (bits) out[used++] = B64[(accumulator << (6 - bits)) & 63];
    out[used] = 0;
    return true;
}

bool oidc_b64_decode(const char *text, size_t length, uint8_t *out, size_t capacity, size_t *written)
{
    if (length % 4 == 1) return false;
    unsigned accumulator = 0, bits = 0;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        const char *value = text[i] ? strchr(B64, text[i]) : NULL;
        if (!value) return false;
        accumulator = accumulator << 6 | (unsigned)(value - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (used == capacity) return false;
            out[used++] = (uint8_t)(accumulator >> bits);
        }
    }
    if (bits && (accumulator & ((1u << bits) - 1))) return false;
    *written = used;
    return true;
}

bool oidc_pkce(const char *verifier, char challenge[44])
{
    uint8_t hash[32];
    return mbedtls_sha256((const uint8_t *)verifier, strlen(verifier), hash, 0) == 0 &&
           oidc_b64_encode(hash, sizeof hash, challenge, 44);
}

char *oidc_url_encode(const char *text)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t length = strlen(text);
    if (length > OIDC_MAX_TOKEN) return NULL;
    char *out = malloc(length * 3 + 1);
    if (!out) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c)) out[used++] = c;
        else { out[used++] = '%'; out[used++] = HEX[c >> 4]; out[used++] = HEX[c & 15]; }
    }
    out[used] = 0;
    return out;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool decode(const char *text, size_t length, char *out, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = text[i];
        if (c == '%') {
            if (i + 2 >= length || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) return false;
            c = (unsigned char)(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c == 127 || used + 1 >= capacity) return false;
        out[used++] = c;
    }
    out[used] = 0;
    return true;
}

int oidc_query(const char *query, const char *name, char *out, size_t capacity)
{
    bool found = false;
    for (const char *p = query; *p;) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *equal = memchr(p, '=', end - p);
        if (!equal) equal = end;
        char key[64];
        if (!decode(p, equal - p, key, sizeof key)) return -1;
        if (!strcmp(key, name)) {
            if (found || equal == end || !decode(equal + 1, end - equal - 1, out, capacity)) return -1;
            found = true;
        }
        p = *end ? end + 1 : end;
    }
    return found;
}

bool oidc_same_origin(const char *url, const char *issuer)
{
    if (strlen(url) >= OIDC_MAX_URL || strchr(url, '#') || strchr(url, '@') || strchr(url, '\\')) return false;
    for (const char *p = url; *p; p++) if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127) return false;
    const char *scheme = strstr(issuer, "://");
    if (!scheme) return false;
    const char *path = strchr(scheme + 3, '/');
    size_t origin = path ? (size_t)(path - issuer) : strlen(issuer);
    return !strncmp(url, issuer, origin) && (url[origin] == '/' || !url[origin]);
}

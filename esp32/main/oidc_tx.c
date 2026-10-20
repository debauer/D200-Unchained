#include "oidc.h"

#include <string.h>
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"

static void hex(const uint8_t *bytes, char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) { out[2 * i] = digits[bytes[i] >> 4]; out[2 * i + 1] = digits[bytes[i] & 15]; }
    out[64] = 0;
}

bool oidc_tx_create(oidc_transactions_t *store, const uint8_t entropy[128], uint64_t now,
                    char cookie[65], oidc_transaction_t *out)
{
    for (unsigned i = 0; i < OIDC_TX_COUNT; i++) {
        oidc_transaction_t *tx = &store->items[i];
        if (tx->expires > now) continue;
        hex(entropy, cookie);
        if (mbedtls_sha256((const uint8_t *)cookie, 64, out->cookie_hash, 0)) return false;
        hex(entropy + 32, out->state);
        hex(entropy + 64, out->nonce);
        hex(entropy + 96, out->verifier);
        out->expires = now + (uint64_t)OIDC_TX_SECONDS * 1000000;
        *tx = *out;
        return true;
    }
    return false;
}

bool oidc_tx_take(oidc_transactions_t *store, const char *cookie, const char *state, uint64_t now, oidc_transaction_t *out)
{
    if (strlen(cookie) != 64) return false;
    uint8_t hash[32];
    if (mbedtls_sha256((const uint8_t *)cookie, 64, hash, 0)) return false;
    for (unsigned i = 0; i < OIDC_TX_COUNT; i++) {
        oidc_transaction_t *tx = &store->items[i];
        unsigned difference = 0;
        for (unsigned j = 0; j < 32; j++) difference |= hash[j] ^ tx->cookie_hash[j];
        if (difference || !tx->expires) continue;
        bool valid = tx->expires > now && strlen(state) == 64;
        for (unsigned j = 0; valid && j < 64; j++) difference |= (unsigned char)state[j] ^ (unsigned char)tx->state[j];
        if (valid && !difference) *out = *tx;
        mbedtls_platform_zeroize(tx, sizeof *tx); /* Consume even an invalid-state callback from the right browser. */
        return valid && !difference;
    }
    return false;
}

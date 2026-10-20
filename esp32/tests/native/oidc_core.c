#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oidc.h"

static void codec(void)
{
    char challenge[44], encoded[64], query[64];
    uint8_t bytes[32];
    size_t written;
    assert(oidc_pkce("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", challenge));
    assert(!strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
    assert(oidc_b64_encode((const uint8_t *)"foobar", 6, encoded, sizeof encoded) && !strcmp(encoded, "Zm9vYmFy"));
    assert(oidc_b64_decode(encoded, strlen(encoded), bytes, sizeof bytes, &written) && written == 6 && !memcmp(bytes, "foobar", 6));
    assert(!oidc_b64_decode("Zh", 2, bytes, sizeof bytes, &written)); /* noncanonical trailing bits */
    assert(!oidc_b64_decode("Zg==", 4, bytes, sizeof bytes, &written));
    assert(!oidc_b64_decode("A", 1, bytes, sizeof bytes, &written));
    assert(oidc_query("code=x%2By&state=a+b", "code", query, sizeof query) == 1 && !strcmp(query, "x+y"));
    assert(oidc_query("state=one&%73tate=two", "state", query, sizeof query) == -1);
    assert(oidc_query("state=%00", "state", query, sizeof query) == -1);
    assert(oidc_query("code=x", "state", query, sizeof query) == 0);
    assert(oidc_same_origin("https://id.example:8443/keys", "https://id.example:8443/realms/test"));
    assert(!oidc_same_origin("https://id.example.evil/keys", "https://id.example/realms/test"));
    assert(!oidc_same_origin("http://id.example/keys", "https://id.example/realms/test"));
    const char *bad[] = {"{\"a\":1,\"a\":2}", "{} trailing", "{\"a\":\"x\\u0000y\"}", "{\"a\":\"\xff\"}"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) assert(!oidc_json(bad[i], strlen(bad[i])));
    char many[4096];
    size_t used = 0;
    many[used++] = '{';
    for (unsigned i = 0; i < 129; i++) used += snprintf(many + used, sizeof many - used, "%s\"field%u\":0", i ? "," : "", i);
    many[used++] = '}';
    assert(!oidc_json(many, used)); /* Bound duplicate-key scanning, not only input bytes/depth. */
}

static void transactions(void)
{
    oidc_transactions_t store = {0};
    uint8_t entropy[128];
    for (unsigned i = 0; i < sizeof entropy; i++) entropy[i] = i;
    oidc_transaction_t tx, out;
    char cookie[65];
    assert(oidc_tx_create(&store, entropy, 1000, cookie, &tx));
    assert(!oidc_tx_take(&store, "foreign", tx.state, 1000, &out));
    assert(!oidc_tx_take(&store, cookie, "wrong state", 1000, &out));
    assert(!oidc_tx_take(&store, cookie, tx.state, 1000, &out));
    assert(oidc_tx_create(&store, entropy, 1000, cookie, &tx));
    assert(oidc_tx_take(&store, cookie, tx.state, 1000, &out) && !strcmp(tx.verifier, out.verifier));
    assert(!oidc_tx_take(&store, cookie, tx.state, 1000, &out));
    for (unsigned i = 0; i < OIDC_TX_COUNT; i++) { entropy[0]++; assert(oidc_tx_create(&store, entropy, 1000, cookie, &tx)); }
    assert(!oidc_tx_create(&store, entropy, 1000, cookie, &tx));
    assert(!oidc_tx_take(&store, cookie, tx.state, 1000 + (uint64_t)OIDC_TX_SECONDS * 1000000, &out));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    codec();
    transactions();
    FILE *f = fopen(argv[1], "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length > 0 && !fseek(f, 0, SEEK_SET));
    char *text = malloc(length + 1);
    assert(text && fread(text, 1, length, f) == (size_t)length);
    fclose(f);
    text[length] = 0;
    cJSON *vectors = cJSON_Parse(text);
    free(text);
    assert(vectors);
    cJSON *jwks = cJSON_GetObjectItem(vectors, "jwks"), *item;
    char original[65] = "";
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(vectors, "cases")) {
        const char *name = cJSON_GetObjectItem(item, "name")->valuestring;
        const char *token = cJSON_GetObjectItem(item, "token")->valuestring;
        const char *want = cJSON_GetObjectItem(item, "expected")->valuestring;
        oidc_identity_t identity;
        const char *error = oidc_verify(token, jwks, "http://id.example/realms/test", "deck", "nonce", 1700001000, &identity);
        if (strcmp(error ? error : "ok", want)) { fprintf(stderr, "OIDC vector failed: %s (%s)\n", name, error ? error : "ok"); abort(); }
        if (!strcmp(name, "valid")) strcpy(original, identity.identity);
        if (!strcmp(name, "renamed display")) assert(!strcmp(original, identity.identity));
        if (!strcmp(name, "other subject same name")) assert(strcmp(original, identity.identity));
        assert(!strcmp(oidc_verify(token, jwks, "http://id.example/realms/test", "deck", "nonce", 0, &identity), "clock"));
    }
    cJSON_Delete(vectors);
    puts("OIDC codec, transactions, RS256 and claims ok");
}

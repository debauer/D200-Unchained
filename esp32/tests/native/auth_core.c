#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "auth_core.h"

int main(void)
{
    auth_store_t store = {0};
    uint8_t hash[32] = {1}, other[32] = {2};
    uint64_t now = 1000, duration = (uint64_t)172800 * 1000000;
    uint64_t ticket = auth_store_create(&store, hash, "admin", now, 172800);
    assert(ticket && auth_store_find(&store, hash, now) == ticket);
    assert(!auth_store_find(&store, other, now));
    assert(auth_store_get(&store, ticket, now + duration - 1));
    assert(!auth_store_get(&store, ticket, now + duration));
    uint64_t replacement = auth_store_create(&store, hash, "admin", now + duration, 10);
    assert(replacement && replacement != ticket && !auth_store_get(&store, ticket, now + duration));
    auth_store_revoke(&store, ticket); /* An old ticket must not revoke a reused slot. */
    assert(auth_store_get(&store, replacement, now + duration));
    auth_store_revoke(&store, replacement);
    assert(!auth_store_find(&store, hash, now + duration));
    for (unsigned i = 0; i < AUTH_MAX_SESSIONS; i++) {
        hash[0] = i;
        assert(auth_store_create(&store, hash, "admin", now, 1));
    }
    assert(!auth_store_create(&store, other, "admin", now, 1));
    assert(auth_store_create(&store, other, "admin", now + 1000000, 1));

    const char *value = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char cookie[256], token[65];
    snprintf(cookie, sizeof cookie, "other=x; deck_session=%s; more=y", value);
    assert(auth_cookie_parse(cookie, "deck_session", token) && !strcmp(token, value));
    assert(!auth_cookie_parse(cookie, "deck_sess", token));
    snprintf(cookie, sizeof cookie, "deck_session=%s; deck_session=%s", value, value);
    assert(!auth_cookie_parse(cookie, "deck_session", token));
    assert(!auth_cookie_parse("deck_session=short", "deck_session", token));
    snprintf(cookie, sizeof cookie, "deck_session=%sX", value);
    assert(!auth_cookie_parse(cookie, "deck_session", token));

    auth_limiter_t rate = {0};
    for (unsigned ip = 1; ip <= 4; ip++) {
        for (int i = 0; i < 5; i++) assert(!auth_rate_attempt(&rate, ip, 0));
        assert(auth_rate_attempt(&rate, ip, 0) == 60);
    }
    assert(auth_rate_attempt(&rate, 99, 0) == 60);
    assert(!auth_rate_attempt(&rate, 99, 60000000));
    puts("auth core ok");
}

#include "auth_core.h"

#include <string.h>

static uint64_t ticket_for(unsigned index, uint32_t generation) { return (uint64_t)generation << 8 | (index + 1); }

const auth_session_t *auth_store_get(const auth_store_t *store, uint64_t ticket, uint64_t now)
{
    unsigned slot = ticket & 255;
    if (!slot || slot > AUTH_MAX_SESSIONS) return NULL;
    const auth_session_t *s = &store->sessions[slot - 1];
    return s->generation && s->generation == ticket >> 8 && now < s->expires ? s : NULL;
}

uint64_t auth_store_create_as(auth_store_t *store, const uint8_t hash[32], const char *user, const char *identity, uint64_t now, uint32_t ttl)
{
    if (!ttl || strlen(user) > AUTH_USER_LENGTH || strlen(identity) > 64) return 0;
    for (unsigned i = 0; i < AUTH_MAX_SESSIONS; i++) {
        auth_session_t *s = &store->sessions[i];
        if (s->generation && now < s->expires) continue;
        if (!++store->generation) ++store->generation;
        *s = (auth_session_t){.generation = store->generation, .expires = now + (uint64_t)ttl * 1000000};
        memcpy(s->hash, hash, sizeof s->hash);
        strcpy(s->user, user);
        strcpy(s->identity, identity);
        return ticket_for(i, s->generation);
    }
    return 0;
}

uint64_t auth_store_create(auth_store_t *store, const uint8_t hash[32], const char *user, uint64_t now, uint32_t ttl)
{
    return auth_store_create_as(store, hash, user, user, now, ttl);
}

uint64_t auth_store_find(const auth_store_t *store, const uint8_t hash[32], uint64_t now)
{
    uint64_t result = 0;
    for (unsigned i = 0; i < AUTH_MAX_SESSIONS; i++) {
        const auth_session_t *s = &store->sessions[i];
        unsigned difference = 0;
        for (unsigned j = 0; j < 32; j++) difference |= s->hash[j] ^ hash[j];
        if (!difference && s->generation && now < s->expires) result = ticket_for(i, s->generation);
    }
    return result;
}

void auth_store_revoke(auth_store_t *store, uint64_t ticket)
{
    unsigned slot = ticket & 255;
    if (slot && slot <= AUTH_MAX_SESSIONS && store->sessions[slot - 1].generation == ticket >> 8)
        memset(&store->sessions[slot - 1], 0, sizeof store->sessions[0]);
}

bool auth_cookie_parse(const char *cookie, const char *name, char token[AUTH_TOKEN_LENGTH + 1])
{
    bool found = false;
    size_t name_len = strlen(name);
    for (const char *p = cookie; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        const char *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        const char *equal = memchr(p, '=', end - p);
        if (equal && (size_t)(equal - p) == name_len && !memcmp(p, name, name_len)) {
            const char *value = equal + 1;
            if (found || end - value != AUTH_TOKEN_LENGTH) return false;
            for (const char *c = value; c < end; c++) if (!(*c >= '0' && *c <= '9') && !(*c >= 'a' && *c <= 'f')) return false;
            memcpy(token, value, AUTH_TOKEN_LENGTH);
            token[AUTH_TOKEN_LENGTH] = 0;
            found = true;
        }
        p = end;
    }
    return found;
}

unsigned auth_rate_attempt(auth_limiter_t *limiter, uint32_t address, uint64_t now)
{
    const uint64_t window = 60000000;
    if (now - limiter->start >= window) {
        limiter->start = now;
        limiter->count = 0;
    }
    if (limiter->count >= 20) return (unsigned)((window - (now - limiter->start) + 999999) / 1000000);
    auth_rate_entry_t *entry = NULL, *oldest = &limiter->sources[0];
    for (unsigned i = 0; i < 8; i++) {
        auth_rate_entry_t *s = &limiter->sources[i];
        if (s->used && s->address == address) entry = s;
        if (!s->used || s->start < oldest->start) oldest = s;
    }
    if (!entry) {
        entry = oldest;
        *entry = (auth_rate_entry_t){.used = true, .address = address, .start = now};
    }
    if (now - entry->start >= window) { entry->start = now; entry->count = 0; }
    if (entry->count >= 5) return (unsigned)((window - (now - entry->start) + 999999) / 1000000);
    entry->count++;
    limiter->count++;
    return 0;
}

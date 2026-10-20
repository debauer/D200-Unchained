#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_MAX_SESSIONS 8
#define AUTH_TOKEN_LENGTH 64
#define AUTH_USER_LENGTH 64
#define AUTH_OPEN_TICKET UINT64_MAX

typedef struct {
    uint8_t hash[32];
    uint64_t expires;
    uint32_t generation;
    char user[AUTH_USER_LENGTH + 1];
} auth_session_t;

typedef struct {
    auth_session_t sessions[AUTH_MAX_SESSIONS];
    uint32_t generation;
} auth_store_t;

/* All times are monotonic microseconds; the caller serializes access. Zero means no session. */
uint64_t auth_store_create(auth_store_t *store, const uint8_t hash[32], const char *user, uint64_t now, uint32_t ttl);
uint64_t auth_store_find(const auth_store_t *store, const uint8_t hash[32], uint64_t now);
const auth_session_t *auth_store_get(const auth_store_t *store, uint64_t ticket, uint64_t now);
void auth_store_revoke(auth_store_t *store, uint64_t ticket);
bool auth_cookie_parse(const char *cookie, const char *name, char token[AUTH_TOKEN_LENGTH + 1]);

typedef struct { uint32_t address, count; uint64_t start; bool used; } auth_rate_entry_t;
typedef struct {
    uint64_t start;
    uint32_t count;
    auth_rate_entry_t sources[8];
} auth_limiter_t;

/* Zero permits an attempt, otherwise seconds until retry. 5/source and 20 globally per minute. */
unsigned auth_rate_attempt(auth_limiter_t *limiter, uint32_t address, uint64_t now);

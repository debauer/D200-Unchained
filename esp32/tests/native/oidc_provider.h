#pragma once
#include "oidc.h"
#include <time.h>

extern char test_nonce[65], test_challenge[44], test_kid[16];
extern bool test_down, test_forbidden, test_bad_nonce, test_bad_signature, test_bad_metadata;
extern bool test_late_keys;
extern unsigned test_discoveries, test_key_requests, test_exchanges;
extern int64_t test_unix;
void test_provider_init(const char *directory);
void test_provider_free(void);
time_t oidc_test_time(time_t *out);

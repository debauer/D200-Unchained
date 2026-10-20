#pragma once
#ifndef TEST_WEB_TLS
#define TEST_WEB_TLS 0
#endif
#define WEB_TLS TEST_WEB_TLS
#define WEB_PORT (WEB_TLS ? 443 : 80)
#ifndef TEST_OIDC_ENABLED
#define TEST_OIDC_ENABLED 0
#endif
#if TEST_OIDC_ENABLED
#if WEB_TLS
#define WEB_ORIGIN "https://deck.example"
#else
#define WEB_ORIGIN "http://deck.example"
#endif
#else
#define WEB_ORIGIN ""
#endif
#define WEB_HOST ""
#define WEB_AUTH_ENABLED 1
#define WEB_SESSION_TTL 172800U
#define WEB_AUTH_USER "admin"
#define WEB_AUTH_ROUNDS 120000
#define WEB_AUTH_SALT {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}
/* Synthetic fixture for test-only-password, independently calculated with Python hashlib. */
#define WEB_AUTH_DIGEST {0x38,0xa9,0x3a,0x01,0x54,0x9a,0xe8,0xe9,0x5f,0x6b,0x21,0x60,0x9e,0x86,0x4b,0x5a,0x3e,0xdb,0x3e,0x62,0xf8,0x50,0x2a,0x29,0x11,0x19,0xa0,0x9b,0xa9,0x62,0x7f,0xba}
#define WEB_COOKIE_NAME (WEB_TLS ? "__Host-deck_session" : "deck_session")
#define WEB_OIDC_ENABLED TEST_OIDC_ENABLED
#define WEB_OIDC_ISSUER "http://id.example/realms/test"
#define WEB_OIDC_CLIENT_ID "deck"
#define WEB_OIDC_SCOPES "openid profile"
#define WEB_OIDC_COOKIE (WEB_TLS ? "__Host-deck_oidc" : "deck_oidc")

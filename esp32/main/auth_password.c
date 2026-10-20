#include "auth_password.h"

#include <string.h>
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "web_settings.h"

bool auth_password_verify(const char *username, const char *password)
{
    static const unsigned char salt[16] = WEB_AUTH_SALT, expected[32] = WEB_AUTH_DIGEST;
    unsigned char hash[32];
    if (!WEB_AUTH_ENABLED || strlen(password) > 128 || strlen(username) > 64) return false;
    int err = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)password, strlen(password),
                                           salt, sizeof salt, WEB_AUTH_ROUNDS, sizeof hash, hash);
    unsigned difference = err != 0;
    if (err) memset(hash, 0, sizeof hash);
    for (unsigned i = 0; i < sizeof hash; i++) difference |= hash[i] ^ expected[i];
    size_t length = strlen(WEB_AUTH_USER), supplied = strlen(username);
    difference |= length ^ supplied;
    for (size_t i = 0; i < length; i++) difference |= (unsigned char)WEB_AUTH_USER[i] ^ (i < supplied ? (unsigned char)username[i] : 0);
    mbedtls_platform_zeroize(hash, sizeof hash);
    return difference == 0;
}

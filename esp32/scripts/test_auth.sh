#!/usr/bin/env bash
# Real mbedTLS on the PC, HTTP/RTOS adapters only. Requires the ESP32 build tools installed by PlatformIO.
set -euo pipefail
cd "$(dirname "$0")/.."
core=${PLATFORMIO_CORE_DIR:-$HOME/.platformio}
mbed="$core/packages/framework-espidf/components/mbedtls/mbedtls"
cjson=${CJSON_DIR:-$core/packages/framework-espidf/components/json/cJSON}
cmake="$core/packages/tool-cmake/bin/cmake"
build="$PWD/.pio/mbedtls-native"
"$cmake" -S "$mbed" -B "$build" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
"$cmake" --build "$build" --target mbedcrypto -j 4
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
for tls in 0 1; do
    gcc -Wall -Werror -pthread -I main -I tests/native/stubs -I "$mbed/include" -I "$cjson" \
        -DTEST_AUTH_CLOCK -DTEST_WEB_TLS="$tls" -DWEB_CONFIG_HEADER='"web_test_config.h"' \
        -o "$out/auth-$tls" tests/native/web_auth.c tests/native/auth_platform.c main/auth_core.c \
        main/auth_password.c main/web_auth.c main/web_login.c main/web_guard.c "$cjson/cJSON.c" \
        "$build/library/libmbedcrypto.a" "$build/3rdparty/everest/libeverest.a" "$build/3rdparty/p256-m/libp256m.a"
    "$out/auth-$tls"
done

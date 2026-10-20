#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
core=${PLATFORMIO_CORE_DIR:-$HOME/.platformio}
mbed="$core/packages/framework-espidf/components/mbedtls/mbedtls"
cjson=${CJSON_DIR:-$core/packages/framework-espidf/components/json/cJSON}
build="$PWD/.pio/mbedtls-native"
python=${PYTHON:-python3}
"$python" tests/oidc_fixtures.py
"$core/packages/tool-cmake/bin/cmake" -S "$mbed" -B "$build" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
"$core/packages/tool-cmake/bin/cmake" --build "$build" --target mbedcrypto -j 4
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -Wall -Werror -I main -I "$mbed/include" -I "$cjson" -o "$out/core" tests/native/oidc_core.c \
    main/oidc_codec.c main/oidc_json.c main/oidc_tx.c main/oidc_jwt.c main/oidc_claims.c "$cjson/cJSON.c" \
    "$build/library/libmbedcrypto.a" "$build/3rdparty/everest/libeverest.a" "$build/3rdparty/p256-m/libp256m.a"
"$out/core" .pio/oidc-fixtures/vectors.json
for tls in 0 1; do
    flags=(-Wall -Werror -pthread -I main -I tests/native/stubs -I "$mbed/include" -I "$cjson"
        -DTEST_AUTH_CLOCK -DTEST_WEB_TLS="$tls" -DTEST_OIDC_ENABLED=1 -DWEB_CONFIG_HEADER='"web_test_config.h"')
    gcc "${flags[@]}" -Dtime=oidc_test_time -c main/web_oidc.c -o "$out/web_oidc-$tls.o"
    gcc "${flags[@]}" -o "$out/flow-$tls" tests/native/oidc_flow.c tests/native/oidc_provider.c tests/native/auth_platform.c \
        main/auth_core.c main/auth_password.c main/web_auth.c main/web_login.c main/web_guard.c \
        main/oidc_client.c main/oidc_codec.c main/oidc_json.c main/oidc_tx.c main/oidc_jwt.c main/oidc_claims.c \
        "$out/web_oidc-$tls.o" "$cjson/cJSON.c" "$build/library/libmbedcrypto.a" \
        "$build/3rdparty/everest/libeverest.a" "$build/3rdparty/p256-m/libp256m.a"
    "$out/flow-$tls" .pio/oidc-fixtures
done
for custom_ca in 0 1; do
    gcc -Wall -Werror -I main -I tests/native/stubs -I "$mbed/include" -I "$cjson" \
        -DTEST_AUTH_CLOCK -DTEST_CUSTOM_CA="$custom_ca" -DWEB_CONFIG_HEADER='"web_test_config.h"' \
        -o "$out/http-$custom_ca" tests/native/oidc_http.c main/oidc_http.c main/oidc_json.c "$cjson/cJSON.c" \
        "$build/library/libmbedcrypto.a" "$build/3rdparty/everest/libeverest.a" "$build/3rdparty/p256-m/libp256m.a"
    "$out/http-$custom_ca"
done

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

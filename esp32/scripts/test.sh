#!/usr/bin/env bash
# Tests of the portable firmware code (renderer, placeholders, colour rules, MQTT values and settings) on the PC: needs gcc, and one `pio run` first
# (it converts the fonts to main/fonts/ and fetches the ESP-IDF sources whose cJSON is used here).
set -euo pipefail
cd "$(dirname "$0")/.."
cjson=${CJSON_DIR:-${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-espidf/components/json/cJSON}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags=(-Wall -Werror -I main -I "$cjson")
gcc "${flags[@]}" -I tests/native/stubs -DCONFIG_DIR='"."' -DIMAGE_DIR='"images"' \
    -Wl,--wrap=fwrite -Wl,--wrap=fclose -o "$out/config" tests/native/config.c \
    main/config.c main/config_validate.c main/config_fields.c "$cjson/cJSON.c"
(cd "$out" && ./config)
gcc "${flags[@]}" -I tests/native/stubs -o "$out/protocol" tests/native/protocol.c main/adb_sync.c main/deckd.c main/rle.c
gcc "${flags[@]}" -I tests/native/stubs -DCONFIG_D200_DEBUG -o "$out/adb_transport" tests/native/adb_transport.c main/adb.c
"$out/protocol"
"$out/adb_transport"
gcc "${flags[@]}" -I tests/native/stubs -DWEB_CONFIG_HEADER='"web_test_config.h"' -o "$out/web_guard" tests/native/web_guard.c main/web_guard.c
gcc "${flags[@]}" -o "$out/auth_core" tests/native/auth_core.c main/auth_core.c
"$out/auth_core"
gcc "${flags[@]}" -I tests/native/stubs -o "$out/device_json" tests/native/device_json.c main/device_json.c "$cjson/cJSON.c"
"$out/web_guard"
"$out/device_json"
gcc "${flags[@]}" -I tests/native/stubs -pthread -o "$out/device_state" tests/native/device_state.c main/device.c "$cjson/cJSON.c"
"$out/device_state"
gcc "${flags[@]}" -o "$out/placeholders" tests/native/placeholders.c main/placeholders.c main/rules.c main/mqttjson.c "$cjson/cJSON.c"
gcc "${flags[@]}" -o "$out/mqtt" tests/native/mqtt.c main/mqttjson.c main/mqttsettings.c "$cjson/cJSON.c"
gcc "${flags[@]}" -O2 -o "$out/render" tests/native/render.c main/render.c main/glyphs.c -lm
"$out/placeholders"
gcc "${flags[@]}" -O2 -o "$out/rle" tests/native/rle.c main/rle.c
"$out/rle"
"$out/mqtt"
"$out/render" main/fonts/chakra-petch-700.ttf main/fonts/chakra-petch-500.ttf

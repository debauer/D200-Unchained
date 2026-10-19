#!/usr/bin/env bash
# Tests of the portable firmware code (renderer, placeholders) on the PC: needs gcc, and one `pio run` first
# (it converts the fonts to main/fonts/ and fetches the ESP-IDF sources whose cJSON is used here).
set -euo pipefail
cd "$(dirname "$0")/.."
cjson=$(ls -d ~/.platformio/packages/framework-espidf/components/json/cJSON)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags=(-Wall -Werror -I main -I "$cjson")
gcc "${flags[@]}" -o "$out/placeholders" tests/native/placeholders.c main/placeholders.c "$cjson/cJSON.c"
gcc "${flags[@]}" -O2 -o "$out/render" tests/native/render.c main/render.c main/glyphs.c -lm
"$out/placeholders"
"$out/render" main/fonts/chakra-petch-700.ttf main/fonts/chakra-petch-500.ttf

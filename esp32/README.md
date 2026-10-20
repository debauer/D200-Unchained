# ESP32-S3 host (work in progress)

An ESP32-S3 replaces the Linux host: it is the USB host of the D200, talks to it over ADB and
serves the shared web interface (`../host/src/d200_deck/web/static/`, flashed unchanged as the
read-only `www` partition). The common HTTP contract is in [web-api.md](../docs/web-api.md);
supported action and variable types are advertised by `/api/info`.

Current state: WiFi with automatic reconnect, web server with the shared UI, USB enumeration of the D200,
the bootstrap (stops the vendor app, compares SHA-256 hashes, pushes the embedded `deckd` if needed and starts it) and a
`deckd` client. The web API serves `/api/info` with the real key windows, `/api/events` (key presses, status), and
stores the configuration (`/api/config`). Key images and icons are stored and drawn (the web UI composes them in the browser). Key actions run on the device (backlight
-/+, display on/off, page, back; `POST .../press` in the TEST tab, `POST /api/page`). Keys with `{time}`/`{date}`
placeholders and the info window's clock are drawn by the firmware itself (stb_truetype with Chakra Petch, converted
from the web interface's woff2 at build time) and refreshed every minute. MQTT follows [docs/mqtt.md](../docs/mqtt.md)
(esp-mqtt): broker settings in the web UI, `mqtt` variables with JSON path, the `mqtt` publish action and colour
rules with the second image. The broker settings are stored in plain text in the `storage` partition (`mqtt.json`;
flash is not encrypted); the web API never returns the password. Not done: variables of type `command` (no source on
this host, they show `?`), `http`. Unknown action and variable types survive configuration round trips but are
not executed/read. Version-1 configurations are converted to version 2 on load/import.

### ESP32 limits and recovery

- Configurations: at most 64 KiB per API upload, 14 keys per page, page IDs up to 32 characters.
- At most 16 MQTT variables, topics up to 128 bytes and JSON paths up to 64 bytes. Configurations exceeding these
  limits are rejected rather than silently truncated. MQTT values are cut to 64 UTF-8 bytes.
- The MQTT receive buffer is 2048 bytes, including protocol/topic overhead. Messages delivered in fragments are
  logged and ignored; keep payloads comfortably below this size.
- Static key images are composed in the browser; keys with placeholders, matching rules and the clock are drawn
  by the firmware. Key images are stored separately and are not included in configuration JSON exports.
- WLAN retries continue with backoff (up to 60 seconds), including when the access point is unavailable at boot.
  USB operation does not depend on WLAN.
- USB transfers time out after 3 seconds and are cancelled before their buffers are reused. If cancellation itself
  does not complete within another 3 seconds, the ESP32 restarts to avoid reusing an in-flight DMA buffer.
- To restore the stock D200 UI, disconnect the ESP32's USB-OTG connection and reboot the D200. While the ESP32 stays
  attached, the next bootstrap stops the stock app again.

### Security

Local password authentication and HTTP/HTTPS are configured at build time in the ignored `auth.local.json`.
Session duration defaults to 48 hours and is configurable. See [web-auth.md](../docs/web-auth.md) for setup,
verifier generation, endpoints and tests; native Keycloak OIDC is planned in [web-auth-plan.md](../docs/web-auth-plan.md).
Without an auth configuration the existing HTTP mode remains unauthenticated and logs `authentication DISABLED`.
With `public_origin` configured, use that exact address; otherwise use the numeric IPv4 address. Host checks
prevent DNS rebinding, and changing requests also require `X-Deck-Request: 1` and a matching Origin when supplied.
HTTP is supported explicitly and sends credentials/cookies unencrypted; HTTPS requires your certificate/key.
WLAN credentials are embedded in the firmware and stored by ESP-IDF in NVS; the MQTT password is stored in
plaintext in `storage`. Flash is not encrypted. The API/export never returns the MQTT password or auth verifier.

Key images go to `deckd` run-length coded (protocol 5, `format 2`; flat areas and text shrink by a factor of 10 to 30,
photos stay as they are), and the USB transfers are reused instead of allocated per block. The menu item
`D200_ADB_ONE_TRANSFER` additionally sends header and data of an ADB message as one transfer (off by default: try it,
and turn it off again if the D200 stops answering).

## Hardware

ESP32-S3 devkit with two USB-C ports: **COM** (USB-UART, to the PC for flashing and the log) and
**USB-OTG** (to the D200). The solder jumpers `USB-OTG` and `IN-OUT` decide how the OTG port is
powered; check them against the schematic of your board revision. The flash layout assumes 16 MB (N16R8): 3 MB app, 1 MB web UI, about 12 MB for images and configuration (`partitions.csv`).

## Requirements

- **16 MB flash and 8 MB octal PSRAM** (N16R8): the renderer needs a few hundred KB for the wide
  info window. `sdkconfig.defaults` turns it on; a board without it will not start.
  N8R8 has PSRAM but only 8 MB flash and does not fit the current partition table.
- The time zone for the clock is a menu item (`D200 host`), default Central Europe.
- `sdkconfig.defaults` only applies to a fresh `sdkconfig.esp32s3`: after it changes, delete that file or change the
  setting in `menuconfig`.
- The WiFi login is not in the configuration: copy `wifi.local.example` to `wifi.local` (ignored by git) and fill in
  `ssid` and `password`; the next build embeds it. Without the file the WiFi stays off.

## Build and flash

The firmware embeds `deckd`. Build it first (`../install.sh`, or `cargo build --release --target
armv7-unknown-linux-musleabihf` in `../device`); `scripts/embed.py` looks for it there and in
`~/.local/share/deck/deckd`, `DECKD_BINARY=<file>` overrides.

[PlatformIO](https://platformio.org/) with the ESP-IDF framework, installed by uv (Python 3.10-3.13):

```sh
cd esp32
uv sync
cp wifi.local.example wifi.local  # then enter the WiFi login
uv run pio run -t upload          # firmware
uv run pio run -t uploadfs        # web UI (../host/src/d200_deck/web/static) into the `www` partition
uv run pio device monitor         # log, on the COM port
```

`scripts/test.sh` runs native tests on the PC: configuration validation/migration and storage, ADB sync and
transfer cancellation (mocked USB), deckd frame handling, Host checks and fragmented HTTP bodies, device state,
JSON sizing, renderer, placeholders, colour rules, MQTT and RLE. It needs `gcc`, pthreads and one `uv run pio run`
first to convert the fonts and fetch ESP-IDF's cJSON sources. `PLATFORMIO_CORE_DIR` and `CJSON_DIR` can override
the source lookup. These tests and a successful firmware build do not replace an on-device reconnect/flash test.

`scripts/run.sh` does upload and monitor in one go and also writes the output to files
(`logs/latest.log` for the monitor, `logs/upload.log` for build and upload; `--seconds 60`, `--no-upload`,
`--fs`, `--port` see the header of the script).

Flash the web UI again whenever the files in `host/.../web/static/` change. The log shows the IP
address; open `http://<ip>/`. Attaching the D200 logs its USB ids and the ADB interface, then the bootstrap
(`installing deckd`, `starting deckd`, `deckd is listening on the device`).

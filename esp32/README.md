# ESP32-S3 host (work in progress)

An ESP32-S3 replaces the Linux host: it is the USB host of the D200, talks to it over ADB and
serves the same web interface (`../host/src/d200_deck/web/static/`, flashed unchanged as the
read-only `www` partition) and the same web API (`../docs/web-api.md`).

Current state: WiFi, web server with the shared UI, `/api/info` stub, USB enumeration of the D200
the bootstrap (stops the vendor app, pushes the embedded `deckd` to the D200 if needed and starts it) and a
`deckd` client. The web API serves `/api/info` with the real key windows, `/api/events` (key presses, status), and
stores the configuration (`/api/config`). Key images and icons are stored and drawn (the web UI composes them in the browser). Key actions run on the device (backlight
-/+, display on/off, page, back; `POST .../press` in the TEST tab, `POST /api/page`). Keys with `{time}`/`{date}`
placeholders and the info window's clock are drawn by the firmware itself (stb_truetype with Chakra Petch, converted
from the web interface's woff2 at build time) and refreshed every minute. MQTT works as in [docs/mqtt.md](../docs/mqtt.md)
(esp-mqtt): broker settings in the web UI, `mqtt` variables with JSON path, the `mqtt` publish action and colour
rules with the second image. The broker settings are stored in plain text in the `storage` partition (`mqtt.json`;
flash is not encrypted); the web API never returns the password. Not done: variables of type `command` (no source on
this host, they show `?`), `http`.

## Hardware

ESP32-S3 devkit with two USB-C ports: **COM** (USB-UART, to the PC for flashing and the log) and
**USB-OTG** (to the D200). The solder jumpers `USB-OTG` and `IN-OUT` decide how the OTG port is
powered; check them against the schematic of your board revision. The flash layout assumes 8 MB or more.

## Requirements

- **PSRAM** (ESP32-S3 with octal PSRAM, e.g. N8R8/N16R8): the renderer needs a few hundred KB for the wide
  info window. `sdkconfig.defaults` turns it on; a board without it will not start.
- The time zone for the clock is a menu item (`D200 host`), default Central Europe.
- `sdkconfig.defaults` only applies to a fresh `sdkconfig.esp32s3`: after it changes, delete that file (and enter
  the WiFi data again in `menuconfig`) or change the setting there.

## Build and flash

The firmware embeds `deckd`. Build it first (`../install.sh`, or `cargo build --release --target
armv7-unknown-linux-musleabihf` in `../device`); `scripts/deckd.py` looks for it there and in
`~/.local/share/deck/deckd`, `DECKD_BINARY=<file>` overrides.

[PlatformIO](https://platformio.org/) with the ESP-IDF framework, installed by uv (Python 3.10-3.13):

```sh
cd esp32
uv sync
uv run pio run -t menuconfig      # "D200 host": WiFi SSID and password
uv run pio run -t upload          # firmware
uv run pio run -t uploadfs        # web UI (../host/src/d200_deck/web/static) into the `www` partition
uv run pio device monitor         # log, on the COM port
```

`scripts/test.sh` runs the tests of the portable code (renderer, placeholders) on the PC.

`scripts/run.sh` does upload and monitor in one go and also writes the output to files
(`logs/latest.log` for the monitor, `logs/upload.log` for build and upload; `--seconds 60`, `--no-upload`,
`--fs`, `--port` see the header of the script).

Flash the web UI again whenever the files in `host/.../web/static/` change. The log shows the IP
address; open `http://<ip>/`. Attaching the D200 logs its USB ids and the ADB interface, then the bootstrap
(`installing deckd`, `starting deckd`, `deckd is listening on the device`).

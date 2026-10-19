# ESP32-S3 host (work in progress)

An ESP32-S3 replaces the Linux host: it is the USB host of the D200, talks to it over ADB and
serves the same web interface (`../host/src/d200_deck/web/static/`, flashed unchanged as the
read-only `www` partition) and the same web API (`../docs/web-api.md`).

Current state: WiFi, web server with the shared UI, `/api/info` stub, USB enumeration of the D200
the bootstrap (stops the vendor app, pushes the embedded `deckd` to the D200 if needed and starts it) and a
`deckd` client. The web API serves `/api/info` with the real key windows, `/api/events` (key presses, status), and
stores the configuration (`/api/config`). Key images and icons are stored and drawn (the web UI composes them in the browser). Still missing: actions,
page changes, and the renderer for text that changes (placeholders, the clock).

## Hardware

ESP32-S3 devkit with two USB-C ports: **COM** (USB-UART, to the PC for flashing and the log) and
**USB-OTG** (to the D200). The solder jumpers `USB-OTG` and `IN-OUT` decide how the OTG port is
powered; check them against the schematic of your board revision. The flash layout assumes 8 MB or more.

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

`scripts/run.sh` does upload and monitor in one go and also writes the output to files
(`logs/latest.log` for the monitor, `logs/upload.log` for build and upload; `--seconds 60`, `--no-upload`,
`--fs`, `--port` see the header of the script).

Flash the web UI again whenever the files in `host/.../web/static/` change. The log shows the IP
address; open `http://<ip>/`. Attaching the D200 logs its USB ids and the ADB interface, then the bootstrap
(`installing deckd`, `starting deckd`, `deckd is listening on the device`).

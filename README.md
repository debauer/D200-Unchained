# d200-unchained

Use the Ulanzi D200 as a standalone control panel with your own software, without the
vendor app. A small Rust service, `deckd`, runs on the device and draws the 14 key
windows; a Linux host (`deck`) configures the keys, runs their actions and serves a
web configurator.

This project is not affiliated with Ulanzi. It replaces the vendor app at runtime without overwriting the
vendor firmware. Installed program files live in `/userdata/d200`; the service also writes temporary logs
and changes runtime display/backlight state. Stop the controlling host (or disconnect the ESP32), then
reboot the D200 to restore the stock UI.

## Motivation

My D200 sat on a shelf for two years. Ulanzi's own software does not run on Linux, and I am a
Linux user who wants to decide for himself what his devices do. There are projects that make the D200
usable with Home Assistant or let it trigger events on the system, but none of them were
comfortable enough, and they always lacked features I wanted. So I wrote my own.

Besides the Linux software there is a standalone variant around an ESP32-S3, which can also cover
more home automation use cases. It follows the same principle: the D200 itself is not modified, the
ESP32 loads everything onto the device via ADB. See [Two variants](#two-variants).

## Features

- **No firmware changes**: the vendor firmware stays untouched. `deckd` runs from `/userdata/d200` and
  the stock app is stopped at runtime; stop/disconnect the host and reboot to bring the original UI back.
- **Custom key images**: set an image or icon per key (file picker or drag and drop), with
  optional text on top (position, size, colour).
- **Colours**: background and text colours per key, with page-wide defaults you can change in
  one place; a key can be reset to follow the default again.
- **Subpages**: build a page tree with `page` and `back` keys, move pages around by drag and
  drop.
- **Actions**: run shell commands, change the backlight, switch the display off and on.
- **MQTT**: keys publish messages; incoming messages show values on keys and change their colours (see [docs/mqtt.md](docs/mqtt.md)).
- **Live values**: show the output of shell commands on keys (e.g. `CPU {cpu}%`, polled at an
  interval you choose), plus a built-in clock and date.
- **Web configurator**: edit and test everything in the browser, changes are saved and shown on
  the device immediately; import and export the configuration as JSON. The UI also works
  without a device.
- **Runs unattended**: `deck serve` brings the device up via `adb`, reconnects after replugging
  and reloads the configuration when it changes; an optional systemd user unit starts it at
  login.
- **Linux host local by default**: its web UI listens on `127.0.0.1` only, because it can configure commands.
  The ESP32 serves the UI on your WiFi network.

## Two variants

The same web configurator and the same on-device `deckd` work with two different hosts. The host
is the program that stores the configuration, draws the keys and reacts to key presses.

| | Linux host (`host/`) | ESP32-S3 (`esp32/`) |
|---|---|---|
| Runs on | your PC (Python, `deck serve`) | an ESP32-S3 (16 MB flash, 8 MB PSRAM) plugged into the D200's USB port |
| Needs a PC | yes, running while you use the deck | no, standalone; WiFi and power are enough |
| Connection to the D200 | `adb` over USB | ADB implemented by the firmware (USB host) |
| Web UI | `http://127.0.0.1:7280/`, local only by default | served by the ESP32 on your WiFi |
| Shell commands | key actions and variables (`sh -c`) | not available (nothing to run them on) |
| Live values | shell commands, clock | MQTT variables, clock |
| MQTT | yes | yes |
| Images and text | browser for static images, Python for dynamic keys | browser for static images, firmware for dynamic keys (same font/layout) |
| Python library | yes ([docs/python-library.md](docs/python-library.md)) | no |
| Autostart | systemd user unit | boots with the power |
| Security | no authentication yet, keep it on `127.0.0.1` | no authentication yet, anyone in your WiFi can use the UI; MQTT password is stored unencrypted in flash |
| Status | usable | work in progress, see `esp32/README.md` |

Pick the Linux host if you want shell commands on key presses or live values from your system.
Pick the ESP32 if the deck should work without a computer, for example as a home automation panel
driven by MQTT. Configurations are exchanged as JSON export/import, but features that exist on only
one side (such as `command` variables) do not work on the other. Images are separate from the JSON export.
The ESP32 accepts configurations up to 64 KiB and at most 16 MQTT variables; see
[its limits and setup](esp32/README.md#esp32-limits-and-recovery).

## Preview

The web configurator: edit a key (image, colours, text, action) and test it live.

![Web configurator, EDIT mode](img/web-edit.jpg)

![Web configurator, TEST mode with event log](img/web-test.jpg)

## Layout

| Folder | Content |
|---|---|
| `device/` | Rust crate for the device: `deckd`, `hello`, `calibrate` |
| `host/` | Python package `d200-deck` with the `deck` command and the web UI |
| `esp32/` | ESP32-S3 firmware (PlatformIO/ESP-IDF): the standalone variant, see `esp32/README.md` |
| `docs/` | `python-library.md` (write your own Python programs), `mqtt.md` (MQTT), `protocol.md` (deckd protocol), `web-api.md` (HTTP API), `hardware.md` (display, keys, layout) |
| `img/` | Screenshots for this README |

## Installation

Requirements: `adb`, Rust (`cargo`, ideally via `rustup`) and `uv`. Then:

```sh
./install.sh        # builds deckd for the D200, installs the `deck` command + shell completion
deck serve          # starts deckd on the device via adb, draws the keys, serves the web UI
```

Open <http://127.0.0.1:7280/> to configure the keys. `deck serve` stops the vendor app,
installs and starts `deckd` on the device and reconnects after replugging. The vendor firmware is not overwritten.
Stop `deck serve` (including its systemd unit, if enabled), then reboot the D200 to restore the stock UI.
To start it at login, enable the systemd user unit written by `install.sh`. See
`host/README.md` for the CLI, configuration format and autostart.

## Security

The following local-access and shell-command guidance applies to the **Linux host**. Its web configurator
can define shell commands, so treat access to it like a shell on the host. There is **no authentication yet**;
a password feature is planned. The ESP32 is network-accessible and has no shell actions; see
[ESP32 security](esp32/README.md#security) for its access controls and credential storage. Until then:

- **Keep the web UI on `127.0.0.1`** (the default). Anyone who can open it can run
  commands as the user running `deck serve`: key commands run on a key press, and
  variable commands (`sh -c`) run on their own as soon as they are saved. Do not use
  `--web 0.0.0.0:...` on an untrusted network; `deck serve` logs a warning when you do.
- **Other local users** of the same machine can reach `127.0.0.1:7280` as well. Do not run
  `deck serve` on a shared machine. For remote access use an SSH tunnel instead of
  exposing the port.
- **Browser request protection**: the API only answers requests with the expected `Host`
  header (protects against DNS rebinding) and requires the header `X-Deck-Request: 1` on
  changing requests (see `docs/web-api.md`).
- **The TEST tab never runs commands**; it only logs them. Device and page actions do run.
- **Protect the configuration**: `$XDG_CONFIG_HOME/deck/config.json` contains your commands;
  whoever can write it can run commands. Keep it writable only by you.
- **Do not put secrets into commands or key texts**: keys, variable values and the
  configuration are visible in the UI and on the device screen.
- **MQTT**: the broker password is stored in `$XDG_CONFIG_HOME/deck/mqtt.json` (mode 0600), kept out
  of `config.json` and the export, and never returned by the API. Incoming messages are only shown,
  never executed. Use TLS for a broker that is not on your own machine; see `docs/mqtt.md`.
- **Device side**: `deckd` listens on `127.0.0.1:7200` on the D200 only and is reached through
  `adb forward`, which makes the port available to local users of the host. `deckd` has no
  authentication either, so anyone with access to that port can draw on the screen. `adb`
  itself gives a root shell on the stock firmware: keep the device physically controlled.

Planned: a password for the web UI and API. Until it exists, do not expose the UI beyond
your own machine. Please report security problems privately to the repository owner rather
than in a public issue.

## Your own programs

The Python package can also be used as a library: draw on keys, react to key presses, show
your own data. See [docs/python-library.md](docs/python-library.md).

## Development

The project has two parts that you can work on separately:

- **`device/`** is the Rust program `deckd` that runs on the D200. It draws the keys and
  reports key presses. It is built for the device's ARMv7 CPU (set in `device/.cargo/config.toml`),
  so a plain `cargo build` produces a binary that only runs on the D200. `./install.sh` does this
  build for you.
- **`host/`** is the Python program `deck` that runs on your PC. It contains the configuration,
  the key actions and the web UI (`host/src/d200_deck/web/static/`, plain JavaScript without a
  build step). See `host/README.md` for details.

To work on the host code:

```sh
cd host
uv sync                                         # create the virtual environment
uv run deck serve --verbose                     # run the service from your checkout
uv run ruff check . && uv run mypy              # lint and type check
```

## Tests

Run them from the repository root:

```sh
(cd host && uv run pytest)                                         # host and web UI tests
(cd device && cargo test --target x86_64-unknown-linux-gnu)          # device tests, run on your PC
```

The device tests run on your PC (x86_64), not on the D200, which is why the target is given.
One optional host test starts a `deckd` built for your PC against fake files; it is skipped unless
you build that binary first:

```sh
cd device && cargo build --target x86_64-unknown-linux-gnu --bin deckd
```

## AI assistance

This project was built with the help of AI. That applies to the
software and especially to the research behind it: analysing the firmware, working out the
hardware and the display and key layout, and designing the protocol. Everything was tested
on a real D200, but treat it as hobby software: review the code before you rely on it,
particularly the parts that run commands.

## License

MIT, see `LICENSE`.

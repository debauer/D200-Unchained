# ulanzi_d200_tooling

Use the Ulanzi D200 as a standalone control panel with your own software, without the
vendor app. A small Rust service, `deckd`, runs on the device and draws the 14 key
windows; a Linux host (`deck`) configures the keys, runs their actions and serves a
web configurator.

This project is not affiliated with Ulanzi. It replaces the vendor app at runtime and
changes nothing outside `/userdata/d200` on the device; `adb reboot` restores the stock UI.

## Preview

The web configurator: edit a key (image, colours, text, action) and test it live.

![Web configurator, EDIT mode](img/web-edit.jpg)

![Web configurator, TEST mode with event log](img/web-test.jpg)

| Folder | Content |
|---|---|
| `device/` | Rust crate for the device: `deckd`, `hello`, `calibrate` |
| `host/` | Python package `d200-deck` with the `deck` command and the web UI |
| `docs/` | `protocol.md` (deckd protocol), `web-api.md` (HTTP API), `hardware.md` (display, keys, layout) |

## Installation

Requirements: `adb`, Rust (`cargo`, ideally via `rustup`) and `uv`. Then:

```sh
./install.sh        # builds deckd for the D200, installs the `deck` command + shell completion
deck serve          # starts deckd on the device via adb, draws the keys, serves the web UI
```

Open <http://127.0.0.1:7280/> to configure the keys. `deck serve` stops the vendor app,
installs and starts `deckd` on the device and reconnects after replugging. Nothing
outside `/userdata/d200` is changed on the device; `adb reboot` restores the stock UI.
To start it at login, enable the systemd user unit written by `install.sh`. See
`host/README.md` for the CLI, configuration format and autostart.

## Development

- Device code: `device/` (Cargo, target ARMv7 musl via `.cargo/config.toml`).
- Host client: see `host/README.md`.

## Tests

```sh
cd device && cargo test --target x86_64-unknown-linux-gnu
cd device && cargo build --target x86_64-unknown-linux-gnu --bin deckd   # for the E2E test
cd host && uv run pytest && uv run ruff check . && uv run mypy
```

## License

MIT, see `LICENSE`.

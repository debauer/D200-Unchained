# d200-deck

Host client for `deckd`, the custom display service on the Ulanzi D200
(protocol: `../docs/protocol.md`). Runs on a Linux host.

## Installation

```sh
../install.sh               # builds deckd, installs the `deck` command for the current user (uv tool) + completion
```

## Usage

`deck serve` starts `deckd` on the device by itself. For the one-shot commands below it
must be running (`deck serve` in the background) or started by hand with port 7200 forwarded:

```sh
deck info                   # protocol, key windows (index + hardware code) and hardware info
deck brightness 255
deck clear 000000
deck fill 0 ff8800          # key 0 orange
deck image 0 icon.png       # image scaled into the window (--fit contain|cover|stretch, --background RRGGBB)
deck image 0 icon.png -l Home   # plus text below the image (--label-color, --label-height, --font)
deck events                 # print key presses (--json for machine output)
deck demo                   # gradient on all keys, a pressed key turns white
```

Keys are addressed by their **index 0-13** (physically row by row from the top
left). `deck info` also shows the hardware code of each key.

Host and port: `--host/-H`, `--port/-p` or `DECK_HOST`, `DECK_PORT`.

## Service: `deck serve`

Keeps the connection to `deckd`, draws the keys from the configuration and runs
the action on a key press. Reloads the configuration as soon as the file changes
or `SIGHUP` arrives, and reconnects after drops. `deckd` allows several clients,
so `deck fill` & co. work in parallel.

```sh
deck serve --verbose        # configuration: $XDG_CONFIG_HOME/deck/config.json
```

**Bringing the device up automatically**: if `deckd` is not reachable (D200
restarted or replugged), `deck serve` does this via `adb`: stop the vendor app, start `/userdata/d200/deckd`, forward the port,
then redraw all keys. `--no-adb` turns this off, `--adb-path` selects the adb
binary, `--deckd-binary <file>` overrides the `deckd` that `install.sh` built into
`$XDG_DATA_HOME/deck/deckd` (pushed to the device whenever `deckd` is not running).

**Autostart (systemd)**: `install.sh` creates `~/.config/systemd/user/deck.service`
but does not enable it:

```sh
systemctl --user daemon-reload && systemctl --user enable --now deck.service
journalctl --user -u deck -f          # log
systemctl --user reload deck          # reload the configuration (SIGHUP)
```

The user service only sees the default `PATH`; if `adb` lives elsewhere, set
`ExecStart=... serve --adb-path /path/to/adb` in the unit.

**Web interface** ("D200 // Configurator"): `deck serve` serves it at
<http://127.0.0.1:7280/> (`--web HOST:PORT`, `--no-web`).

![EDIT mode](../img/web-edit.jpg)

![TEST mode](../img/web-test.jpg)

- **EDIT**: click a key and design it on the right: image (choose a file or drop
  it), background colour, text with colour (quick pick or colour picker), position
  and size, action (command, brightness -/+, display off/on). Changes are saved
  automatically and applied to the device immediately.
- **TEST**: a click triggers the real action (except commands, which are only
  reported); the event log shows actions and key presses on the device, the status
  line the current brightness.
- **Pages**: the page tree is on the left ("+ Subpage" adds a subpage of the open
  page, with a back key). The action "Page" opens a subpage, "Back" the parent
  page; double-clicking a Page key opens its target, "FREE" marks pages no key
  links to; subpages can be moved by drag and drop (links from the old parent are
  cleared). The interface always shows the page the device shows.
- **Import/Export** of the configuration as JSON (without key images).

Images and text are rendered in the browser; the same files
(`src/d200_deck/web/static/`) and the same API (`../docs/web-api.md`) will be used
by the ESP32 later. Reachable only via `127.0.0.1` by default, because commands can
be configured through it; for other machines use `--web 0.0.0.0:7280` plus
`--web-allow-host <name>:7280`.

Configuration (JSON, same format later on the ESP32; version 1 files with a flat
`keys` list are still read):

```json
{
  "version": 2,
  "brightness": 200,
  "background": "000000",
  "pages": [
    {"id": "root", "name": "Default", "parent": null, "keys": [
      {"index": 0, "label": "Play", "color": "ff8800",
       "action": {"type": "command", "argv": ["playerctl", "play-pause"]}},
      {"index": 1, "label": "Lights", "action": {"type": "page", "target": "lights"}}
    ]},
    {"id": "lights", "name": "Lights", "parent": "root", "keys": [
      {"index": 0, "label": "Back", "action": {"type": "back"}}
    ]}
  ]
}
```

- `variables`: values for placeholders in key text, e.g. text `CPU {cpu}%` with
  `{"name": "cpu", "type": "command", "command": "uptime | cut -d, -f1", "interval": 5}`.
  The command runs on the host through `sh -c` (pipes work, 2 s time limit) and
  prints the value on stdout; `!` on the key means the command failed. In the web
  UI the field holds the plain shell command (no extra quoting); only inside the
  JSON file are quotes escaped, e.g.
  `"command": "free | awk '/Mem:/ {printf \"%d\", $3/$2*100}'"`.
  `{time}` and `{date}` are built in; `{{` and `}}` give literal braces. Only
  variables of the shown page are polled. The wide info window shows the clock as
  long as it has neither image nor text.
- Pages form a tree below `root`; `page` opens a subpage, `back` goes to the
  parent page. The device starts on `root`. A page can set `color` (background)
  and `label_color` (text) as defaults for its keys; they apply to empty keys and
  keys without their own colour (otherwise `background`/white). `accent_color` is
  the second text colour of two-colour elements: the clock shows the time in
  `label_color` (default cyan) and the date in `accent_color` (default magenta)
  on `color`.
- MQTT (broker in the web UI, see `../docs/mqtt.md`): `variables` of type `mqtt`
  (`{"name": "light", "type": "mqtt", "topic": "home/light", "json_path": "state.power"}`) show
  the latest message, the action `{"type": "mqtt", "topic": ..., "payload": ..., "retain": false, "qos": 0}`
  publishes one, and a key's `rules` (`{"variable": "light", "value": "ON", "color": "ffcc00",
  "label_color": "000000"}`) change its colours while a variable has that value.
- `color`: fill colour of the key (default: `background`).
- `image`: RGB565 file in `$XDG_DATA_HOME/deck/images/`, exactly the size of the
  key window (`deck info`); if it does not fit, `color` is used.
- `action`: `command` with an argument list (no shell; runs in the background,
  the exit code is logged) or `bl_down`, `bl_up` (brightness -/+ 20 %),
  `disp_off`, `disp_on`, `page` (with `target`), `back`, `mqtt` (publish a message).
  Unknown types (e.g. from the ESP32) are kept and ignored.
- `brightness`: start brightness; key actions only change the runtime value.
- `label`, `label_color`, `label_pos` (`top`/`center`/`bottom`), `label_size`
  (50-250 % of the automatic size; text that is too long is still fitted): text
  of the key; the web interface renders it into the key image.

## As a library

A longer guide with examples (images, key callbacks) is in [../docs/python-library.md](../docs/python-library.md).

```python
from d200_deck.device.client import DeckClient
from d200_deck.device.events import KeyDispatcher
from d200_deck.device.protocol import Color

with DeckClient.connect() as deck:
    deck.hello()
    deck.fill_key(0, Color(255, 0, 0))
    keys = KeyDispatcher(deck)
    keys.on_press(0, lambda: print("key 0"))
    keys.run()  # until Ctrl-C; callbacks run in this thread
```

## Development

```sh
uv sync                 # .venv with all development dependencies
uv run ruff check . && uv run ruff format --check .
uv run mypy
```

## Tests

```sh
uv run pytest
```

The JavaScript tests of the web interface (`tests/web/*.test.mjs`) run with `node`
as part of `pytest` (skipped if `node` is missing).

The end-to-end test starts a `deckd` built on the PC against stand-in files for
framebuffer, keys and backlight. It is skipped if the binary is missing:

```sh
(cd ../device && cargo build --target x86_64-unknown-linux-gnu --bin deckd)
```

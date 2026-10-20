# Web API (version 1)

Changes stay backward compatible (only new fields and events); ignore unknown
fields and events.

HTTP API and web UI of the host service. **The Linux host (`deck serve`) and an
ESP32 implement the same API** and serve the same static files
(`host/src/d200_deck/web/static/`), so the UI is identical on both.

## Principles

- JSON in UTF-8; errors as `{"error": "<text>"}` with a matching status code.
- The configuration has the same format as the configuration file
  (`host/README.md`).
- Images are scaled to the window size **in the browser** and uploaded as raw
  RGB565 (little endian, row by row); the host does not decode PNGs.

## Security

The API can configure actions that the host executes. Therefore:

- **Served only to allowed hosts**: the `Host` header must match the address the
  service listens on (or `localhost:<port>`), otherwise `403`. Protects against
  DNS rebinding. On ESP32 use the numeric IPv4 address of the interface receiving the request
  (with optional `:80`); arbitrary DNS names and `localhost` are not accepted.
- **Changing requests** (`PUT`, `POST`, `DELETE`) must carry the header
  `X-Deck-Request: 1`, otherwise `403`. A foreign origin cannot set this header
  without a CORS preflight, and the service does not answer preflights.
- The Linux host listens on `127.0.0.1` only by default.

## Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/`, `/<file>` | Static UI (`index.html`, `app.js`, ...) |
| GET | `/api/info` | Host, connection state, screen, key windows, action types |
| GET | `/api/config` | Current configuration |
| PUT | `/api/config` | Replace the configuration (validated, saved, applied) |
| POST | `/api/page` | `{"page": "<id>"}`: show a page on the device; `404` for an unknown page |
| GET | `/api/keys/<index>/image` | Raw RGB565 of the key image, `404` if none |
| PUT | `/api/keys/<index>/image` | Set the key image (body: RGB565, exactly the window size) |
| DELETE | `/api/keys/<index>/image` | Remove the key image (the key shows `color` again) |
| GET | `/api/keys/<index>/icon` | Raw RGB565 of the unlabelled icon, `404` if none |
| PUT | `/api/keys/<index>/icon` | Store an icon (body: RGB565, exactly the window size); it is not displayed |
| DELETE | `/api/keys/<index>/icon` | Remove the icon |
| POST | `/api/keys/<index>/press` | Test the key's action (UI test mode): `command` actions are only reported, never started (`(test, not run) $ ...`); device and page actions run; `{"started": bool}`, `503` if the device is unreachable for a device action |
| GET | `/api/keys/<index>/alticon` | Raw RGB565 of the second icon (shown while a colour rule matches), `404` if none |
| PUT | `/api/keys/<index>/alticon` | Store the second icon (body: RGB565, exactly the window size) |
| DELETE | `/api/keys/<index>/alticon` | Remove the second icon |
| GET | `/api/log?since=N` | Optional (`log: true` in `/api/info`): the last log lines of the host as text from line number `N` on; the header `X-Log-Next` is the number to ask for next (smaller than the one asked for: the host restarted). The UI shows it as a "Host log" with download |
| GET | `/api/mqtt` | MQTT broker settings and state (never the password) |
| PUT | `/api/mqtt` | Set the broker settings; a missing `password` keeps the stored one, `null` clears it |
| GET | `/api/events` | Server-sent events with key presses |

All `/api/keys/<index>/...` endpoints also exist per page as
`/api/pages/<page>/keys/<index>/...`; without a page, `root` applies. Unknown page: `404`.

### `GET /api/info`

```json
{
  "api": 1,
  "host": "linux",
  "connected": true,
  "screen": {"width": 1280, "height": 720},
  "keys": [{"index": 0, "code": 29, "x": 11, "y": 3, "width": 196, "height": 196}],
  "actions": ["command", "mqtt", "bl_down", "bl_up", "disp_off", "disp_on", "page", "back"],
  "brightness": {"level": 204, "percent": 80, "display_on": true},
  "page": "root",
  "values": {"cpu": "42", "time": "14:41", "date": "Fri 17.10."},
  "variable_types": ["command", "mqtt"]
}
```

- `host`: `"linux"` or `"esp32"`.
- `log` (optional): `true` when the host serves `/api/log`; the UI then shows the host log.
- `storage` (optional): `{"total": <bytes>, "used": <bytes>}`, the host's memory for key images and the configuration
  (ESP32). The UI shows a meter when it is there.
- `connected`: whether the host is currently connected to `deckd`. Without a
  connection, `screen` and `keys` are the last reported values; before the first
  connection they are the measured D200 grid (`host/src/d200_deck/device/layout.py`).
- `actions`: action types this host can execute. The UI offers only these. Both
  hosts: `bl_down`, `bl_up` (brightness -/+ 20 %), `disp_off`, `disp_on`, `page`
  (open subpage `target`), `back` (to the parent page). Linux additionally
  `command`; both hosts `mqtt` (publish a message, see [mqtt.md](mqtt.md)); an ESP32 later e.g. `http`. Unknown types in the
  configuration are kept but not executed.
- `page`: the page the device is currently showing.
- `values`: current values of the placeholder variables (only variables of the
  shown page are polled) plus the built-in `time` and `date`. `!` = command
  failed, `?` = type not supported by this host.
- `variable_types`: variable types this host can read (Linux: `command`, `mqtt`; ESP32: `mqtt`,
  later e.g. `http`).
- `brightness`: current runtime state. `level` (0-255) applies while the display is
  on; `brightness` in the configuration is only the start value and sets `level`
  when it changes.

### `GET /api/mqtt`, `PUT /api/mqtt`

```json
{"host": "broker", "port": 1883, "username": "me", "tls": false, "client_id": "",
 "password_set": true, "enabled": true, "connected": true, "error": ""}
```

`PUT` takes `host`, `port`, `username`, `tls`, `client_id` and `password` (all optional; an
empty `host` turns MQTT off) and answers with the same object, `400` for invalid values. The
password is write-only: it is stored in `mqtt.json` (mode 0600 on Linux), never returned and not part of
`/api/config`. On the ESP32 it is stored in the unencrypted `storage` partition rather than a user-protected
host file. The test press (`POST .../press`) only reports an `mqtt` action, it does not publish.

### `PUT /api/config`

Body: the complete configuration (version 2 with `pages`; version 1 with a flat
`keys` list is read as page `root`). Reply `200` with the saved configuration,
`400` with an error text for invalid content (the old configuration is kept).

### `PUT /api/keys/<index>/image`

`Content-Type: application/octet-stream`, length exactly `width * height * 2` of
the key window. The host stores the image as `key-<index>.rgb565`, enters it in
the configuration and shows it. Reply `200` with the key entry of the
configuration; `400` for a wrong size, `404` for an unknown index.

### Icon and key image

The UI composes the displayed key image in the browser: colour, icon and label.
So that label or colour can be changed later without uploading the icon again, it
stores the **unlabelled icon** (scaled to the window size) separately. The host only
stores it (`key-<index>-icon.rgb565`) and does not display it.

### `GET /api/events`

`Content-Type: text/event-stream`. For each key press:

```
event: key
data: {"index": 3, "pressed": true}

```

When the connection to `deckd` changes:

```
event: status
data: {"connected": false}

```

For every executed action (key press or `press`):

```
event: action
data: {"index": 2, "type": "bl_up", "detail": "backlight 100%"}

```

On every page change of the device (`page`/`back` key, `POST /api/page`), and
once right after connecting:

```
event: page
data: {"page": "lights"}

```

When a variable value changes, and once right after connecting (all values):

```
event: values
data: {"cpu": "43", "time": "14:42", "date": "Fri 17.10."}

```

When the brightness changes, and once right after connecting (like `status`):

```
event: brightness
data: {"level": 255, "percent": 100, "display_on": true}

```

When the broker state changes, and once right after connecting (same object as `GET /api/mqtt`):

```
event: mqtt
data: {"host": "broker", "port": 1883, "enabled": true, "connected": true, "error": ""}

```

A comment (`: keepalive`) every 15 s so proxies keep the connection open.

## Dynamic keys

Keys whose text contains placeholders (`{name}`) and the empty info window (clock)
are rendered **by the host** itself and refreshed when a value changes. For such
keys the UI uploads only the icon (`.../icon`), not a key image.

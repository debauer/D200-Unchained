# deckd protocol (version 5)

Binary protocol between a host (PC, Raspberry Pi, microcontroller) and the `deckd`
service on the D200. Goal: simple enough for an ESP32.

## Transport

- `deckd` listens on **TCP 127.0.0.1:7200** on the device.
- PC/Pi: `adb forward tcp:7200 tcp:7200`, then connect to `127.0.0.1:7200`.
- Microcontroller: open the ADB stream `tcp:7200`.
- **Several clients at once** (up to 8). Commands from all clients are executed
  one after the other on the device; the screen is shared, the last command
  wins. An additional client receives `ERROR "too many clients"` and is
  disconnected.
- A client that does not accept a frame within 2 s is disconnected.
- A dropped connection does not stop `deckd`; the display keeps its content.

## Frames

All numbers are **little endian**.

```
+--------+----------------+-------------------+
| type   | length         | payload           |
| u8     | u32            | length bytes      |
+--------+----------------+-------------------+
```

Maximum payload: 4 MiB. Larger frames close the connection.

## Flow

- Every command from the host is answered in order with exactly one reply:
  `OK` or `ERROR`, `INFO` for `HELLO`, `SYSINFO_REPLY` for `SYSINFO`.
- Replies go only to the client that sent the command.
- `KEY_EVENT` goes to **all** clients that have sent `HELLO` and can arrive
  at any time between replies.
- Recommended start: send `HELLO`, read `INFO`.

## Commands (host -> device)

| type | Name | Payload | Reply |
|---|---|---|---|
| 0x01 | HELLO | empty | `INFO` |
| 0x02 | SET_BRIGHTNESS | `u8 value` (0-255) | `OK` |
| 0x03 | CLEAR | `u8 r, u8 g, u8 b` | `OK` |
| 0x04 | FILL_KEY | `u16 index, u8 r, u8 g, u8 b` | `OK` |
| 0x05 | SET_KEY_IMAGE | `u16 index, u8 format, u16 w, u16 h, pixels` | `OK` |
| 0x06 | PING | empty | `OK` |
| 0x07 | SYSINFO | empty | `SYSINFO_REPLY` |

- Keys are addressed by their **index** (0-based), not by the hardware key
  code. The index counts physically row by row from the top left.
- `CLEAR` fills the whole screen, including the gaps between the windows.
- `SET_KEY_IMAGE`: the image must not be larger than the key window and is
  centred in it; the border is left unchanged.
  - `format 0` = RGB565, 2 bytes per pixel (u16 LE: `rrrrrggg gggbbbbb`)
  - `format 1` = RGB888, 3 bytes per pixel (R, G, B)
  - `format 2` = RGB565, run-length coded (much smaller for flat areas and text). A control byte `c`:
    `0..=127` is followed by `c + 1` pixels that are copied; `128..=255` is followed by one pixel that is
    repeated `c - 126` times (2..129). The runs must add up to exactly `w * h` pixels.
  - Pixels row by row from the top left, `len(pixels) = w * h * bpp` (formats 0 and 1).
- Unknown `index` or invalid payload -> `ERROR`.

## Replies and events (device -> host)

| type | Name | Payload |
|---|---|---|
| 0x80 | OK | empty |
| 0x81 | INFO | see below |
| 0x82 | SYSINFO_REPLY | see below |
| 0x90 | KEY_EVENT | `u16 index, u8 pressed` (1 = pressed, 0 = released) |
| 0xFF | ERROR | UTF-8 text |

`INFO`:

```
u16 protocol_version   (5)
u16 screen_width       (1280)
u16 screen_height      (720)
u8  key_count          (14)
key_count times:
  u16 index, u16 code, u16 x, u16 y, u16 w, u16 h    (key window, landscape)
```

`index` is the physical order, row by row from the top left (0-based);
`code` is the hardware key code of the key matrix (informational only).

`SYSINFO_REPLY`: UTF-8 text, one fact per line as `key=value` (separated by `\n`).
Facts that cannot be read are missing; ignore unknown lines. Known keys:

| Key | Meaning |
|---|---|
| `cpu_cores`, `cpu_model` | Number of cores, model (from `/proc/cpuinfo`) |
| `arch`, `kernel` | Architecture, kernel release |
| `mem_total_kb`, `mem_free_kb`, `mem_available_kb` | RAM in kB (`/proc/meminfo`) |
| `swap_total_kb`, `swap_free_kb` | Swap in kB |
| `userdata_total_bytes`, `userdata_free_bytes` | Space on `/userdata` |
| `root_total_bytes`, `root_free_bytes` | Space on `/` |
| `uptime_s` | Uptime in seconds |
| `loadavg` | Load over 1/5/15 minutes |
| `temp_mc` | Temperature in millidegrees Celsius (if a sensor exists) |
| `serial` | Serial number from `/proc/cmdline` |

## Versions

| Version | Change |
|---|---|
| 5 | `SET_KEY_IMAGE` format 2 (run-length coded RGB565) |
| 4 | Several clients at once, events to all (before: a new connection replaced the old one) |
| 3 | `SYSINFO` |
| 2 | Keys by index instead of key code, `INFO` contains index and code |
| 1 | First version |

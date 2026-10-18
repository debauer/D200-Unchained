# D200 hardware (measured on the device)

Source: probing a real device on 2026-10-16, plus the vendor device tree. The measured
device ran a build from 2025-01-13 (kernel 5.10.160, built 2024-11-11).

## System

| | |
|---|---|
| SoC | Rockchip RK3308HS, 4x Cortex-A35, kernel runs 32-bit (`armv7l`) |
| RAM | 118 MiB total, about 84 MiB available, no swap |
| Storage | NAND (`rkflash0`); `/` ext2 full (95M/95M), `/userdata` 52M (47M free), `/oem` 27M |
| `/tmp` | tmpfs, 59M |

## SoC interfaces (RK3308HS)

Theoretical peripherals of the SoC, from the mainline `rk3308.dtsi` and the vendor
device tree. The board DT is called `rockchip,rk3308hs-voice-module-board-v10-aarch32`,
so the D200 is based on Rockchip's voice-module reference design.

| Block | Interfaces |
|---|---|
| CPU | 4x Cortex-A35 (mainline up to 1.008 GHz; D200 OPP table up to 1.2 GHz, 1.296 GHz disabled) |
| Memory | DDR controller (DMC) with DVFS, 256 KiB SRAM |
| Mass storage | SFC (SPI NAND/NOR), NFC (parallel NAND), SDMMC, eMMC (8 bit), SDIO |
| USB | 1x USB 2.0 OTG (dwc2) + 1x USB 2.0 host (EHCI/OHCI), 2 PHYs |
| Network | GMAC (RMII, 10/100 Mbit) |
| Display | VOP with RGB/DPI output |
| Audio | 4x I2S/TDM (8ch), 2x I2S (2ch), PDM (8ch), S/PDIF TX and RX, VAD, internal codec (8 record / 2 playback channels) |
| Serial | 5x UART, 4x I2C, 3x SPI |
| Analog | SARADC, TSADC (temperature), 12x PWM |
| Other | Timers, RTC, watchdog, eFuse/OTP, TRNG, 2x PL330 DMA, 5x GPIO bank, PMU/PVTM |

Active on the D200 according to the vendor device tree:

- **Display:** VOP (RGB) -> `simple-panel`, backlight via `pwm-backlight`.
- **Keys:** `gpio-matrix-keypad`.
- **USB:** OTG port as gadget (ADB + 2x HID); host controller enabled in the DT but
  not routed to the outside.
- **Boot/storage:** SFC (SPI NAND).
- **Power:** CPU core voltage via PWM regulator (`vdd-core`).
- **Also active:** RTC, TSADC, SARADC, TRNG, I2S2 8ch sound card with codec.
- **Disabled in the DT:** Ethernet, SD/eMMC/SDIO, I2C, SPI0-2, further UARTs
  (the console runs over the FIQ debugger on UART2), PDM, S/PDIF, VAD,
  watchdog, ADC keys.

### Kernel drivers (firmware 2.0.3)

The rootfs contains **no kernel modules** (no `/lib/modules`, no `.ko`); available
drivers are built in. State according to `/info/config-5.10` and
`/info/System.map-5.10`:

| Interface | State in the kernel |
|---|---|
| Ethernet (GMAC) | built in (`DWMAC_ROCKCHIP`, stmmac) |
| SD/eMMC/SDIO | built in (`MMC_DW_ROCKCHIP`) |
| I2C | built in (`I2C_RK3X`) |
| SPI | built in (`SPI_ROCKCHIP`) |
| UART x5 | built in (`SERIAL_8250_DW`) |
| PWM | built in (`PWM_ROCKCHIP`; 2 used, rest free) |
| GPIO/pinctrl | built in |
| SARADC | built in (`ROCKCHIP_SARADC`) |
| TSADC/thermal | built in (`ROCKCHIP_THERMAL`, used) |
| Watchdog | built in (`DW_WATCHDOG`) |
| SPI NAND (SFC) | built in (`RK_FLASH`, no MTD; used) |
| USB OTG | built in (`USB_DWC2` peripheral; used) |
| USB host (EHCI/OHCI) | **not configured** |
| Audio (I2S, PDM, S/PDIF, VAD, codec) | **no sound subsystem** (`CONFIG_SOUND` not set) |
| RTC | **no driver configured** |
| TRNG (cryptov2-rng) | **not configured** |

There are also 159 `=m` options (mostly DVB tuners, but also ext4/vfat/ntfs) whose
modules are not in the rootfs and therefore cannot be loaded.

Conclusion: for most unused interfaces (Ethernet, SD, I2C, SPI, UART, watchdog) the
driver is present; audio, RTC, USB host and TRNG have none. In practice only what
has pads routed out is usable.

## Display

| | |
|---|---|
| Device | `/dev/fb0` (`rockchipdrmfb`, DRM fbdev emulation); DRM: `/dev/dri/card0`, connector `DPI-1` |
| Resolution | 720 x 1280 (natively portrait), physically 217 x 136 mm -> mounted in landscape |
| Format | 32 bpp, XRGB8888 (`rgba 8/16,8/8,8/0,0/0`), little endian: byte order B, G, R, X |
| Stride | 2880 bytes (= 720 * 4, no padding) |
| Buffer | single (`virtual_size` = visible), no page flipping via fbdev |
| Rotation | `rotate: 0` in fbdev; the panel is mounted rotated by 90 degrees (see below) |

### Orientation

Determined with `hello` (coloured corners) and a photo. Physical landscape
coordinates: 1280 x 720, origin top left, x to the right, y downwards.

| Landscape (x, y) | Framebuffer |
|---|---|
| top left (0, 0) | (0, 1279) |
| top right (1279, 0) | (0, 0) |
| bottom left (0, 719) | (719, 1279) |

Conversion: `fb_x = y`, `fb_y = 1279 - x` (implemented in `device/src/screen.rs`).

## Keys

| | |
|---|---|
| Device | `/dev/input/event0`, name `matrix-keypad` |
| Type | `gpio-matrix-keypad`, 3 rows x 5 columns, `no-autorepeat`, debounce 50 ms |
| Events | `EV_KEY` (value 1 = pressed, 0 = released), plus `EV_MSC`/`MSC_SCAN` |
| Event size | 16 bytes (32-bit kernel: sec u32, usec u32, type u16, code u16, value s32) |

Keymap from the device tree (row, column -> keycode):

| | Column 0 | Column 1 | Column 2 | Column 3 | Column 4 |
|---|---|---|---|---|---|
| Row 0 | 7 | 8 | 9 | 10 | 11 |
| Row 1 | 12 | 13 | 14 | 15 | 29 |
| Row 2 | 30 | - | 31 | 33 | 34 |

Physical position (measured with `hello`, pressed row by row from the top left):

| | Column 0 | Column 1 | Column 2 | Column 3 | Column 4 |
|---|---|---|---|---|---|
| top | 29 | 15 | 14 | 13 | 12 |
| middle | 11 | 10 | 9 | 8 | 7 |
| bottom | 34 | 33 | 31 | 30 (wide, columns 3+4) | |

Display windows in landscape coordinates, **calibrated on the device** with
`calibrate` (colour ladder, viewed straight on): a regular grid of square windows.

| Parameter | Value |
|---|---|
| x0, y0 (top left) | 11, 3 |
| Column pitch | 260 px |
| Row pitch | 259 px |
| Window size | 196 x 196 px |

Window x per column: 11 / 271 / 531 / 791 / 1051, window y per row: 3 / 262 / 521.
The wide key 30 spans columns 3 and 4 (x = 791 to 1247, 456 px wide).
Source of the values in code: `device/src/layout.rs`.

## Backlight

`/sys/class/backlight/backlight/brightness`, range 0-255 (`max_brightness` 255).

## USB gadget (running)

Functions in `configs/b.1`: `ffs.adb`, `hid.usb0`, `hid.usb1`.
Device nodes `/dev/hidg0`, `/dev/hidg1` (root only).

### Quirks of `adbd` (android-tools 4.2.2)

- **No exit code**: `adb shell <cmd>` always succeeds on the host. Checks must
  evaluate the output (e.g. `netstat -tln | grep :7200`).
- **Background processes are cleaned up**: if `adb shell "... &"` returns
  immediately, the job is killed before it detaches. Workaround:
  `adb shell "setsid <cmd> </dev/null >log 2>&1 & sleep 1"`.
- `netstat` reports `/proc/net/tcp6: No such file or directory` (no IPv6);
  the IPv4 lines still appear.

## Running vendor processes

| Process | Note |
|---|---|
| `/userdata/UlanziDeckKey -platform linuxfb` | holds `/dev/fb0`, `/dev/input/event0`, `/dev/hidg*` |
| `/etc/WatcherProcess -platform linuxfb` | restarts the app if the PID from `/tmp/app.pid` is missing |
| `input-event-daemon` | Buildroot default configuration, irrelevant for the matrix keys |

The first two keep **each other** alive: the app restarts the watcher via
`pgrep`/`QProcess::startDetached`, the watcher restarts the app. Killing them one
after the other can therefore fail. The right way: stop both, then kill both
(`killall -STOP ...; killall -KILL ...`) and check the result with `pidof`
(done by `deck serve`, `host/src/d200_deck/service/bootstrap.py`). If the app keeps running, it draws into the
framebuffer in parallel to `deckd` (flicker).

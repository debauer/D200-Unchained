"""Embed what the firmware needs from the rest of the repository: main/blobs.S includes

  deckd       the device daemon (ARMv7 musl) from ../install.sh or `cargo build --release --target
              armv7-unknown-linux-musleabihf` in ../device; DECKD_BINARY overrides the search
  font_bold / font_medium
              Chakra Petch 700 / 500 as TrueType, converted from the woff2 files of the web interface
              (host/src/d200_deck/web/static/fonts), so both hosts draw with the same font

and main/wifi_local.h holds the WiFi login from `wifi.local` (not checked in, see wifi.local.example).
"""

import os
import sys
from pathlib import Path

Import("env")  # noqa: F821  (provided by PlatformIO)

project = Path(env["PROJECT_DIR"])  # noqa: F821
sys.path.insert(0, str(project / "scripts"))
import auth_config

try:
    web_config = auth_config.load(project, os.environ.get("DECK_AUTH_CONFIG"))
except (OSError, ValueError) as error:
    sys.stderr.write(f"invalid auth.local.json: {error}\n")
    env.Exit(1)  # noqa: F821
web_header = project / "main/web_local.h"
web_text = auth_config.header(web_config)
if not web_header.exists() or web_header.read_text() != web_text:
    web_header.write_text(web_text)
data_home = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
fonts_dir = project.parent / "host/src/d200_deck/web/static/fonts"


def fail(message):
    sys.stderr.write(message + "\n")
    env.Exit(1)  # noqa: F821


def find_deckd():
    candidates = [
        os.environ.get("DECKD_BINARY"),
        project.parent / "device/target/armv7-unknown-linux-musleabihf/release/deckd",
        data_home / "deck/deckd",
    ]
    found = next((Path(c) for c in candidates if c and Path(c).is_file()), None)
    if found is None:
        fail("deckd binary not found: run ../install.sh or set DECKD_BINARY")
    return found


def convert_font(weight):
    """woff2 -> ttf (stb_truetype reads TrueType only); rewritten only when the source is newer."""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        fail("fontTools/brotli missing: run `uv sync` in esp32/")
    source = fonts_dir / f"chakra-petch-{weight}.woff2"
    target = project / "main/fonts" / f"chakra-petch-{weight}.ttf"
    if not source.is_file():
        fail(f"font not found: {source}")
    if not target.exists() or target.stat().st_mtime < source.stat().st_mtime:
        target.parent.mkdir(exist_ok=True)
        font = TTFont(str(source))
        font.flavor = None
        font.save(str(target))
    return target


def section(name, path):
    body = f'.incbin "{path.resolve()}"' if path is not None else ""
    return f""".global {name}_start
{name}_start:
{body}
.global {name}_end
{name}_end:
.byte 0
"""


deckd = find_deckd()
blobs = ".section .rodata.blobs,\"a\",@progbits\n.balign 4\n" + "".join(
    section(name, path)
    for name, path in (("deckd", deckd), ("font_bold", convert_font(700)), ("font_medium", convert_font(500)),
                       ("web_cert", web_config["cert"]), ("web_key", web_config["key"]))
)
target = project / "main/blobs.S"
if not target.exists() or target.read_text() != blobs:
    target.write_text(blobs)
print(f"embedding deckd from {deckd} and Chakra Petch from {fonts_dir}")


def wifi_header():
    """The WiFi login as C strings, from the untracked `wifi.local` (lines `ssid=...` and `password=...`)."""
    values = {"ssid": "", "password": ""}
    path = project / "wifi.local"
    if path.exists():
        for line in path.read_text().splitlines():
            key, sep, value = line.partition("=")
            if sep and key.strip() in values:
                values[key.strip()] = value.strip()
    else:
        print("no wifi.local: WiFi stays off (copy wifi.local.example)")
    quote = lambda text: '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return f"#define WIFI_SSID {quote(values['ssid'])}\n#define WIFI_PASSWORD {quote(values['password'])}\n"


header = project / "main/wifi_local.h"
text = wifi_header()
if not header.exists() or header.read_text() != text:
    header.write_text(text)

"""deckd protocol v1: message types, typed models and pure encode/decode helpers."""

from __future__ import annotations

import re
import struct
from collections.abc import Iterable
from dataclasses import dataclass
from enum import IntEnum

from d200_deck.constants import HEADER_SIZE, MAX_PAYLOAD
from d200_deck.errors import InvalidColorError, ProtocolError


class Msg(IntEnum):
    HELLO = 0x01
    SET_BRIGHTNESS = 0x02
    CLEAR = 0x03
    FILL_KEY = 0x04
    SET_KEY_IMAGE = 0x05
    PING = 0x06
    SYSINFO = 0x07
    OK = 0x80
    INFO = 0x81
    SYSINFO_REPLY = 0x82
    KEY_EVENT = 0x90
    ERROR = 0xFF


class PixelFormat(IntEnum):
    RGB565 = 0
    RGB888 = 1

    @property
    def bytes_per_pixel(self) -> int:
        return 2 if self is PixelFormat.RGB565 else 3


_HEX_COLOR = re.compile(r"#?([0-9a-fA-F]{6})")


@dataclass(frozen=True)
class Color:
    r: int
    g: int
    b: int

    def __post_init__(self) -> None:
        if not all(0 <= v <= 255 for v in (self.r, self.g, self.b)):
            raise InvalidColorError(f"colour components must be 0-255: {self}")

    @classmethod
    def from_hex(cls, text: str) -> Color:
        m = _HEX_COLOR.fullmatch(text.strip())
        if m is None:
            raise InvalidColorError(f"expected RRGGBB, got {text!r}")
        v = int(m.group(1), 16)
        return cls(v >> 16, (v >> 8) & 0xFF, v & 0xFF)

    def to_bytes(self) -> bytes:
        return bytes((self.r, self.g, self.b))


@dataclass(frozen=True)
class KeyInfo:
    index: int
    code: int
    x: int
    y: int
    width: int
    height: int


@dataclass(frozen=True)
class DeviceInfo:
    version: int
    width: int
    height: int
    keys: tuple[KeyInfo, ...]

    def key(self, index: int) -> KeyInfo:
        for k in self.keys:
            if k.index == index:
                return k
        raise KeyError(f"unknown key {index}")


@dataclass(frozen=True)
class KeyEvent:
    index: int
    pressed: bool


def encode_frame(msg: Msg, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError(f"payload of {len(payload)} bytes exceeds {MAX_PAYLOAD}")
    return struct.pack("<BI", msg, len(payload)) + payload


def decode_header(head: bytes) -> tuple[int, int]:
    """Return (type, payload length) of a 5-byte frame header."""
    if len(head) != HEADER_SIZE:
        raise ProtocolError(f"header must be {HEADER_SIZE} bytes")
    msg_type, length = struct.unpack("<BI", head)
    if length > MAX_PAYLOAD:
        raise ProtocolError(f"frame of {length} bytes exceeds {MAX_PAYLOAD}")
    return msg_type, length


def parse_info(payload: bytes) -> DeviceInfo:
    if len(payload) < 7:
        raise ProtocolError(f"INFO too short: {len(payload)} bytes")
    version, width, height, count = struct.unpack_from("<HHHB", payload)
    if len(payload) != 7 + 12 * count:
        raise ProtocolError(f"INFO with {count} keys must be {7 + 12 * count} bytes, got {len(payload)}")
    keys = tuple(KeyInfo(*struct.unpack_from("<6H", payload, 7 + 12 * i)) for i in range(count))
    return DeviceInfo(version, width, height, keys)


def parse_key_event(payload: bytes) -> KeyEvent:
    if len(payload) != 3:
        raise ProtocolError(f"KEY_EVENT must be 3 bytes, got {len(payload)}")
    index, pressed = struct.unpack("<HB", payload)
    return KeyEvent(index, pressed != 0)


def parse_sysinfo(payload: bytes) -> dict[str, str]:
    """SYSINFO_REPLY: UTF-8 `key=value` lines; order is preserved."""
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as e:
        raise ProtocolError(f"SYSINFO is not UTF-8: {e}") from e
    info: dict[str, str] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if not sep or not key:
            raise ProtocolError(f"malformed SYSINFO line {line!r}")
        info[key] = value
    return info


def fill_key_payload(index: int, color: Color) -> bytes:
    return struct.pack("<H", index) + color.to_bytes()


def key_image_payload(index: int, width: int, height: int, pixels: bytes, fmt: PixelFormat) -> bytes:
    expected = width * height * fmt.bytes_per_pixel
    if len(pixels) != expected:
        raise ProtocolError(f"{width}x{height} {fmt.name} needs {expected} bytes, got {len(pixels)}")
    return struct.pack("<HBHH", index, fmt, width, height) + pixels


def rgb888_to_rgb565(data: bytes) -> bytes:
    """Convert packed RGB888 bytes to little-endian RGB565."""
    if len(data) % 3:
        raise ProtocolError(f"RGB888 data length {len(data)} is not a multiple of 3")
    out = bytearray(len(data) // 3 * 2)
    for i in range(len(data) // 3):
        r, g, b = data[3 * i], data[3 * i + 1], data[3 * i + 2]
        v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
        out[2 * i] = v & 0xFF
        out[2 * i + 1] = v >> 8
    return bytes(out)


def rgb565_to_rgb888(data: bytes) -> bytes:
    """Little-endian RGB565 to packed RGB888, expanding like deckd and the web UI."""
    if len(data) % 2:
        raise ProtocolError(f"RGB565 data length {len(data)} is odd")
    out = bytearray(len(data) // 2 * 3)
    for i in range(len(data) // 2):
        v = data[2 * i] | data[2 * i + 1] << 8
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        out[3 * i] = r << 3 | r >> 2
        out[3 * i + 1] = g << 2 | g >> 4
        out[3 * i + 2] = b << 3 | b >> 2
    return bytes(out)


def rgb565(colors: Iterable[Color]) -> bytes:
    """Pack colours as little-endian RGB565."""
    return rgb888_to_rgb565(b"".join(c.to_bytes() for c in colors))

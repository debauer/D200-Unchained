"""Key windows of the D200, as measured on the device (mirrors device/src/layout.rs).

Used before deckd has reported its own layout, so the host can be configured without
the panel connected."""

from __future__ import annotations

from d200_deck.constants import PROTOCOL_VERSION
from d200_deck.device.protocol import DeviceInfo, KeyInfo

WIDTH, HEIGHT = 1280, 720
X0, Y0, PITCH_X, PITCH_Y, SIZE = 11, 3, 260, 259, 196
# Keycodes by (row, column); 0 = no key. Key 30 spans columns 3 and 4.
CODES = ((29, 15, 14, 13, 12), (11, 10, 9, 8, 7), (34, 33, 31, 30, 0))
WIDE_KEY = 30


def d200_layout() -> DeviceInfo:
    keys: list[KeyInfo] = []
    for row, codes in enumerate(CODES):
        for col, code in enumerate(codes):
            if code:
                width = PITCH_X + SIZE if code == WIDE_KEY else SIZE
                keys.append(KeyInfo(len(keys), code, X0 + col * PITCH_X, Y0 + row * PITCH_Y, width, SIZE))
    return DeviceInfo(PROTOCOL_VERSION, WIDTH, HEIGHT, tuple(keys))

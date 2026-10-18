"""Generated test images (no image library needed)."""

from __future__ import annotations

from d200_deck.device.protocol import Color, KeyInfo, rgb565

# One base colour per key position, cycled.
PALETTE = (
    Color(230, 60, 60),
    Color(240, 150, 40),
    Color(230, 210, 50),
    Color(80, 200, 90),
    Color(50, 170, 220),
    Color(90, 100, 230),
    Color(180, 80, 220),
)


def gradient(key: KeyInfo, base: Color) -> bytes:
    """Vertical gradient from `base` (top) to black (bottom) as RGB565."""
    rows = []
    for y in range(key.height):
        f = 1.0 - 0.8 * y / max(key.height - 1, 1)
        row = Color(int(base.r * f), int(base.g * f), int(base.b * f))
        rows.append(rgb565([row]) * key.width)
    return b"".join(rows)

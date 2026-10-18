from __future__ import annotations

from datetime import datetime

from PIL import Image

from d200_deck.device.protocol import Color, rgb565_to_rgb888, rgb888_to_rgb565
from d200_deck.render.textrender import font, layout, render_clock, render_key

BG = Color(10, 13, 18)
WHITE = Color(255, 255, 255)


def image(raw: bytes, w: int = 196, h: int = 196) -> Image.Image:
    return Image.frombytes("RGB", (w, h), rgb565_to_rgb888(raw))


def ink_rows(img: Image.Image, bg: tuple[int, int, int]) -> list[int]:
    """Rows that contain anything but the background colour."""
    px = img.load()
    assert px is not None
    return [y for y in range(img.height) if any(px[x, y] != bg for x in range(img.width))]


def test_rgb565_roundtrip_matches_the_device_expansion() -> None:
    assert rgb565_to_rgb888(bytes([0x00, 0xF8, 0xE0, 0x07, 0xFF, 0xFF])) == bytes([255, 0, 0, 0, 255, 0, 255, 255, 255])
    data = bytes([10, 200, 30, 0, 0, 0])
    assert rgb888_to_rgb565(rgb565_to_rgb888(rgb888_to_rgb565(data))) == rgb888_to_rgb565(data)


def test_text_position_follows_the_setting() -> None:
    bg = (8, 12, 16)  # BG after the RGB565 round trip
    bottom = ink_rows(
        image(render_key(196, 196, color=BG, icon=None, text="CPU 42%", text_color=WHITE, position="bottom")), bg
    )
    top = ink_rows(
        image(render_key(196, 196, color=BG, icon=None, text="CPU 42%", text_color=WHITE, position="top")), bg
    )
    assert min(bottom) > 140
    assert max(top) < 60
    assert (
        ink_rows(image(render_key(196, 196, color=BG, icon=None, text="   ", text_color=WHITE, position="top")), bg)
        == []
    )


def test_long_text_wraps_to_three_lines_then_shrinks_and_cuts() -> None:
    fnt, lines = layout("A label that is much too long for one single key", 196 - 22, 196)
    assert 1 < len(lines) <= 3
    assert fnt.size <= round(196 * 0.12)
    fnt, lines = layout(" ".join(["word"] * 40), 196 - 22, 196)
    assert len(lines) == 3
    assert lines[-1].endswith("...")
    assert all(fnt.getlength(line) <= 196 - 22 for line in lines)


def test_text_over_an_icon_keeps_the_icon() -> None:
    icon = rgb888_to_rgb565(bytes([255, 0, 0]) * (196 * 196))
    img = image(render_key(196, 196, color=BG, icon=icon, text="OBS", text_color=WHITE, position="center"))
    assert img.getpixel((5, 5)) == (255, 0, 0)  # corner still shows the icon
    assert img.getpixel((98, 98)) != (255, 0, 0)  # text (or its shadow) in the middle


def test_clock_shows_time_and_date() -> None:
    img = image(render_clock(456, 196, BG, datetime(2026, 10, 17, 14, 41)), 456, 196)
    colours = {c for _, c in img.getcolors(maxcolors=456 * 196) or [] if isinstance(c, tuple)}  # RGB: tuples
    assert any(g > 200 and b > 200 and r < 60 for r, g, b in colours)  # cyan time
    assert any(r > 200 and b > 170 and g < 90 for r, g, b in colours)  # magenta date
    assert font(24).getname() == ("Chakra Petch", "Bold")


def test_clock_uses_given_colours() -> None:
    img = image(
        render_clock(
            456, 196, BG, datetime(2026, 10, 17, 14, 41), time_color=Color(255, 255, 0), date_color=Color(0, 255, 0)
        ),
        456,
        196,
    )
    colours = {c for _, c in img.getcolors(maxcolors=456 * 196) or [] if isinstance(c, tuple)}
    assert any(r > 200 and g > 200 and b < 60 for r, g, b in colours)  # yellow time
    assert any(g > 200 and r < 60 and b < 60 for r, g, b in colours)  # green date


def test_text_size_scales_but_long_text_still_fits() -> None:
    small, _ = layout("OK", 174, 196, 0.5)
    big, _ = layout("OK", 174, 196, 2.0)
    assert big.size == round(196 * 0.12 * 2.0) and small.size == round(196 * 0.12 * 0.5)
    fnt, lines = layout("A label that is much too long for one single key", 174, 196, 2.5)
    assert len(lines) <= 3
    assert all(fnt.getlength(line) <= 174 for line in lines)
    bg = (8, 12, 16)
    rows_100 = ink_rows(
        image(render_key(196, 196, color=BG, icon=None, text="OK", text_color=WHITE, position="center")), bg
    )
    rows_200 = ink_rows(
        image(render_key(196, 196, color=BG, icon=None, text="OK", text_color=WHITE, position="center", size=200)), bg
    )
    assert len(rows_200) > 1.5 * len(rows_100)

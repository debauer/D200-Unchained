from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import pytest
from PIL import Image
from typer.testing import CliRunner

from d200_deck.cli.main import app
from d200_deck.device.protocol import Color, KeyInfo, Msg, rgb888_to_rgb565
from d200_deck.errors import ImageLoadError
from d200_deck.render.images import Fit, fit_label, load_font, load_key_image, render

from .conftest import FakeDeck

KEY = KeyInfo(0, 29, 11, 3, 196, 196)
RED, BLUE = Color(255, 0, 0), Color(0, 0, 255)


def test_rgb888_to_rgb565() -> None:
    assert rgb888_to_rgb565(bytes([255, 0, 0, 0, 255, 0])) == bytes([0x00, 0xF8, 0xE0, 0x07])


def test_contain_letterboxes_wide_image() -> None:
    out = render(Image.new("RGB", (100, 50), (255, 0, 0)), 196, 196, Fit.CONTAIN, BLUE)
    assert out.size == (196, 196)
    assert out.getpixel((98, 98)) == (255, 0, 0)
    assert out.getpixel((98, 5)) == (0, 0, 255)


def test_cover_and_stretch_fill_window() -> None:
    wide = Image.new("RGB", (100, 50), (255, 0, 0))
    for fit in (Fit.COVER, Fit.STRETCH):
        out = render(wide, 196, 196, fit, BLUE)
        assert out.getpixel((98, 2)) == (255, 0, 0)


def test_transparency_uses_background() -> None:
    out = render(Image.new("RGBA", (10, 10), (255, 0, 0, 0)), 20, 20, Fit.STRETCH, BLUE)
    assert out.getpixel((10, 10)) == (0, 0, 255)


def test_load_key_image(tmp_path: Path) -> None:
    path = tmp_path / "red.png"
    Image.new("RGB", (8, 8), (255, 0, 0)).save(path)
    data = load_key_image(path, KEY)
    assert len(data) == 196 * 196 * 2
    assert data[:2] == bytes([0x00, 0xF8])
    bad = tmp_path / "bad.png"
    bad.write_text("no image")
    with pytest.raises(ImageLoadError):
        load_key_image(bad, KEY)


def test_label_is_drawn_in_bottom_strip() -> None:
    out = render(Image.new("RGB", (196, 196), (255, 0, 0)), 196, 196, Fit.COVER, BLUE, label="Hi")
    strip_top = 196 - 44
    assert all(out.getpixel((x, y)) == (255, 0, 0) for x in range(196) for y in range(strip_top))
    strip = [out.getpixel((x, y)) for x in range(196) for y in range(strip_top, 196)]
    assert any(pixel != (0, 0, 255) for pixel in strip)


def test_empty_label_uses_the_whole_window() -> None:
    out = render(Image.new("RGB", (10, 10), (255, 0, 0)), 10, 10, Fit.COVER, BLUE, label="   ", label_height=200)
    assert out.getpixel((5, 9)) == (255, 0, 0)


def test_label_height_is_clamped_to_half_the_window() -> None:
    out = render(Image.new("RGB", (10, 10), (255, 0, 0)), 10, 10, Fit.COVER, BLUE, label="x", label_height=99)
    assert out.getpixel((5, 0)) == (255, 0, 0)


def test_long_label_shrinks_then_ellipsizes() -> None:
    font, text = fit_label("A label that is much too long for one key", 196, 44, None)
    assert text.endswith("...")
    assert font.getlength(text) <= 196 - 8
    short_font, short = fit_label("1234", 196, 44, None)
    assert short == "1234"
    assert short_font.getlength("1234") <= 196 - 8


def test_load_font_falls_back_to_pillow_default(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("d200_deck.render.images.FONT_DIRS", ())
    assert load_font(None, 20).getlength("abc") > 0


def test_explicit_missing_font_is_rejected(tmp_path: Path) -> None:
    with pytest.raises(ImageLoadError):
        load_font(tmp_path / "missing.ttf", 20)


def test_cli_image_sends_full_window(tmp_path: Path, fake_deck: Callable[..., FakeDeck]) -> None:
    path = tmp_path / "icon.png"
    Image.new("RGB", (40, 20), (0, 255, 0)).save(path)
    deck = fake_deck()
    result = CliRunner().invoke(app, ["--port", str(deck.port), "image", "0", str(path), "--fit", "cover"])
    assert result.exit_code == 0, result.output
    msg_type, payload = deck.received[1]
    assert msg_type == Msg.SET_KEY_IMAGE
    assert len(payload) == 7 + 196 * 196 * 2


def test_cli_image_with_label(tmp_path: Path, fake_deck: Callable[..., FakeDeck]) -> None:
    path = tmp_path / "icon.png"
    Image.new("RGB", (40, 20), (0, 255, 0)).save(path)
    deck = fake_deck()
    args = ["--port", str(deck.port), "image", "0", str(path), "--fit", "cover", "--label", "Home"]
    result = CliRunner().invoke(app, args)
    assert result.exit_code == 0, result.output
    msg_type, payload = deck.received[1]
    assert msg_type == Msg.SET_KEY_IMAGE
    assert len(payload) == 7 + 196 * 196 * 2

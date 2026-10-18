from __future__ import annotations

import os
from dataclasses import replace
from pathlib import Path
from typing import Any

from d200_deck.config import DeckConfig, KeyConfig
from d200_deck.device.protocol import Color, parse_info
from d200_deck.render.renderer import KeyRenderer

from .conftest import INFO_PAYLOAD

INFO = parse_info(INFO_PAYLOAD)  # key 0: 196x196, key 1: 456x196
KEY0 = 196 * 196 * 2


class RecordingClient:
    def __init__(self) -> None:
        self.calls: list[tuple[str, Any]] = []

    def set_brightness(self, value: int) -> None:
        self.calls.append(("brightness", value))

    def clear(self, color: Color) -> None:
        self.calls.append(("clear", color))

    def fill_key(self, index: int, color: Color) -> None:
        self.calls.append(("fill", index))

    def set_key_image(self, index: int, width: int, height: int, pixels: bytes) -> None:
        self.calls.append(("image", index))

    def take(self) -> list[tuple[str, Any]]:
        calls, self.calls = self.calls, []
        return calls


def render(r: KeyRenderer, c: RecordingClient, cfg: DeckConfig, level: int = 255) -> list[tuple[str, Any]]:
    r.render(c, INFO, cfg, level)  # type: ignore[arg-type]  # duck-typed client
    return c.take()


def test_first_render_is_full_then_nothing_to_do(tmp_path: Path) -> None:
    r, c = KeyRenderer(tmp_path), RecordingClient()
    cfg = DeckConfig()
    assert render(r, c, cfg) == [("brightness", 255), ("clear", Color(0, 0, 0)), ("fill", 0), ("fill", 1)]
    assert render(r, c, cfg) == []


def test_brightness_change_sends_only_brightness(tmp_path: Path) -> None:
    r, c = KeyRenderer(tmp_path), RecordingClient()
    render(r, c, DeckConfig())
    assert render(r, c, DeckConfig(), level=100) == [("brightness", 100)]


def test_only_the_changed_key_is_sent(tmp_path: Path) -> None:
    r, c = KeyRenderer(tmp_path), RecordingClient()
    cfg = DeckConfig()
    render(r, c, cfg)
    assert render(r, c, cfg.with_key(KeyConfig(1, color=Color(0, 0, 255)))) == [("fill", 1)]


def test_rewritten_image_with_the_same_name_is_resent(tmp_path: Path) -> None:
    r, c = KeyRenderer(tmp_path), RecordingClient()
    image = tmp_path / "key-0.rgb565"
    image.write_bytes(b"\x00" * KEY0)
    cfg = DeckConfig().with_key(KeyConfig(0, image="key-0.rgb565"))
    assert ("image", 0) in render(r, c, cfg)
    assert render(r, c, cfg) == []
    image.write_bytes(b"\xff" * KEY0)
    st = image.stat()
    os.utime(image, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))
    assert render(r, c, cfg) == [("image", 0)]


def test_background_change_and_new_connection_redraw_everything(tmp_path: Path) -> None:
    r, c = KeyRenderer(tmp_path), RecordingClient()
    cfg = DeckConfig()
    render(r, c, cfg)
    blue = Color(0, 0, 50)
    assert render(r, c, replace(cfg, background=blue))[0] == ("clear", blue)
    r.forget()
    assert len(render(r, c, replace(cfg, background=blue))) == 4  # brightness, clear, both keys

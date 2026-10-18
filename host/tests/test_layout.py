from __future__ import annotations

from d200_deck.device.layout import d200_layout


def test_default_layout_is_the_measured_d200_grid() -> None:
    info = d200_layout()
    assert (info.width, info.height, len(info.keys)) == (1280, 720, 14)
    assert info.key(0).x == 11 and info.key(0).y == 3 and info.key(0).code == 29
    wide = info.key(13)
    assert (wide.code, wide.x, wide.y, wide.width, wide.height) == (30, 791, 521, 456, 196)
    assert all(k.width == 196 for k in info.keys if k.index != 13)

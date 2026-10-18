from __future__ import annotations

import pytest

from d200_deck.service.brightness import Brightness


def test_steps_are_clamped() -> None:
    b = Brightness(230)
    assert b.apply("bl_up").level == 255
    assert b.apply("bl_up").level == 255
    b.set_level(40)
    assert b.apply("bl_down").level == 0
    assert b.apply("bl_down").percent == 0


def test_display_off_keeps_the_level() -> None:
    b = Brightness(204)
    off = b.apply("disp_off")
    assert (off.effective, off.level, off.describe()) == (0, 204, "display off")
    assert b.apply("bl_up").effective == 0  # still off, level remembered
    on = b.apply("disp_on")
    assert (on.effective, on.percent, on.describe()) == (255, 100, "backlight 100%")
    assert on.to_json() == {"level": 255, "percent": 100, "display_on": True}


def test_rejects_other_actions() -> None:
    with pytest.raises(ValueError, match="not a device action"):
        Brightness(10).apply("command")

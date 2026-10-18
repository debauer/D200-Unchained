"""Runtime backlight state behind the backlight and display key actions."""

from __future__ import annotations

import threading
from dataclasses import dataclass
from typing import Any

from d200_deck.config import ACTION_BL_DOWN, ACTION_BL_UP, ACTION_DISP_OFF, ACTION_DISP_ON

MAX_LEVEL = 255
STEP = 51  # 20 % of the range, as in the UI design


@dataclass(frozen=True)
class Snapshot:
    level: int  # backlight level while the display is on (0-255)
    display_on: bool

    @property
    def effective(self) -> int:
        """Level to send to the device."""
        return self.level if self.display_on else 0

    @property
    def percent(self) -> int:
        return round(self.level * 100 / MAX_LEVEL)

    def to_json(self) -> dict[str, Any]:
        return {"level": self.level, "percent": self.percent, "display_on": self.display_on}

    def describe(self) -> str:
        return f"backlight {self.percent}%" if self.display_on else "display off"


class Brightness:
    """Thread-safe: key presses come from the event loop and from web requests."""

    def __init__(self, level: int) -> None:
        self._lock = threading.Lock()
        self._state = Snapshot(level, True)

    def snapshot(self) -> Snapshot:
        with self._lock:
            return self._state

    def set_level(self, level: int) -> Snapshot:
        with self._lock:
            self._state = Snapshot(max(0, min(MAX_LEVEL, level)), self._state.display_on)
            return self._state

    def apply(self, action_type: str) -> Snapshot:
        """Apply one of the device actions and return the new state."""
        with self._lock:
            s = self._state
            if action_type == ACTION_BL_UP:
                s = Snapshot(min(MAX_LEVEL, s.level + STEP), s.display_on)
            elif action_type == ACTION_BL_DOWN:
                s = Snapshot(max(0, s.level - STEP), s.display_on)
            elif action_type == ACTION_DISP_OFF:
                s = Snapshot(s.level, False)
            elif action_type == ACTION_DISP_ON:
                s = Snapshot(s.level, True)
            else:
                raise ValueError(f"not a device action: {action_type}")
            self._state = s
            return s

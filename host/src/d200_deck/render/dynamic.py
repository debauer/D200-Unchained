"""Keys whose image changes over time: text with placeholders, the info-window clock."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from d200_deck.config import DeckConfig, KeyConfig, Variable
from d200_deck.config.keystore import KeyStore
from d200_deck.config.placeholders import BUILTINS, fill, names
from d200_deck.device.protocol import DeviceInfo, KeyInfo
from d200_deck.render.textrender import CYAN, MAGENTA, date_label, render_clock, render_key


def builtin_values(now: datetime) -> dict[str, str]:
    return {"time": f"{now:%H:%M}", "date": date_label(now)}


def is_info_window(window: KeyInfo) -> bool:
    return window.width > window.height * 1.5


def shows_clock(key: KeyConfig, window: KeyInfo) -> bool:
    """The design shows the clock in the info window while it has no image and no text."""
    return is_info_window(window) and not key.image and not key.label.strip()


class DynamicKeys:
    def __init__(self, store: KeyStore) -> None:
        self.store = store

    def variables(self, cfg: DeckConfig, page: str) -> list[Variable]:
        """Variables used by the keys of `page` (what needs polling)."""
        used = set().union(*(names(k.label) for k in cfg.page(page).keys))
        return [v for v in cfg.variables if v.name in used]

    def pixels(
        self, cfg: DeckConfig, info: DeviceInfo, page: str, values: dict[str, str], now: datetime
    ) -> dict[int, bytes]:
        """RGB565 images of the dynamic keys of `page`, by key index."""
        filled = {**values, **builtin_values(now)}
        known = {v.name for v in cfg.variables} | set(BUILTINS)
        out: dict[int, bytes] = {}
        for window in info.keys:
            key = cfg.key(window.index, page)
            if shows_clock(key, window):
                out[window.index] = self._clock(cfg, window, page, now)
            elif names(key.label):
                out[window.index] = render_key(
                    window.width,
                    window.height,
                    color=key.color,
                    icon=self._icon(page, window),
                    text=fill(key.label, filled, known),
                    text_color=key.label_color,
                    position=key.label_pos,
                    size=key.label_size,
                )
        return out

    def _clock(self, cfg: DeckConfig, window: KeyInfo, page: str, now: datetime) -> bytes:
        """The clock in the page's colours: background, text (time), secondary text (date)."""
        key, p = cfg.key(window.index, page), cfg.page(page)
        time_color = cfg.stored_key(window.index, page).label_color or p.label_color or CYAN
        return render_clock(
            window.width, window.height, key.color, now, time_color=time_color, date_color=p.accent_color or MAGENTA
        )

    def _icon(self, page: str, window: KeyInfo) -> bytes | None:
        path: Path = self.store.icon_path(page, window.index)
        try:
            data = path.read_bytes()
        except FileNotFoundError:
            return None
        return data if len(data) == window.width * window.height * 2 else None

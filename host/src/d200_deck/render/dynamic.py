"""Keys whose image changes over time: text with placeholders, the info-window clock."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from d200_deck.config import ColorRule, DeckConfig, KeyConfig, Variable
from d200_deck.config.keystore import KeyStore
from d200_deck.config.placeholders import BUILTINS, fill, names
from d200_deck.device.protocol import Color, DeviceInfo, KeyInfo
from d200_deck.render.textrender import CYAN, MAGENTA, date_label, render_clock, render_key


def builtin_values(now: datetime) -> dict[str, str]:
    return {"time": f"{now:%H:%M}", "date": date_label(now)}


def is_info_window(window: KeyInfo) -> bool:
    return window.width > window.height * 1.5


def shows_clock(key: KeyConfig, window: KeyInfo) -> bool:
    """The design shows the clock in the info window while it has no image and no text."""
    return is_info_window(window) and not key.image and not key.label.strip()


def matched_rule(key: KeyConfig, values: dict[str, str]) -> ColorRule | None:
    """The first colour rule whose variable currently has its value."""
    for rule in key.rules:
        value = values.get(rule.variable)
        if value is not None and value.strip() == rule.value.strip():
            return rule
    return None


class DynamicKeys:
    def __init__(self, store: KeyStore) -> None:
        self.store = store

    def variables(self, cfg: DeckConfig, page: str) -> list[Variable]:
        """Variables used by the keys of `page` (what needs polling)."""
        keys = cfg.page(page).keys
        used = set().union(*(names(k.label) for k in keys), *({r.variable for r in k.rules} for k in keys))
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
            rule = matched_rule(key, filled)
            color = (rule.color if rule else None) or key.color
            text_color = (rule.label_color if rule else None) or key.label_color
            icon = self._icon(page, window)
            alt = self._icon(page, window, alt=True) if rule is not None else None
            if shows_clock(key, window):
                out[window.index] = self._clock(cfg, window, page, now, color)
            # A matching rule recolours the key or swaps in its second icon; a picture without a stored icon
            # cannot be recoloured.
            elif names(key.label) or (rule is not None and (alt is not None or icon is not None or not key.image)):
                out[window.index] = render_key(
                    window.width,
                    window.height,
                    color=color,
                    icon=alt if alt is not None else icon,
                    text=fill(key.label, filled, known),
                    text_color=text_color,
                    position=key.label_pos,
                    size=key.label_size,
                )
        return out

    def _clock(self, cfg: DeckConfig, window: KeyInfo, page: str, now: datetime, color: Color) -> bytes:
        """The clock in the page's colours: background, text (time), secondary text (date)."""
        p = cfg.page(page)
        time_color = cfg.stored_key(window.index, page).label_color or p.label_color or CYAN
        return render_clock(
            window.width, window.height, color, now, time_color=time_color, date_color=p.accent_color or MAGENTA
        )

    def _icon(self, page: str, window: KeyInfo, alt: bool = False) -> bytes | None:
        path: Path = (self.store.alt_icon_path if alt else self.store.icon_path)(page, window.index)
        try:
            data = path.read_bytes()
        except FileNotFoundError:
            return None
        return data if len(data) == window.width * window.height * 2 else None

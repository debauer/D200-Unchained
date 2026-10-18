"""Send key content to deckd, but only what changed since the last render.

A full redraw clears the screen and resends every key; on the panel that shows
as flicker. After the first render only keys whose content differs are sent,
and a brightness change alone sends nothing but SET_BRIGHTNESS."""

from __future__ import annotations

import hashlib
import logging
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path

from d200_deck.config import ROOT, DeckConfig
from d200_deck.device.client import DeckClient
from d200_deck.device.protocol import Color, DeviceInfo, KeyInfo

log = logging.getLogger(__name__)

# What a key shows: ("image", name, mtime_ns, size), ("fill", colour) or ("dynamic", digest).
Signature = tuple[object, ...]


@dataclass
class KeyRenderer:
    images_dir: Path
    _shown: dict[int, Signature] = field(default_factory=dict)
    _background: Color | None = None
    _level: int | None = None

    def forget(self) -> None:
        """The device content is unknown (new connection): next render is a full one."""
        self._shown.clear()
        self._background = None
        self._level = None

    def render(  # noqa: PLR0913
        self,
        client: DeckClient,
        info: DeviceInfo,
        cfg: DeckConfig,
        level: int,
        *,
        page: str = ROOT,
        dynamic: Mapping[int, bytes] | None = None,
    ) -> None:
        """`dynamic`: images rendered by the host for keys with changing content."""
        if level != self._level:
            client.set_brightness(level)
            self._level = level
        if cfg.background != self._background:
            client.clear(cfg.background)
            self._background = cfg.background
            self._shown.clear()  # everything was painted over
        for window in info.keys:
            pixels = (dynamic or {}).get(window.index)
            if pixels is not None:
                self._render_dynamic(client, window, pixels)
            else:
                self._render_key(client, window, cfg, page)

    def _render_dynamic(self, client: DeckClient, window: KeyInfo, pixels: bytes) -> None:
        signature: Signature = ("dynamic", hashlib.blake2b(pixels, digest_size=16).digest())
        if self._shown.get(window.index) != signature:
            client.set_key_image(window.index, window.width, window.height, pixels)
            self._shown[window.index] = signature

    def _render_key(self, client: DeckClient, window: KeyInfo, cfg: DeckConfig, page: str) -> None:
        key = cfg.key(window.index, page)
        image = self._image(key.image, window) if key.image else None
        signature: Signature = image[0] if image else ("fill", key.color)
        if self._shown.get(window.index) == signature:
            return
        if image:
            client.set_key_image(window.index, window.width, window.height, image[1])
        else:
            client.fill_key(window.index, key.color)
        self._shown[window.index] = signature

    def _image(self, name: str, window: KeyInfo) -> tuple[Signature, bytes] | None:
        path = self.images_dir / name
        expected = window.width * window.height * 2
        try:
            st = path.stat()
            signature: Signature = ("image", name, st.st_mtime_ns, st.st_size)
            if signature == self._shown.get(window.index):
                return signature, b""  # unchanged: no need to read the file
            data = path.read_bytes()
        except OSError as e:
            log.error("key image %s: %s", path, e)
            return None
        if len(data) != expected:
            log.error("key image %s has %d bytes, the key window needs %d", path, len(data), expected)
            return None
        return signature, data

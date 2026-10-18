"""Key image and icon files behind the web API (mixed into HostService)."""

from __future__ import annotations

import threading
from dataclasses import replace
from pathlib import Path

from d200_deck.config import ROOT, DeckConfig, KeyConfig
from d200_deck.config.keystore import KeyStore
from d200_deck.device.protocol import DeviceInfo, KeyInfo
from d200_deck.errors import ImageSizeError, UnknownKeyError


class KeyFiles:
    """Needs `config`, `info`, `store`, `_lock` and `_save()` from the service."""

    config: DeckConfig
    info: DeviceInfo
    store: KeyStore
    _lock: threading.Lock

    def _save(self, cfg: DeckConfig) -> None:
        raise NotImplementedError("provided by HostService")

    def window(self, index: int) -> KeyInfo:
        try:
            return self.info.key(index)
        except KeyError as e:
            raise UnknownKeyError(f"unknown key {index}") from e

    def image_path(self, index: int, page: str = ROOT) -> Path | None:
        name = self.config.key(index, page).image
        return self.store.path(name) if name else None

    def _check_size(self, index: int, pixels: bytes) -> None:
        w = self.window(index)
        expected = w.width * w.height * 2
        if len(pixels) != expected:
            raise ImageSizeError(f"key {index} needs {expected} bytes ({w.width}x{w.height} RGB565), got {len(pixels)}")

    def icon_path(self, index: int, page: str = ROOT) -> Path:
        """Unlabelled icon kept for the web UI (never shown directly)."""
        self.config.page(page)
        return self.store.icon_path(page, index)

    def store_icon(self, index: int, pixels: bytes, page: str = ROOT) -> None:
        self._check_size(index, pixels)
        self.store.write(self.icon_path(index, page), pixels)

    def delete_icon(self, index: int, page: str = ROOT) -> None:
        self.window(index)
        self.icon_path(index, page).unlink(missing_ok=True)

    def store_image(self, index: int, pixels: bytes, page: str = ROOT) -> KeyConfig:
        """Store raw RGB565 for key `index` of `page` and show it; returns the new key entry."""
        self._check_size(index, pixels)
        self.config.page(page)
        name = self.store.image_name(page, index)
        self.store.write(self.store.path(name), pixels)
        with self._lock:
            key = replace(self.config.stored_key(index, page), image=name)
            self._save(self.config.with_key(key, page))
        return key

    def delete_image(self, index: int, page: str = ROOT) -> KeyConfig:
        self.window(index)
        with self._lock:
            path = self.image_path(index, page)
            key = replace(self.config.stored_key(index, page), image=None)
            self._save(self.config.with_key(key, page))
        if path is not None:
            path.unlink(missing_ok=True)
        return key

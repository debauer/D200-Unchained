"""XDG locations of the host configuration and key images."""

from __future__ import annotations

import os
from pathlib import Path

APP_DIR = "deck"


def config_dir() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / APP_DIR


def data_dir() -> Path:
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local/share")
    return Path(base) / APP_DIR


def config_file() -> Path:
    return config_dir() / "config.json"


def deckd_binary() -> Path:
    """The ARMv7 deckd installed by install.sh; pushed to the device when it is not running."""
    return data_dir() / "deckd"


def images_dir() -> Path:
    return data_dir() / "images"

"""Serialise the host configuration to JSON, load and save it."""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any

from d200_deck.config.model import (
    ACTION_COMMAND,
    ACTION_PAGE,
    CONFIG_VERSION,
    VARIABLE_COMMAND,
    DeckConfig,
    KeyConfig,
    Page,
    Variable,
)
from d200_deck.config.parse import parse
from d200_deck.device.protocol import Color
from d200_deck.errors import ConfigError


def _hex(c: Color) -> str:
    return f"{c.r:02x}{c.g:02x}{c.b:02x}"


def key_to_json(k: KeyConfig) -> dict[str, Any]:
    entry: dict[str, Any] = {
        "index": k.index,
        "label": k.label,
        "label_pos": k.label_pos,
        "label_size": k.label_size,
    }
    entry.update({n: _hex(c) for n, c in (("color", k.color), ("label_color", k.label_color)) if c is not None})
    if k.image is not None:
        entry["image"] = k.image
    if k.action is not None:
        action: dict[str, Any] = {**k.action.extra, "type": k.action.type}
        if k.action.type == ACTION_COMMAND:
            action["argv"] = list(k.action.argv)
        elif k.action.type == ACTION_PAGE:
            action["target"] = k.action.target
        entry["action"] = action
    return entry


def _variable_json(v: Variable) -> dict[str, Any]:
    entry: dict[str, Any] = {**v.extra, "name": v.name, "type": v.type, "interval": v.interval}
    if v.type == VARIABLE_COMMAND:
        entry["command"] = v.command
    return entry


def _page_json(p: Page) -> dict[str, Any]:
    colors = {
        n: _hex(c)
        for n, c in (("color", p.color), ("label_color", p.label_color), ("accent_color", p.accent_color))
        if c is not None
    }
    return {"id": p.id, "name": p.name, "parent": p.parent, **colors, "keys": [key_to_json(k) for k in p.keys]}


def to_json(cfg: DeckConfig) -> dict[str, Any]:
    return {
        "version": CONFIG_VERSION,
        "brightness": cfg.brightness,
        "background": _hex(cfg.background),
        "pages": [_page_json(p) for p in cfg.pages],
        "variables": [_variable_json(v) for v in cfg.variables],
    }


def load(path: Path) -> DeckConfig:
    """Read the config; a missing file yields the defaults (all keys in the background colour)."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return DeckConfig()
    except OSError as e:
        raise ConfigError(f"cannot read {path}: {e}") from e
    try:
        return parse(json.loads(text))
    except json.JSONDecodeError as e:
        raise ConfigError(f"{path}: invalid JSON: {e}") from e


def save(path: Path, cfg: DeckConfig) -> None:
    """Write atomically so a concurrently reloading service never sees half a file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".config-", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(to_json(cfg), f, indent=2)
        f.write("\n")
    os.replace(tmp, path)

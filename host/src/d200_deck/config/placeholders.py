"""`{name}` placeholders in key text: find them and fill in values."""

from __future__ import annotations

import re
from collections.abc import Mapping

BUILTINS = ("time", "date")
NAME = r"[a-z][a-z0-9_]{0,31}"
_TOKEN = re.compile(r"\{\{|\}\}|\{(" + NAME + r")\}")
PENDING = "..."  # shown until a variable has its first value


def names(text: str) -> set[str]:
    """Placeholder names used in `text` (`{{`/`}}` are literal braces)."""
    return {m.group(1) for m in _TOKEN.finditer(text) if m.group(1)}


def is_dynamic(text: str) -> bool:
    return bool(names(text))


def fill(text: str, values: Mapping[str, str], known: set[str]) -> str:
    """Replace placeholders. Known names without a value yet show PENDING; unknown
    names stay as written, so a typo is visible on the key."""

    def sub(m: re.Match[str]) -> str:
        token, name = m.group(0), m.group(1)
        if not name:
            return token[0]  # "{{" -> "{", "}}" -> "}"
        if name in values:
            return values[name]
        return PENDING if name in known else token

    return _TOKEN.sub(sub, text)

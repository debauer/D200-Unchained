"""Authentication settings: `auth.json`, apart from config.json (which is exported and served by the API)."""

from __future__ import annotations

import json
import os
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from d200_deck.errors import ConfigError
from d200_deck.web.auth.verifier import parse_verifier

DEFAULT_TTL_S = 172_800
MIN_TTL_S = 60
MAX_TTL_S = 30 * 24 * 3600
KEYS = {"enabled", "username", "password_hash", "session_ttl_seconds", "public_origin", "trusted_proxies"}
MAX_USERNAME = 64


@dataclass(frozen=True)
class AuthSettings:
    enabled: bool = False
    username: str = ""
    password_hash: str = ""
    session_ttl_seconds: int = DEFAULT_TTL_S
    public_origin: str = ""  # scheme://host[:port] as seen by the browser; empty: derived from the Host header
    trusted_proxies: tuple[str, ...] = ()  # peers whose X-Forwarded-For is believed

    @property
    def secure(self) -> bool:
        return self.public_origin.startswith("https://")

    @property
    def cookie_name(self) -> str:
        return "__Host-deck_session" if self.secure else "deck_session"

    @property
    def public_host(self) -> str:
        return urlsplit(self.public_origin).netloc if self.public_origin else ""


def _origin(value: Any) -> str:
    if not isinstance(value, str):
        raise ConfigError("auth: public_origin must be a string")
    if not value:
        return ""
    parts = urlsplit(value)
    if parts.scheme not in ("http", "https") or not parts.hostname or parts.path not in ("", "/") or parts.query:
        raise ConfigError("auth: public_origin must look like https://deck.example or http://host:7280")
    return f"{parts.scheme}://{parts.netloc.lower()}"


def parse_settings(data: Any) -> AuthSettings:
    if not isinstance(data, dict):
        raise ConfigError("auth settings must be a JSON object")
    unknown = set(data) - KEYS
    if unknown:
        raise ConfigError(f"auth: unknown keys {sorted(unknown)}")
    enabled = data.get("enabled", False)
    ttl = data.get("session_ttl_seconds", DEFAULT_TTL_S)
    username = data.get("username", "")
    verifier = data.get("password_hash", "")
    proxies = data.get("trusted_proxies", [])
    if not isinstance(enabled, bool):
        raise ConfigError("auth: enabled must be true or false")
    if not isinstance(ttl, int) or isinstance(ttl, bool) or not MIN_TTL_S <= ttl <= MAX_TTL_S:
        raise ConfigError(f"auth: session_ttl_seconds must be {MIN_TTL_S}-{MAX_TTL_S}")
    if not isinstance(username, str) or not isinstance(verifier, str):
        raise ConfigError("auth: username and password_hash must be strings")
    if not isinstance(proxies, list) or not all(isinstance(p, str) for p in proxies):
        raise ConfigError("auth: trusted_proxies must be a list of addresses")
    if enabled:
        if not 0 < len(username) <= MAX_USERNAME:
            raise ConfigError(f"auth: username must be 1-{MAX_USERNAME} characters when auth is enabled")
        parse_verifier(verifier)
    return AuthSettings(enabled, username, verifier, ttl, _origin(data.get("public_origin", "")), tuple(proxies))


def load_settings(path: Path) -> AuthSettings:
    """Read the settings; a missing file means authentication is off."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return AuthSettings()
    except OSError as e:
        raise ConfigError(f"cannot read {path}: {e}") from e
    if path.stat().st_mode & 0o077:
        raise ConfigError(f"{path} holds the password verifier: chmod 600 it")
    try:
        return parse_settings(json.loads(text))
    except json.JSONDecodeError as e:
        raise ConfigError(f"{path}: invalid JSON: {e}") from e


def save_settings(path: Path, settings: AuthSettings) -> None:
    """Write atomically with mode 0600."""
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {**asdict(settings), "trusted_proxies": list(settings.trusted_proxies)}
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".auth-", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)

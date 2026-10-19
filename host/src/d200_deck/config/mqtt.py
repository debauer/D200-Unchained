"""MQTT broker settings: a file of their own (mode 0600), because they hold a password and
`config.json` is exported and served by the web API."""

from __future__ import annotations

import json
import os
import tempfile
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from typing import Any

from d200_deck.errors import ConfigError

DEFAULT_PORT = 1883
MAX_PORT = 65535


@dataclass(frozen=True)
class MqttSettings:
    host: str = ""  # empty: MQTT is off
    port: int = DEFAULT_PORT
    username: str = ""
    password: str = ""
    tls: bool = False
    client_id: str = ""  # empty: chosen by the library

    @property
    def enabled(self) -> bool:
        return bool(self.host)

    def public_json(self) -> dict[str, Any]:
        """For the web API: everything but the password (only whether one is set)."""
        data = {k: v for k, v in asdict(self).items() if k != "password"}
        return {**data, "password_set": bool(self.password)}


def _text(data: dict[str, Any], key: str, default: str) -> str:
    value = data.get(key, default)
    if not isinstance(value, str):
        raise ConfigError(f"mqtt: {key} must be a string")
    return value.strip() if key != "password" else value


def parse_settings(data: Any, previous: MqttSettings | None = None) -> MqttSettings:
    """Validate settings. A missing `password` keeps the one of `previous`; `null` clears it."""
    if not isinstance(data, dict):
        raise ConfigError("mqtt settings must be a JSON object")
    old = previous or MqttSettings()
    port = data.get("port", old.port)
    if not isinstance(port, int) or isinstance(port, bool) or not 1 <= port <= MAX_PORT:
        raise ConfigError(f"mqtt: port must be 1-{MAX_PORT}")
    tls = data.get("tls", old.tls)
    if not isinstance(tls, bool):
        raise ConfigError("mqtt: tls must be true or false")
    password = "" if "password" in data and data["password"] is None else _text(data, "password", old.password)
    return replace(
        old,
        host=_text(data, "host", old.host),
        port=port,
        username=_text(data, "username", old.username),
        password=password,
        tls=tls,
        client_id=_text(data, "client_id", old.client_id),
    )


def load_settings(path: Path) -> MqttSettings:
    """Read the settings; a missing file means MQTT is off."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return MqttSettings()
    except OSError as e:
        raise ConfigError(f"cannot read {path}: {e}") from e
    try:
        return parse_settings(json.loads(text))
    except json.JSONDecodeError as e:
        raise ConfigError(f"{path}: invalid JSON: {e}") from e


def save_settings(path: Path, settings: MqttSettings) -> None:
    """Write atomically with mode 0600 (mkstemp creates the file that way)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".mqtt-", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(asdict(settings), f, indent=2)
        f.write("\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)

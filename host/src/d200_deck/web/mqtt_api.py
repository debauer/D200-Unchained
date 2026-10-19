"""`/api/mqtt`: broker settings. The password is write-only: it is never sent back."""

from __future__ import annotations

import json
from typing import Any

from d200_deck.config import parse_settings
from d200_deck.errors import ConfigError
from d200_deck.service.host import HostService


def mqtt_state(service: HostService) -> dict[str, Any]:
    return {**service.mqtt.settings.public_json(), **service.mqtt.status()}


def update_mqtt(service: HostService, body: bytes) -> dict[str, Any]:
    """Apply settings from a request body; raises ConfigError (also for invalid JSON)."""
    try:
        data = json.loads(body)
    except ValueError as e:
        raise ConfigError(f"invalid JSON: {e}") from e
    service.mqtt.update_settings(parse_settings(data, service.mqtt.settings))
    return mqtt_state(service)

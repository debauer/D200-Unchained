"""Execute key actions: commands, backlight/display control, unknown types."""

from __future__ import annotations

import logging
import shlex
from collections.abc import Callable

from d200_deck.config import ACTION_COMMAND, ACTION_MQTT, DEVICE_ACTIONS, NAV_ACTIONS, Action
from d200_deck.device.client import DeckClient
from d200_deck.service.actions import run_action
from d200_deck.service.brightness import Brightness
from d200_deck.service.hub import EventHub

log = logging.getLogger(__name__)

# (level, client): send the backlight level, via `client` or a connection of its own.
SetLevel = Callable[[int, DeckClient | None], None]
# Run a page/back action; returns the log line ('!' first if nothing happened).
Navigate = Callable[[Action], str]

# Publish an mqtt action; returns the log line ("!" first on failure).
Publish = Callable[[Action], str]

HOST_ACTIONS = (ACTION_COMMAND, ACTION_MQTT, *DEVICE_ACTIONS, *NAV_ACTIONS)


class ActionRunner:
    def __init__(
        self, hub: EventHub, brightness: Brightness, set_level: SetLevel, navigate: Navigate, publish: Publish
    ) -> None:
        self.hub = hub
        self.brightness = brightness
        self._set_level = set_level
        self._navigate = navigate
        self._publish = publish

    def run(self, index: int, action: Action, client: DeckClient | None, *, commands: bool = True) -> bool:
        """Run `action` of key `index`; False if this host cannot run its type.
        With `commands=False` (web test press) a command is only reported, never started."""
        if action.type == ACTION_COMMAND:
            line = f"$ {shlex.join(action.argv)}"
            if commands:
                run_action(action)
            self._report(index, action, line if commands else f"(test, not run) {line}")
            return True
        if action.type == ACTION_MQTT:
            line = f"mqtt {action.topic} {action.payload}"
            detail = self._publish(action) if commands else f"(test, not run) {line}"
            self._report(index, action, detail)
            return not detail.startswith("!")
        if action.type in NAV_ACTIONS:
            detail = self._navigate(action)
            self._report(index, action, detail)
            return not detail.startswith("!")
        if action.type in DEVICE_ACTIONS:
            state = self.brightness.apply(action.type)
            self._set_level(state.effective, client)
            self._report(index, action, state.describe())
            self.hub.publish("brightness", state.to_json())
            return True
        log.warning("key %d: action type %r is not supported on this host, ignored", index, action.type)
        self._report(index, action, f"! {action.type} not supported on this host")
        return False

    def _report(self, index: int, action: Action, detail: str) -> None:
        self.hub.publish("action", {"index": index, "type": action.type, "detail": detail})

"""MQTT: publish key messages and keep variables up to date from incoming messages.

Incoming messages only become text values (shown on keys, matched by colour rules); nothing
received is ever executed. The broker connection runs in paho's own thread; the transport
sits behind a small interface so tests need no broker."""

from __future__ import annotations

import json
import logging
import threading
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Any, Protocol

import paho.mqtt.client as paho
from paho.mqtt.enums import CallbackAPIVersion

from d200_deck.config import VARIABLE_MQTT, Action, MqttSettings, Variable, load_settings, save_settings

log = logging.getLogger(__name__)

MAX_LENGTH = 64
ERROR_VALUE = "!"  # message that does not have the configured JSON path
RECONNECT_MIN_S, RECONNECT_MAX_S = 1, 30


class Transport(Protocol):
    def start(self) -> None: ...
    def stop(self) -> None: ...
    def set_topics(self, topics: set[str]) -> None: ...
    def publish(self, topic: str, payload: str, qos: int, retain: bool) -> str:
        """Queue a message; returns an error text, empty on success."""
        ...


MessageHandler = Callable[[str, bytes], None]
StateHandler = Callable[[bool, str], None]  # connected, error text
TransportFactory = Callable[[MqttSettings, MessageHandler, StateHandler], Transport]


class PahoTransport:
    def __init__(self, settings: MqttSettings, on_message: MessageHandler, on_state: StateHandler) -> None:
        self._settings = settings
        self._on_message = on_message
        self._on_state = on_state
        self._topics: set[str] = set()
        self._lock = threading.Lock()
        self._connected = False
        client = paho.Client(CallbackAPIVersion.VERSION2, client_id=settings.client_id)
        if settings.username:
            client.username_pw_set(settings.username, settings.password)
        if settings.tls:
            client.tls_set()
        client.reconnect_delay_set(RECONNECT_MIN_S, RECONNECT_MAX_S)
        client.on_connect = self._connected_cb
        client.on_disconnect = self._disconnected_cb
        client.on_connect_fail = lambda _c, _u: self._on_state(False, "cannot reach the broker")
        client.on_message = lambda _c, _u, msg: self._on_message(msg.topic, bytes(msg.payload))
        self._client = client

    def start(self) -> None:
        self._client.connect_async(self._settings.host, self._settings.port)
        self._client.loop_start()

    def stop(self) -> None:
        self._client.disconnect()
        self._client.loop_stop()

    def set_topics(self, topics: set[str]) -> None:
        with self._lock:
            old, self._topics = self._topics, set(topics)
            connected = self._connected
        if connected:
            for topic in old - topics:
                self._client.unsubscribe(topic)
            for topic in topics - old:
                self._client.subscribe(topic)

    def publish(self, topic: str, payload: str, qos: int, retain: bool) -> str:
        info = self._client.publish(topic, payload, qos=qos, retain=retain)
        return "" if info.rc == paho.MQTT_ERR_SUCCESS else paho.error_string(info.rc)

    def _connected_cb(self, client: paho.Client, _userdata: Any, _flags: Any, reason: Any, _props: Any) -> None:
        if reason.is_failure:
            self._on_state(False, str(reason))
            return
        with self._lock:
            self._connected = True
            topics = set(self._topics)
        for topic in topics:  # (re)subscribe: also delivers retained messages again
            client.subscribe(topic)
        self._on_state(True, "")

    def _disconnected_cb(self, _client: paho.Client, _userdata: Any, _flags: Any, reason: Any, _props: Any) -> None:
        with self._lock:
            self._connected = False
        self._on_state(False, "" if str(reason) == "Normal disconnection" else str(reason))


def json_value(payload: str, path: str) -> str:
    """The scalar at the dotted `path` (`state.power`, `items.0.on`) of a JSON message; ERROR_VALUE if missing."""
    try:
        node: Any = json.loads(payload)
        for part in path.split("."):
            node = node[int(part)] if isinstance(node, list) else node[part]
    except (ValueError, KeyError, IndexError, TypeError):
        return ERROR_VALUE
    if isinstance(node, bool):
        return "true" if node else "false"
    if node is None:
        return ""
    if isinstance(node, dict | list):
        return ERROR_VALUE
    return str(node)


class MqttService:
    """Owns the broker settings, the connection, the latest message per mqtt variable and publishing."""

    def __init__(
        self,
        on_values: Callable[[], None],
        on_status: Callable[[dict[str, Any]], None],
        path: Path | None = None,
        factory: TransportFactory = PahoTransport,
    ) -> None:
        self._on_values = on_values
        self._on_status = on_status
        self._path = path
        self._factory = factory
        self._lock = threading.Lock()
        self._transport: Transport | None = None
        self._running = False
        self._connected = False
        self._error = ""
        self._variables: tuple[Variable, ...] = ()
        self._values: dict[str, str] = {}
        self.settings = load_settings(path) if path is not None else MqttSettings()

    # --- state ------------------------------------------------------------

    def values(self) -> dict[str, str]:
        with self._lock:
            return dict(self._values)

    def status(self) -> dict[str, Any]:
        with self._lock:
            return {"enabled": self.settings.enabled, "connected": self._connected, "error": self._error}

    # --- configuration ----------------------------------------------------

    def start(self) -> None:
        self._running = True
        self._reconnect()

    def close(self) -> None:
        self._running = False
        self._reconnect()

    def configure(self, variables: Iterable[Variable]) -> None:
        """Subscribe to the topics of all mqtt variables (of every page, so values are current after a page change)."""
        mine = tuple(v for v in variables if v.type == VARIABLE_MQTT and v.topic)
        with self._lock:
            self._variables = mine
            names = {v.name for v in mine}
            dropped = {n for n in self._values if n not in names}
            for name in dropped:
                del self._values[name]
            transport = self._transport
        if transport is not None:
            transport.set_topics({v.topic for v in mine})
        if dropped:
            self._on_values()

    def update_settings(self, settings: MqttSettings) -> None:
        """Apply and save new broker settings (reconnects)."""
        if self._path is not None:
            save_settings(self._path, settings)
        self.settings = settings
        self._reconnect()

    def _reconnect(self) -> None:
        with self._lock:
            old, self._transport = self._transport, None
            self._connected, self._error = False, ""
            self._values = {}
        if old is not None:
            old.stop()
        if self._running and self.settings.enabled:
            transport = self._factory(self.settings, self._message, self._state)
            with self._lock:
                self._transport = transport
            transport.set_topics({v.topic for v in self._variables})
            transport.start()
        self._on_status(self.status())
        self._on_values()

    # --- from the broker --------------------------------------------------

    def _state(self, connected: bool, error: str) -> None:
        with self._lock:
            changed = error != self._error
            self._connected, self._error = connected, error
        if error and changed:  # paho retries: report a problem once, not at every attempt
            log.warning("mqtt: %s", error)
        self._on_status(self.status())

    def _message(self, topic: str, raw: bytes) -> None:
        text = raw.decode("utf-8", errors="replace")
        changed = False
        with self._lock:
            targets = [v for v in self._variables if v.topic == topic]
            for v in targets:
                value = (json_value(text, v.json_path) if v.json_path else text.strip())[:MAX_LENGTH]
                if self._values.get(v.name) != value:
                    self._values[v.name] = value
                    changed = True
        if changed:
            self._on_values()

    # --- to the broker ----------------------------------------------------

    def publish(self, action: Action) -> str:
        """Publish the message of `action`; returns the log line ('!' first on failure)."""
        if not action.topic:
            return "! topic not set"
        with self._lock:
            transport = self._transport
        if transport is None:
            return "! mqtt is not connected" if self.settings.enabled else "! mqtt is not configured"
        error = transport.publish(action.topic, action.payload, action.qos, action.retain)
        if error:
            return f"! mqtt publish failed: {error}"
        return f"mqtt {action.topic} {action.payload}"

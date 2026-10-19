from __future__ import annotations

import json
import threading
from collections.abc import Callable
from datetime import datetime
from pathlib import Path
from typing import Any, ClassVar

import pytest

from d200_deck.config import (
    Action,
    ColorRule,
    DeckConfig,
    KeyConfig,
    MqttSettings,
    Page,
    Variable,
    load_settings,
    parse_settings,
    save_settings,
)
from d200_deck.config.keystore import KeyStore
from d200_deck.device.protocol import Color, Msg, parse_info, rgb888_to_rgb565
from d200_deck.errors import ConfigError
from d200_deck.render.dynamic import DynamicKeys, matched_rule
from d200_deck.service.host import HostService, ServiceSettings
from d200_deck.service.mqtt import ERROR_VALUE, MqttService, PahoTransport, json_value

from .conftest import INFO_PAYLOAD, FakeDeck
from .mini_broker import MiniBroker
from .test_service import start, stop, wait_until


class FakeTransport:
    """Stands in for the broker connection; tests call `message` / `state` like paho would."""

    instances: ClassVar[list[FakeTransport]] = []

    def __init__(self, settings: MqttSettings, on_message: Callable[[str, bytes], None], on_state: Any) -> None:
        self.settings, self.message, self.state = settings, on_message, on_state
        self.topics: set[str] = set()
        self.published: list[tuple[str, str, int, bool]] = []
        self.started = self.stopped = False
        self.error = ""
        FakeTransport.instances.append(self)

    def start(self) -> None:
        self.started = True

    def stop(self) -> None:
        self.stopped = True

    def set_topics(self, topics: set[str]) -> None:
        self.topics = set(topics)

    def publish(self, topic: str, payload: str, qos: int, retain: bool) -> str:
        self.published.append((topic, payload, qos, retain))
        return self.error


@pytest.fixture(autouse=True)
def _reset() -> None:
    FakeTransport.instances.clear()


BROKER = MqttSettings(host="broker", username="u", password="p")
LIGHT = Variable("light", "mqtt", topic="home/light")
STATE = Variable("power", "mqtt", topic="home/dev", json_path="state.power")


def make(path: Path | None = None) -> tuple[MqttService, list[str], list[dict[str, Any]]]:
    changes: list[str] = []
    statuses: list[dict[str, Any]] = []
    service = MqttService(lambda: changes.append("v"), statuses.append, path, FakeTransport)
    return service, changes, statuses


def test_settings_file_is_private_and_roundtrips(tmp_path: Path) -> None:
    path = tmp_path / "sub" / "mqtt.json"
    assert load_settings(path) == MqttSettings()  # missing file: MQTT off
    save_settings(path, BROKER)
    assert oct(path.stat().st_mode & 0o777) == "0o600"
    assert load_settings(path) == BROKER
    assert "password" not in BROKER.public_json() and BROKER.public_json()["password_set"] is True


def test_parse_settings_keeps_or_clears_the_password() -> None:
    assert parse_settings({"port": 8883}, BROKER).password == "p"
    assert parse_settings({"password": None}, BROKER).password == ""
    assert parse_settings({"password": "new"}, BROKER).password == "new"
    with pytest.raises(ConfigError, match="port"):
        parse_settings({"port": 70000})
    with pytest.raises(ConfigError, match="host must be a string"):
        parse_settings({"host": 1})


def test_connects_only_when_started_and_configured() -> None:
    service, _, statuses = make()
    service.configure([LIGHT])
    service.update_settings(BROKER)
    assert FakeTransport.instances == []  # not started yet
    service.start()
    transport = FakeTransport.instances[-1]
    assert transport.started and transport.topics == {"home/light"}
    transport.state(True, "")
    assert statuses[-1] == {"enabled": True, "connected": True, "error": ""}
    service.update_settings(MqttSettings())  # switched off: connection closed
    assert transport.stopped and len(FakeTransport.instances) == 1


def test_incoming_messages_become_values() -> None:
    service, changes, _ = make()
    service.update_settings(BROKER)
    service.configure([LIGHT, STATE, Variable("cpu", command="x")])
    service.start()
    transport = FakeTransport.instances[-1]
    transport.message("home/light", b"  ON \n")
    transport.message("home/dev", json.dumps({"state": {"power": True}}).encode())
    transport.message("other/topic", b"ignored")
    assert service.values() == {"light": "ON", "power": "true"}
    n = len(changes)
    transport.message("home/light", b"ON")  # same value: no redraw
    assert len(changes) == n
    transport.message("home/dev", b'{"state": {}}')
    assert service.values()["power"] == ERROR_VALUE
    transport.message("home/light", b"x" * 100)
    assert service.values()["light"] == "x" * 64


def test_configure_resubscribes_and_drops_values_of_removed_variables() -> None:
    service, _, _ = make()
    service.update_settings(BROKER)
    service.configure([LIGHT])
    service.start()
    transport = FakeTransport.instances[-1]
    transport.message("home/light", b"ON")
    service.configure([STATE])
    assert transport.topics == {"home/dev"} and service.values() == {}


@pytest.mark.parametrize(
    ("payload", "path", "expected"),
    [
        ('{"a": {"b": 1.5}}', "a.b", "1.5"),
        ('{"a": [{"on": false}]}', "a.0.on", "false"),
        ('{"a": null}', "a", ""),
        ('{"a": {"b": 1}}', "a", ERROR_VALUE),
        ('{"a": 1}', "x", ERROR_VALUE),
        ("not json", "a", ERROR_VALUE),
    ],
)
def test_json_value(payload: str, path: str, expected: str) -> None:
    assert json_value(payload, path) == expected


def test_publish_reports_failures() -> None:
    service, _, _ = make()
    action = Action("mqtt", topic="a/b", payload="x", retain=True, qos=1)
    assert service.publish(action) == "! mqtt is not configured"
    service.update_settings(BROKER)
    assert service.publish(action) == "! mqtt is not connected"  # not started
    service.start()
    transport = FakeTransport.instances[-1]
    assert service.publish(action) == "mqtt a/b x"
    assert transport.published == [("a/b", "x", 1, True)]
    assert service.publish(Action("mqtt")) == "! topic not set"
    transport.error = "no connection"
    assert service.publish(action) == "! mqtt publish failed: no connection"


def test_rules_recolour_keys_and_pull_in_their_variables(tmp_path: Path) -> None:
    rule_on = ColorRule("light", "ON", Color(255, 204, 0), Color(0, 0, 0))
    key = KeyConfig(0, label="Lamp", rules=(rule_on, ColorRule("light", "OFF", Color(0, 0, 80))))
    cfg = DeckConfig(pages=(Page("root", keys=(key,)),), variables=(LIGHT,))
    assert matched_rule(key, {"light": " ON "}) == rule_on
    assert matched_rule(key, {"light": "dim"}) is None and matched_rule(key, {}) is None
    dyn = DynamicKeys(KeyStore(tmp_path))
    assert [v.name for v in dyn.variables(cfg, "root")] == ["light"]  # used only by the rule, not by the text
    info, now = parse_info(INFO_PAYLOAD), datetime(2026, 10, 19, 12, 0)
    assert 0 not in dyn.pixels(cfg, info, "root", {}, now)  # no value yet: the key stays static (key 1 is the clock)
    on = dyn.pixels(cfg, info, "root", {"light": "ON"}, now)[0]
    off = dyn.pixels(cfg, info, "root", {"light": "OFF"}, now)[0]
    assert on != off and len(on) == len(off) == 196 * 196 * 2


def test_a_matching_rule_shows_the_second_icon(tmp_path: Path) -> None:
    rule_on = ColorRule("light", "ON", None)
    key = KeyConfig(0, image="key-0.rgb565", rules=(rule_on,))  # a picture; the colour rule alone cannot recolour it
    cfg = DeckConfig(pages=(Page("root", keys=(key,)),), variables=(LIGHT,))
    store = KeyStore(tmp_path)
    dyn = DynamicKeys(store)
    info, now = parse_info(INFO_PAYLOAD), datetime(2026, 10, 19, 12, 0)
    assert 0 not in dyn.pixels(cfg, info, "root", {"light": "ON"}, now)  # no second icon: the stored image stays
    alt = rgb888_to_rgb565(bytes([255, 0, 0]) * (196 * 196))
    store.write(store.alt_icon_path("root", 0), alt)
    assert dyn.pixels(cfg, info, "root", {"light": "ON"}, now)[0] == alt
    assert 0 not in dyn.pixels(cfg, info, "root", {"light": "OFF"}, now)  # rule off: back to the stored image


def test_key_press_publishes_and_incoming_message_redraws(tmp_path: Path, fake_deck: Callable[..., FakeDeck]) -> None:
    cfg = tmp_path / "config.json"
    cfg.write_text(
        json.dumps(
            {
                "variables": [{"name": "light", "type": "mqtt", "topic": "home/light"}],
                "pages": [
                    {
                        "id": "root",
                        "keys": [
                            {
                                "index": 0,
                                "label": "{light}",
                                "action": {"type": "mqtt", "topic": "home/light/set", "payload": "toggle"},
                                "rules": [{"variable": "light", "value": "ON", "color": "00ff00"}],
                            }
                        ],
                    }
                ],
            }
        )
    )
    deck = fake_deck()
    settings = ServiceSettings("127.0.0.1", deck.port, cfg, tmp_path, poll_s=0.02, mqtt_path=tmp_path / "mqtt.json")
    save_settings(tmp_path / "mqtt.json", BROKER)
    service = HostService(settings, mqtt_factory=FakeTransport)
    worker: threading.Thread = start(service)
    try:
        deck.wait_for(lambda r: len(r) >= 5)  # hello, brightness, clear, two key images
        wait_until(lambda: bool(FakeTransport.instances) and FakeTransport.instances[-1].started)
        transport = FakeTransport.instances[-1]
        assert transport.topics == {"home/light"}
        deck.push(Msg.KEY_EVENT, bytes([0, 0, 1]))
        wait_until(lambda: transport.published == [("home/light/set", "toggle", 0, False)])
        before = len(deck.received)
        transport.message("home/light", b"ON")
        wait_until(lambda: len(deck.received) > before)  # the key was redrawn with the new text and colour
        assert service.values()["light"] == "ON"
    finally:
        stop(service, worker)
    assert FakeTransport.instances[-1].stopped


def test_paho_transport_against_a_real_broker() -> None:
    broker = MiniBroker()
    got: list[tuple[str, bytes]] = []
    states: list[tuple[bool, str]] = []
    broker.inject("home/light", b"ON", retain=True)
    settings = MqttSettings(host="127.0.0.1", port=broker.port, username="me", password="pw", client_id="deck-test")
    transport = PahoTransport(settings, lambda t, p: got.append((t, p)), lambda c, e: states.append((c, e)))
    transport.set_topics({"home/light"})  # chosen before connecting
    try:
        transport.start()
        wait_until(lambda: (True, "") in states and got == [("home/light", b"ON")])  # retained message on subscribe
        assert b"deck-test" in broker.connects[0] and b"me" in broker.connects[0] and b"pw" in broker.connects[0]
        broker.inject("home/light", b"OFF")
        wait_until(lambda: got[-1] == ("home/light", b"OFF"))
        transport.set_topics({"home/other"})  # a live change subscribes and unsubscribes
        wait_until(lambda: broker.subscribers("home/other") == 1)
        assert transport.publish("home/set", "toggle", 1, True) == ""
        wait_until(lambda: broker.received == [("home/set", "toggle", 1, True)])
    finally:
        transport.stop()
        broker.close()
    wait_until(lambda: states[-1] == (False, ""))  # a normal disconnect is not an error


def test_paho_transport_reports_an_unreachable_broker() -> None:
    broker = MiniBroker()
    port = broker.port
    broker.close()  # nothing listens on this port any more
    states: list[tuple[bool, str]] = []
    transport = PahoTransport(
        MqttSettings(host="127.0.0.1", port=port), lambda t, p: None, lambda c, e: states.append((c, e))
    )
    try:
        transport.start()
        wait_until(lambda: (False, "cannot reach the broker") in states)
    finally:
        transport.stop()

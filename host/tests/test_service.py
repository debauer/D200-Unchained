from __future__ import annotations

import json
import os
import sys
import threading
import time
from collections.abc import Callable, Iterator
from dataclasses import replace
from pathlib import Path

import pytest

from d200_deck.config import Action, KeyConfig
from d200_deck.device.client import DeckClient
from d200_deck.device.protocol import Msg, parse_info
from d200_deck.errors import ConfigError, DeviceNotFoundError
from d200_deck.service import actions
from d200_deck.service.host import HostService, ServiceSettings

from .conftest import INFO_PAYLOAD, FakeDeck

KEY0_IMAGE = 196 * 196 * 2


def write_config(path: Path, data: dict[str, object]) -> None:
    path.write_text(json.dumps(data))
    # Make sure the mtime changes even on coarse-grained filesystems.
    st = path.stat()
    os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))


def wait_until(predicate: Callable[[], bool], timeout: float = 2.0) -> None:
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, "condition not reached in time"
        time.sleep(0.01)


def types(received: list[tuple[int, bytes]]) -> list[int]:
    return [t for t, _ in received]


# Key 1 of the test layout is the wide info window: without text it would show the clock.
def rendered(received: list[tuple[int, bytes]]) -> bool:
    return Msg.FILL_KEY in types(received)


def start(service: HostService) -> threading.Thread:
    worker = threading.Thread(target=service.run)
    worker.start()
    return worker


def stop_slow(service: HostService, worker: threading.Thread) -> None:
    """Stop a service with a long poll interval (it notices `stop` after one poll)."""
    service.stop.set()
    worker.join(timeout=service.settings.poll_s + 2)
    assert not worker.is_alive()


def stop(service: HostService, worker: threading.Thread) -> None:
    service.stop.set()
    worker.join(timeout=2)
    assert not worker.is_alive()


@pytest.fixture
def setup(tmp_path: Path, fake_deck: Callable[..., FakeDeck]) -> Iterator[tuple[HostService, FakeDeck, Path]]:
    deck = fake_deck()
    images = tmp_path / "images"
    images.mkdir()
    (images / "k0.rgb565").write_bytes(b"\x00\xf8" * (KEY0_IMAGE // 2))
    cfg = tmp_path / "config.json"
    write_config(
        cfg,
        {
            "brightness": 100,
            "keys": [{"index": 0, "image": "k0.rgb565"}, {"index": 1, "label": "x", "color": "00ff00"}],
        },
    )
    service = HostService(ServiceSettings("127.0.0.1", deck.port, cfg, images, poll_s=0.02, reconnect_s=0.05))
    worker = start(service)
    try:  # stop the service even if setup fails, or its thread keeps pytest alive
        deck.wait_for(rendered)
        yield service, deck, cfg
    finally:
        stop(service, worker)


def test_renders_config_on_connect(setup: tuple[HostService, FakeDeck, Path]) -> None:
    _, deck, _ = setup
    assert types(deck.received)[:3] == [Msg.HELLO, Msg.SET_BRIGHTNESS, Msg.CLEAR]
    assert deck.received[1][1] == bytes([100])
    image = next(p for t, p in deck.received if t == Msg.SET_KEY_IMAGE)
    assert image[:2] == bytes([0, 0])
    assert len(image) == 7 + KEY0_IMAGE
    assert (Msg.FILL_KEY, bytes([1, 0, 0, 255, 0])) in deck.received


def test_reloads_when_the_file_changes(setup: tuple[HostService, FakeDeck, Path]) -> None:
    _, deck, cfg = setup
    write_config(cfg, {"keys": [{"index": 1, "label": "x", "color": "0000ff"}]})
    deck.wait_for(lambda r: (Msg.FILL_KEY, bytes([1, 0, 0, 0, 255])) in r)


def test_broken_reload_keeps_previous_config(
    setup: tuple[HostService, FakeDeck, Path], caplog: pytest.LogCaptureFixture
) -> None:
    service, _, cfg = setup
    write_config(cfg, {"brightness": "max"})
    wait_until(lambda: "config not reloaded" in caplog.text)
    assert service.config.brightness == 100


def test_wrong_size_image_falls_back_to_colour(
    tmp_path: Path, fake_deck: Callable[..., FakeDeck], caplog: pytest.LogCaptureFixture
) -> None:
    (tmp_path / "small.rgb565").write_bytes(b"\x00" * 10)
    cfg = tmp_path / "config.json"
    write_config(cfg, {"keys": [{"index": 0, "image": "small.rgb565", "color": "ff0000"}]})
    deck = fake_deck()
    service = HostService(ServiceSettings("127.0.0.1", deck.port, cfg, tmp_path, poll_s=0.02))
    worker = start(service)
    try:
        deck.wait_for(lambda r: (Msg.FILL_KEY, bytes([0, 0, 255, 0, 0])) in r)
        assert "the key window needs" in caplog.text
    finally:
        stop(service, worker)


def test_press_runs_the_action(
    tmp_path: Path, fake_deck: Callable[..., FakeDeck], monkeypatch: pytest.MonkeyPatch
) -> None:
    marker = tmp_path / "pressed"
    cfg = tmp_path / "config.json"
    argv = [sys.executable, "-c", f"open({str(marker)!r}, 'w').write('x')"]
    write_config(cfg, {"keys": [{"index": 1, "action": {"type": "command", "argv": argv}}]})
    watchers: list[threading.Thread] = []

    def tracking_run(action: Action) -> threading.Thread | None:
        watcher = actions.run_action(action)
        if watcher is not None:
            watchers.append(watcher)
        return watcher

    monkeypatch.setattr("d200_deck.service.keyactions.run_action", tracking_run)
    deck = fake_deck()
    service = HostService(ServiceSettings("127.0.0.1", deck.port, cfg, tmp_path, poll_s=0.02))
    worker = start(service)
    try:
        deck.wait_for(rendered)
        deck.push(Msg.KEY_EVENT, bytes([1, 0, 1]))
        wait_until(lambda: bool(watchers))
        watchers[0].join(timeout=5)
        assert marker.read_text() == "x"
    finally:
        stop(service, worker)


def test_saved_action_applies_before_the_next_poll(
    tmp_path: Path, fake_deck: Callable[..., FakeDeck], monkeypatch: pytest.MonkeyPatch
) -> None:
    started: list[Action] = []
    monkeypatch.setattr("d200_deck.service.keyactions.run_action", started.append)
    deck = fake_deck()
    # A long poll interval: the loop sits in next_event() while the config changes.
    service = HostService(ServiceSettings("127.0.0.1", deck.port, tmp_path / "c.json", tmp_path, poll_s=2.0))
    worker = start(service)
    try:
        deck.wait_for(rendered)
        action = Action("command", ("true",))
        service.update_config(service.config.with_key(KeyConfig(0, action=action)))
        deck.push(Msg.KEY_EVENT, bytes([0, 0, 1]))
        wait_until(lambda: started == [action])
    finally:
        stop_slow(service, worker)


def test_backlight_actions_from_device_and_web(tmp_path: Path, fake_deck: Callable[..., FakeDeck]) -> None:
    cfg = tmp_path / "config.json"
    keys = [{"index": 0, "action": {"type": "bl_down"}}, {"index": 1, "action": {"type": "disp_off"}}]
    write_config(cfg, {"brightness": 255, "keys": keys})
    deck = fake_deck()
    service = HostService(ServiceSettings("127.0.0.1", deck.port, cfg, tmp_path, poll_s=0.02))
    events = service.hub.subscribe()
    worker = start(service)
    try:
        deck.wait_for(rendered)
        deck.push(Msg.KEY_EVENT, bytes([0, 0, 1]))  # hardware press: via the session client
        deck.wait_for(lambda r: (Msg.SET_BRIGHTNESS, bytes([204])) in r)
        assert service.press(1) is True  # web press: via a connection of its own
        deck.wait_for(lambda r: (Msg.SET_BRIGHTNESS, bytes([0])) in r)
        seen = []
        while not events.empty():
            seen.append(events.get_nowait())
        assert ("action", {"index": 0, "type": "bl_down", "detail": "backlight 80%"}) in seen
        assert ("brightness", {"level": 204, "percent": 80, "display_on": False}) in seen
    finally:
        stop(service, worker)


def test_changed_start_brightness_sets_the_level(tmp_path: Path) -> None:
    service = HostService(ServiceSettings("127.0.0.1", 1, tmp_path / "config.json", tmp_path))
    service.brightness.apply("bl_down")
    service.update_config(service.config.with_key(KeyConfig(0, label="x")))  # brightness unchanged
    assert service.brightness.snapshot().level == 204
    service.update_config(replace(service.config, brightness=100))
    assert service.brightness.snapshot().level == 100


def test_unsupported_action_type_is_ignored(tmp_path: Path, caplog: pytest.LogCaptureFixture) -> None:
    service = HostService(ServiceSettings("127.0.0.1", 1, tmp_path / "config.json", tmp_path))
    service.info = parse_info(INFO_PAYLOAD)
    service.update_config(service.config.with_key(KeyConfig(0, action=Action("http", extra={"url": "x"}))))
    assert service.press(0) is False
    assert "'http' is not supported on this host" in caplog.text


def test_brightness_change_does_not_redraw_keys(setup: tuple[HostService, FakeDeck, Path]) -> None:
    """Regression: every config reload used to clear the screen and resend all keys (flicker)."""
    service, deck, _ = setup
    before = len(deck.received)
    service.update_config(replace(service.config, brightness=60))
    deck.wait_for(lambda r: (Msg.SET_BRIGHTNESS, bytes([60])) in r)
    time.sleep(0.1)  # give a (wrong) full redraw the chance to show up
    assert types(deck.received[before:]) == [Msg.SET_BRIGHTNESS]


def test_retries_while_deckd_is_unreachable(tmp_path: Path) -> None:
    attempts: list[int] = []

    def refuse(host: str, port: int) -> DeckClient:
        attempts.append(port)
        raise ConnectionRefusedError("nothing listens")

    settings = ServiceSettings("127.0.0.1", 7200, tmp_path / "config.json", tmp_path, reconnect_s=0.01)
    service = HostService(settings, connect=refuse)
    worker = start(service)
    try:
        wait_until(lambda: len(attempts) >= 3)
    finally:
        stop(service, worker)


def test_bootstraps_deckd_then_connects(
    tmp_path: Path, fake_deck: Callable[..., FakeDeck], caplog: pytest.LogCaptureFixture
) -> None:
    deck = fake_deck()
    booted = threading.Event()
    attempts: list[bool] = []

    def connect(host: str, port: int) -> DeckClient:
        attempts.append(booted.is_set())
        if not booted.is_set():
            raise ConnectionRefusedError("adb forward missing")
        return DeckClient.connect(host, port)

    def bootstrap() -> None:
        if len(attempts) < 3:
            raise DeviceNotFoundError("no D200 connected via adb")
        booted.set()

    settings = ServiceSettings(
        "127.0.0.1", deck.port, tmp_path / "c.json", tmp_path, poll_s=0.02, reconnect_s=0.01, bootstrap=bootstrap
    )
    service = HostService(settings, connect=connect)
    worker = start(service)
    try:
        deck.wait_for(rendered)
    finally:
        stop(service, worker)
    assert attempts[:3] == [False, False, False]
    assert attempts[-1] is True
    assert caplog.text.count("preparing the device failed: no D200") == 1  # the attempt at start
    assert caplog.text.count("starting it failed: no D200") == 1  # repeated errors are logged once


def test_broken_config_at_start_fails_fast(tmp_path: Path) -> None:
    cfg = tmp_path / "config.json"
    cfg.write_text('{"keys": "nope"}')
    with pytest.raises(ConfigError):
        HostService(ServiceSettings("127.0.0.1", 1, cfg, tmp_path))

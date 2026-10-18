from __future__ import annotations

from collections.abc import Callable

import pytest

from d200_deck.device.client import DeckClient
from d200_deck.device.protocol import Color, KeyEvent, Msg
from d200_deck.errors import DeviceError, ProtocolError

from .conftest import FakeDeck, Reply, default_handler


def test_hello_and_commands(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck()
    with DeckClient.connect(port=deck.port) as client:
        info = client.hello()
        client.set_brightness(200)
        client.fill_key(0, Color(255, 0, 0))
        client.ping()
    assert [k.index for k in info.keys] == [0, 1]
    assert [k.code for k in info.keys] == [29, 30]
    assert deck.received[1] == (Msg.SET_BRIGHTNESS, bytes([200]))
    assert deck.received[2] == (Msg.FILL_KEY, bytes([0, 0, 255, 0, 0]))


def test_device_error_is_raised(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck(lambda t, p: [(Msg.ERROR, b"unknown key code 99")])
    with DeckClient.connect(port=deck.port) as client, pytest.raises(DeviceError, match="unknown key code 99"):
        client.fill_key(99, Color(0, 0, 0))


def test_sysinfo(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck()
    with DeckClient.connect(port=deck.port) as client:
        hw = client.sysinfo()
    assert hw["cpu_cores"] == "4"
    assert hw["mem_available_kb"] == "84200"
    assert deck.received == [(Msg.SYSINFO, b"")]


def test_sysinfo_wrong_reply(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck(lambda t, p: [(Msg.INFO, b"")])
    with DeckClient.connect(port=deck.port) as client, pytest.raises(ProtocolError, match="expected SYSINFO_REPLY"):
        client.sysinfo()


def test_events_during_request_are_queued(fake_deck: Callable[..., FakeDeck]) -> None:
    def handler(msg_type: int, payload: bytes) -> Reply:
        if msg_type == Msg.PING:
            return [(Msg.KEY_EVENT, bytes([7, 0, 1])), (Msg.OK, b""), (Msg.KEY_EVENT, bytes([7, 0, 0]))]
        return default_handler(msg_type, payload)

    deck = fake_deck(handler)
    with DeckClient.connect(port=deck.port) as client:
        client.ping()
        assert client.next_event(timeout=2) == KeyEvent(7, True)
        assert client.next_event(timeout=2) == KeyEvent(7, False)


def test_unexpected_reply_and_bad_brightness(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck(lambda t, p: [(Msg.OK, b"")])
    with DeckClient.connect(port=deck.port) as client:
        with pytest.raises(ProtocolError, match="expected INFO"):
            client.hello()
        with pytest.raises(ValueError):
            client.set_brightness(256)

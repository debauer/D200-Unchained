from __future__ import annotations

import socket
import threading
from collections.abc import Iterator

import pytest

from d200_deck.device.client import DeckClient
from d200_deck.device.events import KeyDispatcher
from d200_deck.device.protocol import KeyEvent, Msg, encode_frame


def event_frame(index: int, pressed: bool) -> bytes:
    return encode_frame(Msg.KEY_EVENT, bytes([index, 0, int(pressed)]))


@pytest.fixture
def pair() -> Iterator[tuple[DeckClient, socket.socket]]:
    client_sock, device_sock = socket.socketpair()
    with client_sock, device_sock:
        yield DeckClient(client_sock), device_sock


def test_timeout_mid_frame_keeps_stream_in_sync(pair: tuple[DeckClient, socket.socket]) -> None:
    client, device = pair
    frame = event_frame(3, True)
    device.sendall(frame[:4])
    with pytest.raises(TimeoutError):
        client.next_event(timeout=0.05)
    device.sendall(frame[4:] + event_frame(4, False))
    assert client.next_event(timeout=1) == KeyEvent(3, True)
    assert client.next_event(timeout=1) == KeyEvent(4, False)


def test_dispatch_order_and_failing_callback(caplog: pytest.LogCaptureFixture) -> None:
    calls: list[str] = []
    a, b = socket.socketpair()
    a.close()
    b.close()
    d = KeyDispatcher(DeckClient(a))  # dispatch() never touches the socket
    d.on_event(lambda ev: calls.append(f"any {ev.index}"))

    def boom() -> None:
        raise RuntimeError("boom")

    d.on_press(2, boom)
    d.on_press(2, lambda: calls.append("press 2"))
    d.on_release(2, lambda: calls.append("release 2"))
    d.dispatch(KeyEvent(2, True))
    d.dispatch(KeyEvent(2, False))
    d.dispatch(KeyEvent(5, True))
    assert calls == ["any 2", "press 2", "any 2", "release 2", "any 5"]
    assert "callback for key 2 failed" in caplog.text


def test_run_dispatches_until_stopped(pair: tuple[DeckClient, socket.socket]) -> None:
    client, device = pair
    stop = threading.Event()
    seen: list[KeyEvent] = []
    d = KeyDispatcher(client)
    d.on_event(seen.append)
    d.on_release(1, stop.set)
    worker = threading.Thread(target=d.run, args=(stop, 0.05))
    worker.start()
    device.sendall(event_frame(1, True) + event_frame(1, False))
    worker.join(timeout=2)
    assert not worker.is_alive()
    assert seen == [KeyEvent(1, True), KeyEvent(1, False)]

from __future__ import annotations

import socket
import struct
import threading
import time
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field

import pytest

from d200_deck.device.protocol import Msg, encode_frame

Reply = list[tuple[Msg, bytes]]
Handler = Callable[[int, bytes], Reply]

INFO_PAYLOAD = (
    struct.pack("<HHHB", 4, 1280, 720, 2)
    + struct.pack("<6H", 0, 29, 11, 3, 196, 196)
    + struct.pack("<6H", 1, 30, 791, 521, 456, 196)
)

SYSINFO_PAYLOAD = (
    b"cpu_cores=4\ncpu_model=Test CPU\narch=arm\nkernel=5.10.160\n"
    b"mem_total_kb=118520\nmem_available_kb=84200\n"
    b"userdata_total_bytes=54525952\nuserdata_free_bytes=49283072\n"
    b"uptime_s=3661\nloadavg=0.10 0.05 0.01\ntemp_mc=42123\nserial=D200TEST\n"
)


def default_handler(msg_type: int, payload: bytes) -> Reply:
    if msg_type == Msg.HELLO:
        return [(Msg.INFO, INFO_PAYLOAD)]
    if msg_type == Msg.SYSINFO:
        return [(Msg.SYSINFO_REPLY, SYSINFO_PAYLOAD)]
    return [(Msg.OK, b"")]


@dataclass
class FakeDeck:
    """Minimal deckd stand-in on a random localhost port."""

    handler: Handler = default_handler
    received: list[tuple[int, bytes]] = field(default_factory=list)
    port: int = 0

    def __post_init__(self) -> None:
        self._server = socket.create_server(("127.0.0.1", 0))
        self.port = self._server.getsockname()[1]
        self._conn: socket.socket | None = None
        self._send_lock = threading.Lock()
        self.connected = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def push(self, msg: Msg, payload: bytes = b"") -> None:
        """Send an unsolicited frame (e.g. a KEY_EVENT) to the connected client."""
        assert self.connected.wait(timeout=2), "no client connected"
        assert self._conn is not None
        with self._send_lock:
            self._conn.sendall(encode_frame(msg, payload))

    def wait_for(self, predicate: Callable[[list[tuple[int, bytes]]], bool], timeout: float = 2.0) -> None:
        """Wait until `predicate(received)` holds."""
        deadline = time.monotonic() + timeout
        while not predicate(self.received):
            assert time.monotonic() < deadline, f"timed out, received: {[t for t, _ in self.received]}"
            time.sleep(0.01)

    def _recv_exact(self, conn: socket.socket, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf

    def _serve(self) -> None:
        """Accept any number of connections; `push` goes to the most recent one."""
        while True:
            try:
                conn, _ = self._server.accept()
            except OSError:  # server socket closed by close()
                return
            self._conn = conn
            self.connected.set()
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn: socket.socket) -> None:
        with conn:
            try:
                while True:
                    msg_type, length = struct.unpack("<BI", self._recv_exact(conn, 5))
                    payload = self._recv_exact(conn, length)
                    self.received.append((msg_type, payload))
                    for reply_type, body in self.handler(msg_type, payload):
                        with self._send_lock:
                            conn.sendall(encode_frame(reply_type, body))
            except ConnectionError:
                return

    def close(self) -> None:
        self._server.close()


@pytest.fixture
def fake_deck() -> Iterator[Callable[..., FakeDeck]]:
    decks: list[FakeDeck] = []

    def make(handler: Handler = default_handler) -> FakeDeck:
        deck = FakeDeck(handler)
        decks.append(deck)
        return deck

    yield make
    for d in decks:
        d.close()

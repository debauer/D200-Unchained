"""Blocking TCP client for deckd."""

from __future__ import annotations

import socket
from collections import deque
from types import TracebackType

from d200_deck.constants import DEFAULT_HOST, DEFAULT_PORT, DEFAULT_TIMEOUT_S, HEADER_SIZE, PROTOCOL_VERSION
from d200_deck.device.protocol import (
    Color,
    DeviceInfo,
    KeyEvent,
    Msg,
    PixelFormat,
    decode_header,
    encode_frame,
    fill_key_payload,
    key_image_payload,
    parse_info,
    parse_key_event,
    parse_sysinfo,
)
from d200_deck.errors import ConnectionClosedError, DeviceError, ProtocolError


class DeckClient:
    """One connection to deckd. Replies are read synchronously; key events that
    arrive in between are queued and returned by `next_event`."""

    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock
        self._events: deque[KeyEvent] = deque()
        # Bytes received but not yet consumed; survives socket timeouts mid-frame.
        self._rx = bytearray()

    @classmethod
    def connect(
        cls, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT_S
    ) -> DeckClient:
        sock = socket.create_connection((host, port), timeout=timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return cls(sock)

    def close(self) -> None:
        self._sock.close()

    def __enter__(self) -> DeckClient:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, tb: TracebackType | None
    ) -> None:
        self.close()

    def _fill(self, n: int) -> None:
        """Receive until at least `n` bytes are buffered; a timeout keeps what arrived."""
        while len(self._rx) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionClosedError("device closed the connection")
            self._rx += chunk

    def _read_frame(self) -> tuple[int, bytes]:
        self._fill(HEADER_SIZE)
        msg_type, length = decode_header(bytes(self._rx[:HEADER_SIZE]))
        self._fill(HEADER_SIZE + length)
        body = bytes(self._rx[HEADER_SIZE : HEADER_SIZE + length])
        del self._rx[: HEADER_SIZE + length]
        return msg_type, body

    def _request(self, msg: Msg, payload: bytes = b"") -> tuple[int, bytes]:
        self._sock.sendall(encode_frame(msg, payload))
        while True:
            msg_type, body = self._read_frame()
            if msg_type == Msg.KEY_EVENT:
                self._events.append(parse_key_event(body))
                continue
            if msg_type == Msg.ERROR:
                raise DeviceError(body.decode("utf-8", errors="replace"))
            return msg_type, body

    def _command(self, msg: Msg, payload: bytes = b"") -> None:
        msg_type, _ = self._request(msg, payload)
        if msg_type != Msg.OK:
            raise ProtocolError(f"expected OK for {msg.name}, got 0x{msg_type:02x}")

    def hello(self) -> DeviceInfo:
        msg_type, body = self._request(Msg.HELLO)
        if msg_type != Msg.INFO:
            raise ProtocolError(f"expected INFO, got 0x{msg_type:02x}")
        info = parse_info(body)
        if info.version != PROTOCOL_VERSION:
            raise ProtocolError(f"device speaks protocol {info.version}, client {PROTOCOL_VERSION}")
        return info

    def ping(self) -> None:
        self._command(Msg.PING)

    def sysinfo(self) -> dict[str, str]:
        """Hardware facts reported by deckd (CPU, memory, storage, ...)."""
        msg_type, body = self._request(Msg.SYSINFO)
        if msg_type != Msg.SYSINFO_REPLY:
            raise ProtocolError(f"expected SYSINFO_REPLY, got 0x{msg_type:02x}")
        return parse_sysinfo(body)

    def set_brightness(self, value: int) -> None:
        if not 0 <= value <= 255:
            raise ValueError(f"brightness must be 0-255, got {value}")
        self._command(Msg.SET_BRIGHTNESS, bytes((value,)))

    def clear(self, color: Color) -> None:
        self._command(Msg.CLEAR, color.to_bytes())

    def fill_key(self, index: int, color: Color) -> None:
        self._command(Msg.FILL_KEY, fill_key_payload(index, color))

    def set_key_image(
        self, index: int, width: int, height: int, pixels: bytes, fmt: PixelFormat = PixelFormat.RGB565
    ) -> None:
        self._command(Msg.SET_KEY_IMAGE, key_image_payload(index, width, height, pixels, fmt))

    def next_event(self, timeout: float | None = None) -> KeyEvent:
        """Return the next key event, waiting up to `timeout` seconds (None = forever).
        Raises `TimeoutError` when nothing arrived; the connection stays usable."""
        if self._events:
            return self._events.popleft()
        self._sock.settimeout(timeout)
        try:
            while True:
                msg_type, body = self._read_frame()
                if msg_type == Msg.KEY_EVENT:
                    return parse_key_event(body)
                raise ProtocolError(f"unexpected message 0x{msg_type:02x} while waiting for events")
        finally:
            self._sock.settimeout(DEFAULT_TIMEOUT_S)

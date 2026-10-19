"""A tiny MQTT 3.1.1 broker for tests: QoS 0/1 publish, exact-topic subscribe, retained messages."""

from __future__ import annotations

import socket
import socketserver
import threading


def _read(sock: socket.socket, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError
        data += chunk
    return data


def _packet(first: int, body: bytes) -> bytes:
    n, length = len(body), bytearray()
    while True:
        byte, n = n % 128, n // 128
        length.append(byte | (0x80 if n else 0))
        if not n:
            break
    return bytes([first, *length]) + body


def _publish(topic: str, payload: bytes, retain: bool = False) -> bytes:
    return _packet(0x30 | int(retain), len(topic.encode()).to_bytes(2, "big") + topic.encode() + payload)


class Handler(socketserver.BaseRequestHandler):
    server: MiniBroker

    def handle(self) -> None:
        sock = self.request
        try:
            while True:
                first = _read(sock, 1)[0]
                length, shift = 0, 0
                while True:
                    byte = _read(sock, 1)[0]
                    length |= (byte & 0x7F) << shift
                    shift += 7
                    if not byte & 0x80:
                        break
                body = _read(sock, length)
                self.packet(first, body)
        except (ConnectionError, OSError):
            pass
        finally:
            self.server.drop(sock)

    def packet(self, first: int, body: bytes) -> None:
        sock, kind = self.request, first >> 4
        if kind == 1:  # CONNECT
            self.server.connects.append(body)
            sock.sendall(bytes([0x20, 2, 0, 0]))
        elif kind == 8:  # SUBSCRIBE
            pid, rest, granted = body[:2], body[2:], bytearray()
            while rest:
                size = int.from_bytes(rest[:2], "big")
                topic = rest[2 : 2 + size].decode()
                rest = rest[3 + size :]
                granted.append(0)
                self.server.subscribe(sock, topic)
            sock.sendall(_packet(0x90, pid + bytes(granted)))
            for topic, payload in self.server.retained_for(sock):
                sock.sendall(_publish(topic, payload, retain=True))
        elif kind == 3:  # PUBLISH
            qos = (first >> 1) & 3
            size = int.from_bytes(body[:2], "big")
            topic, rest = body[2 : 2 + size].decode(), body[2 + size :]
            if qos:
                sock.sendall(_packet(0x40, rest[:2]))
                rest = rest[2:]
            self.server.received.append((topic, rest.decode(), qos, bool(first & 1)))
            self.server.deliver(topic, rest)
        elif kind == 12:  # PINGREQ
            sock.sendall(bytes([0xD0, 0]))


class MiniBroker(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), Handler)
        self.port: int = self.server_address[1]
        self.received: list[tuple[str, str, int, bool]] = []
        self.connects: list[bytes] = []
        self.retained: dict[str, bytes] = {}
        self._subs: dict[socket.socket, set[str]] = {}
        self._lock = threading.Lock()
        threading.Thread(target=self.serve_forever, args=(0.05,), daemon=True).start()

    def subscribe(self, sock: socket.socket, topic: str) -> None:
        with self._lock:
            self._subs.setdefault(sock, set()).add(topic)

    def retained_for(self, sock: socket.socket) -> list[tuple[str, bytes]]:
        with self._lock:
            return [(t, p) for t, p in self.retained.items() if t in self._subs.get(sock, set())]

    def drop(self, sock: socket.socket) -> None:
        with self._lock:
            self._subs.pop(sock, None)

    def subscribers(self, topic: str) -> int:
        with self._lock:
            return sum(topic in topics for topics in self._subs.values())

    def deliver(self, topic: str, payload: bytes) -> None:
        with self._lock:
            targets = [s for s, topics in self._subs.items() if topic in topics]
        for sock in targets:
            sock.sendall(_publish(topic, payload))

    def inject(self, topic: str, payload: bytes, retain: bool = False) -> None:
        """Publish as if another client had (retained ones also reach later subscribers)."""
        if retain:
            self.retained[topic] = payload
        self.deliver(topic, payload)

    def close(self) -> None:
        self.shutdown()
        self.server_close()

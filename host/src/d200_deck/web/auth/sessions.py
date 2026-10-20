"""Server-side sessions: random 256-bit tokens, only their hashes are kept, absolute monotonic expiry."""

from __future__ import annotations

import hashlib
import re
import secrets
import threading
import time
from dataclasses import dataclass

MAX_SESSIONS = 8
_COOKIE_VALUE = re.compile(r"^[0-9a-f]{64}$")


@dataclass(frozen=True)
class Session:
    username: str
    expires: float  # time.monotonic()


def parse_cookie(header: str, name: str) -> str | None:
    """The session token if `header` holds exactly one cookie `name` with a well-formed value."""
    found = [v for k, _, v in (part.strip().partition("=") for part in header.split(";")) if k == name]
    if len(found) != 1 or not _COOKIE_VALUE.match(found[0]):
        return None
    return found[0]


def _digest(token: str) -> bytes:
    return hashlib.sha256(token.encode()).digest()


class SessionStore:
    def __init__(self, ttl_s: int, capacity: int = MAX_SESSIONS) -> None:
        self.ttl_s = ttl_s
        self.capacity = capacity
        self._lock = threading.Lock()
        self._sessions: dict[bytes, Session] = {}

    def create(self, username: str) -> str:
        """A new session (always a fresh token); the oldest one makes room when the table is full."""
        token = secrets.token_hex(32)
        now = time.monotonic()
        with self._lock:
            self._sessions = {k: s for k, s in self._sessions.items() if s.expires > now}
            while len(self._sessions) >= self.capacity:
                oldest = min(self._sessions, key=lambda k: self._sessions[k].expires)
                del self._sessions[oldest]
            self._sessions[_digest(token)] = Session(username, now + self.ttl_s)
        return token

    def lookup(self, token: str | None) -> Session | None:
        if token is None:
            return None
        with self._lock:
            session = self._sessions.get(_digest(token))
        if session is None or session.expires <= time.monotonic():
            return None
        return session

    def revoke(self, token: str | None) -> None:
        if token is not None:
            with self._lock:
                self._sessions.pop(_digest(token), None)

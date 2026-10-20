"""Login throttling: a few attempts per source and overall per minute."""

from __future__ import annotations

import threading
import time

WINDOW_S = 60.0
PER_SOURCE = 5
GLOBAL = 20
MAX_SOURCES = 64


class LoginThrottle:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._global: list[float] = []
        self._sources: dict[str, list[float]] = {}

    def attempt(self, source: str) -> int:
        """Count an attempt; 0 if allowed, else the seconds until the next one may be made."""
        now = time.monotonic()
        with self._lock:
            self._global = [t for t in self._global if now - t < WINDOW_S]
            mine = [t for t in self._sources.get(source, []) if now - t < WINDOW_S]
            for history, limit in ((mine, PER_SOURCE), (self._global, GLOBAL)):
                if len(history) >= limit:
                    return max(1, int(WINDOW_S - (now - history[0])) + 1)
            mine.append(now)
            self._global.append(now)
            self._sources[source] = mine
            if len(self._sources) > MAX_SOURCES:
                self._sources = {k: v for k, v in self._sources.items() if v and now - v[-1] < WINDOW_S}
        return 0

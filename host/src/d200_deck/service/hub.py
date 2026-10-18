"""Fan-out of service events (key presses, connection status) to web subscribers."""

from __future__ import annotations

import logging
import queue
import threading
from typing import Any

log = logging.getLogger(__name__)

Message = tuple[str, dict[str, Any]]
QUEUE_SIZE = 100


class EventHub:
    """Each subscriber gets its own bounded queue; a subscriber that falls
    `QUEUE_SIZE` messages behind is dropped instead of slowing the service."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._subscribers: list[queue.Queue[Message | None]] = []

    def subscribe(self) -> queue.Queue[Message | None]:
        q: queue.Queue[Message | None] = queue.Queue(maxsize=QUEUE_SIZE)
        with self._lock:
            self._subscribers.append(q)
        return q

    def unsubscribe(self, q: queue.Queue[Message | None]) -> None:
        with self._lock:
            if q in self._subscribers:
                self._subscribers.remove(q)

    def publish(self, kind: str, data: dict[str, Any]) -> None:
        with self._lock:
            subscribers = list(self._subscribers)
        for q in subscribers:
            try:
                q.put_nowait((kind, data))
            except queue.Full:
                log.warning("dropping a web event subscriber that stopped reading")
                self.unsubscribe(q)
                self._close(q)

    def close_all(self) -> None:
        """Wake every subscriber with `None` so its stream can end."""
        with self._lock:
            subscribers, self._subscribers = self._subscribers, []
        for q in subscribers:
            self._close(q)

    @staticmethod
    def _close(q: queue.Queue[Message | None]) -> None:
        try:
            q.put_nowait(None)
        except queue.Full:
            # Make room for the end marker; the reader stops at it anyway.
            q.get_nowait()
            q.put_nowait(None)

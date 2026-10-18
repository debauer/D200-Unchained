"""Key event callbacks on top of `DeckClient`."""

from __future__ import annotations

import logging
import threading
from collections import defaultdict
from collections.abc import Callable

from d200_deck.device.client import DeckClient
from d200_deck.device.protocol import KeyEvent

log = logging.getLogger(__name__)

KeyCallback = Callable[[], None]
EventCallback = Callable[[KeyEvent], None]


class KeyDispatcher:
    """Calls registered callbacks for key presses and releases.

    Callbacks run in the thread that calls `run()`; long work belongs in its own
    thread or process so the panel stays responsive."""

    def __init__(self, client: DeckClient) -> None:
        self.client = client
        self._press: defaultdict[int, list[KeyCallback]] = defaultdict(list)
        self._release: defaultdict[int, list[KeyCallback]] = defaultdict(list)
        self._any: list[EventCallback] = []

    def on_press(self, index: int, callback: KeyCallback) -> None:
        self._press[index].append(callback)

    def on_release(self, index: int, callback: KeyCallback) -> None:
        self._release[index].append(callback)

    def on_event(self, callback: EventCallback) -> None:
        """Called for every key event, before the per-key callbacks."""
        self._any.append(callback)

    def dispatch(self, event: KeyEvent) -> None:
        for any_cb in self._any:
            self._call(event, any_cb, event)
        for key_cb in (self._press if event.pressed else self._release).get(event.index, []):
            self._call(event, key_cb)

    @staticmethod
    def _call(event: KeyEvent, fn: Callable[..., None], *args: KeyEvent) -> None:
        try:
            fn(*args)
        except Exception:
            # A failing user callback must not stop the panel; the traceback is logged.
            log.exception("callback for key %d failed", event.index)

    def run(self, stop: threading.Event | None = None, poll_s: float = 0.5) -> None:
        """Dispatch events until `stop` is set (checked every `poll_s` seconds) or the
        connection fails (the client error propagates)."""
        stop = stop or threading.Event()
        while not stop.is_set():
            try:
                event = self.client.next_event(timeout=poll_s)
            except TimeoutError:
                continue
            self.dispatch(event)

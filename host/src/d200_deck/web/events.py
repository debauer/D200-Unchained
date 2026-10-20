"""Server-sent events for `GET /api/events`."""

from __future__ import annotations

import json
import queue
import time
from collections.abc import Callable
from http import HTTPStatus
from typing import TYPE_CHECKING, Any

from d200_deck.web.auth.sessions import Session
from d200_deck.web.mqtt_api import mqtt_state

if TYPE_CHECKING:
    from d200_deck.web.server import DeckHandler

KEEPALIVE_S = 15.0
POLL_S = 1.0  # how quickly a stream notices that its session ended


def stream_events(handler: DeckHandler, session_of: Callable[[DeckHandler], Session | None]) -> None:
    """Stream until the service stops, the subscriber is dropped or the session of the request ends."""
    service = handler.server.service
    auth = handler.server.auth
    q = service.hub.subscribe()
    try:
        handler.send_response(HTTPStatus.OK)
        handler.send_header("Content-Type", "text/event-stream")
        handler.send_header("Cache-Control", "no-store")
        handler.end_headers()
        _send(handler, "status", {"connected": service.connected})
        _send(handler, "brightness", service.brightness.snapshot().to_json())
        _send(handler, "page", {"page": service.page})
        _send(handler, "values", service.values())
        _send(handler, "mqtt", mqtt_state(service))
        quiet_since = time.monotonic()
        while not service.stop.is_set():
            if auth.enabled and session_of(handler) is None:
                break
            try:
                message = q.get(timeout=POLL_S)
            except queue.Empty:
                if time.monotonic() - quiet_since >= KEEPALIVE_S:
                    handler.wfile.write(b": keepalive\n\n")
                    handler.wfile.flush()
                    quiet_since = time.monotonic()
                continue
            if message is None:
                break
            _send(handler, *message)
            quiet_since = time.monotonic()
    finally:
        service.hub.unsubscribe(q)
        handler.close_connection = True


def _send(handler: DeckHandler, kind: str, data: dict[str, Any]) -> None:
    handler.wfile.write(f"event: {kind}\ndata: {json.dumps(data)}\n\n".encode())
    handler.wfile.flush()

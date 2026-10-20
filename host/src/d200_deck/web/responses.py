"""Reading request bodies and writing responses for the web handler."""

from __future__ import annotations

import json
from collections.abc import Mapping
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from typing import TYPE_CHECKING, Any

from d200_deck.web.apierror import ApiError

if TYPE_CHECKING:
    from d200_deck.web.server import DeckWebServer

MAX_BODY = 2 << 20
CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
    ".txt": "text/plain; charset=utf-8",
}
Headers = Mapping[str, str | tuple[str, ...]]  # a tuple sends the header once per value (Set-Cookie)


class ResponseMixin(BaseHTTPRequestHandler):
    server: DeckWebServer
    _body_read = False  # whether the request body was consumed (else the connection must close)

    def _body(self) -> bytes:
        return self.read_body(MAX_BODY)

    def read_body(self, limit: int) -> bytes:
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError as e:
            raise ApiError(HTTPStatus.LENGTH_REQUIRED, "Content-Length required") from e
        if not 0 <= length <= limit:
            raise ApiError(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, f"body limit is {limit} bytes")
        self._body_read = True
        return self.rfile.read(length)

    def _send(self, status: HTTPStatus, content_type: str, body: bytes, headers: Headers | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for name, value in (headers or {}).items():
            for single in (value,) if isinstance(value, str) else value:
                self.send_header(name, single)
        self.end_headers()
        self.wfile.write(body)

    def _raw_file(self, path: Path | None, missing: str) -> None:
        if path is None or not path.is_file():
            raise ApiError(HTTPStatus.NOT_FOUND, missing)
        self._send(HTTPStatus.OK, "application/octet-stream", path.read_bytes())

    def _json(self, status: HTTPStatus, data: Any, headers: Headers | None = None) -> None:
        self._send(status, "application/json", json.dumps(data).encode(), headers)

    json_response = _json

    def redirect(self, location: str, headers: Headers | None = None) -> None:
        self._send(HTTPStatus.FOUND, "text/plain", b"", {"Location": location, **(headers or {})})

    def _static(self, path: str) -> None:
        root = self.server.root.resolve()
        target = (root / (path.lstrip("/") or "index.html")).resolve()
        if not target.is_relative_to(root) or not target.is_file():
            raise ApiError(HTTPStatus.NOT_FOUND, f"no file {path}")
        content_type = CONTENT_TYPES.get(target.suffix, "application/octet-stream")
        self._send(HTTPStatus.OK, content_type, target.read_bytes())

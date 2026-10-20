"""Web API v1 and static UI for `deck serve` (see docs/web-api.md)."""

from __future__ import annotations

import json
import logging
import re
import threading
from dataclasses import asdict
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib import resources
from pathlib import Path
from typing import Any

from d200_deck import config as cfgmod
from d200_deck.errors import (
    ConfigError,
    DeckError,
    ImageSizeError,
    UnknownKeyError,
    UnknownPageError,
)
from d200_deck.service.host import HostService
from d200_deck.service.keyactions import HOST_ACTIONS
from d200_deck.web.apierror import ApiError
from d200_deck.web.auth.gate import AuthGate
from d200_deck.web.auth.settings import AuthSettings
from d200_deck.web.events import stream_events
from d200_deck.web.mqtt_api import mqtt_state, update_mqtt

log = logging.getLogger(__name__)

API_VERSION = 1
MAX_BODY = 2 << 20
SHUTDOWN_POLL_S = 0.1  # how quickly shutdown() returns
MUTATING = {"PUT", "POST", "DELETE"}
CSRF_HEADER = "X-Deck-Request"
CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
    ".txt": "text/plain; charset=utf-8",
}
# /api/keys/<n>/... is the root page, /api/pages/<page>/keys/<n>/... any page.
KEY_PATH = re.compile(r"^/api/(?:pages/([a-z0-9_-]{1,32})/)?keys/(\d+)/(image|icon|alticon|press)$")


def web_root() -> Path:
    return Path(str(resources.files("d200_deck") / "web" / "static"))


def info_json(service: HostService) -> dict[str, Any]:
    info = service.info
    return {
        "api": API_VERSION,
        "host": "linux",
        "connected": service.connected,
        "screen": {"width": info.width, "height": info.height},
        "keys": [asdict(k) for k in info.keys],
        "actions": list(HOST_ACTIONS),
        "brightness": service.brightness.snapshot().to_json(),
        "page": service.page,
        "values": service.values(),
        "variable_types": [cfgmod.VARIABLE_COMMAND, cfgmod.VARIABLE_MQTT],
    }


class DeckHandler(BaseHTTPRequestHandler):
    """One request; `service`, `root` and `allowed_hosts` are set on the server."""

    server: DeckWebServer
    protocol_version = "HTTP/1.1"
    _body_read = False

    def log_message(self, format: str, *args: Any) -> None:
        log.debug("%s %s", self.address_string(), format % args)

    # --- dispatch --------------------------------------------------------

    def do_GET(self) -> None:
        self._handle("GET")

    def do_PUT(self) -> None:
        self._handle("PUT")

    def do_POST(self) -> None:
        self._handle("POST")

    def do_DELETE(self) -> None:
        self._handle("DELETE")

    def _handle(self, method: str) -> None:
        self._body_read = False
        try:
            self._check_access(method)
            path = self.path.split("?", 1)[0]
            if path.startswith("/auth/"):
                self.server.auth.handle(self, method, path)
            elif path.startswith("/api/"):
                self.server.auth.guard(self, path)
                self._api(method, path)
            elif method == "GET":
                self._static(path)
            else:
                raise ApiError(HTTPStatus.METHOD_NOT_ALLOWED, f"{method} not allowed on {path}")
        except ApiError as e:
            headers = dict(e.headers)
            if method in MUTATING and not self._body_read:
                headers["Connection"] = "close"  # the unread request body must not be parsed as the next request
            self._json(e.status, {"error": str(e)}, headers)
        except (BrokenPipeError, ConnectionResetError):
            log.debug("client went away during %s %s", method, self.path)

    def _check_access(self, method: str) -> None:
        if self.headers.get("Host", "") not in self.server.allowed_hosts:
            raise ApiError(HTTPStatus.FORBIDDEN, "host not allowed")
        if method in MUTATING and self.headers.get(CSRF_HEADER) != "1":
            raise ApiError(HTTPStatus.FORBIDDEN, f"missing {CSRF_HEADER} header")
        origin = self.headers.get("Origin")
        if method in MUTATING and origin is not None and origin != self.server.origin_for(self.headers["Host"]):
            raise ApiError(HTTPStatus.FORBIDDEN, "origin not allowed")

    def _api(self, method: str, path: str) -> None:
        service = self.server.service
        if path == "/api/info" and method == "GET":
            self._json(HTTPStatus.OK, info_json(service))
        elif path == "/api/config" and method == "GET":
            self._json(HTTPStatus.OK, cfgmod.to_json(service.config))
        elif path == "/api/config" and method == "PUT":
            try:
                cfg = cfgmod.parse(json.loads(self._body()))
            except (ValueError, ConfigError) as e:  # JSONDecodeError is a ValueError
                raise ApiError(HTTPStatus.BAD_REQUEST, str(e)) from e
            service.update_config(cfg)
            self._json(HTTPStatus.OK, cfgmod.to_json(cfg))
        elif path == "/api/mqtt" and method == "GET":
            self._json(HTTPStatus.OK, mqtt_state(service))
        elif path == "/api/mqtt" and method == "PUT":
            try:
                self._json(HTTPStatus.OK, update_mqtt(service, self._body()))
            except ConfigError as e:
                raise ApiError(HTTPStatus.BAD_REQUEST, str(e)) from e
        elif path == "/api/events" and method == "GET":
            stream_events(self, self.server.auth.session)
        elif path == "/api/page" and method == "POST":
            self._show_page()
        elif m := KEY_PATH.match(path):
            self._key(method, m.group(1) or cfgmod.ROOT, int(m.group(2)), m.group(3))
        else:
            raise ApiError(HTTPStatus.NOT_FOUND, f"no endpoint {method} {path}")

    def _show_page(self) -> None:
        try:
            data = json.loads(self._body())
            page = data["page"]
        except (ValueError, KeyError, TypeError) as e:  # JSONDecodeError is a ValueError
            raise ApiError(HTTPStatus.BAD_REQUEST, 'expected {"page": "<id>"}') from e
        try:
            self.server.service.show_page(page)
        except UnknownPageError as e:
            raise ApiError(HTTPStatus.NOT_FOUND, str(e)) from e
        self._json(HTTPStatus.OK, {"page": page})

    def _key(self, method: str, page: str, index: int, what: str) -> None:
        handlers = {"press": self._key_press, "image": self._key_image}
        try:
            if what in ("icon", "alticon"):
                self._key_icon(method, page, index, alt=what == "alticon")
            else:
                handlers[what](method, page, index)
        except (UnknownKeyError, UnknownPageError) as e:
            raise ApiError(HTTPStatus.NOT_FOUND, str(e)) from e
        except ImageSizeError as e:
            raise ApiError(HTTPStatus.BAD_REQUEST, str(e)) from e
        except (DeckError, OSError) as e:  # e.g. deckd unreachable for a backlight action
            raise ApiError(HTTPStatus.SERVICE_UNAVAILABLE, f"device not reachable: {e}") from e

    def _key_press(self, method: str, page: str, index: int) -> None:
        if method != "POST":
            raise ApiError(HTTPStatus.METHOD_NOT_ALLOWED, f"{method} not allowed on press")
        self._json(HTTPStatus.OK, {"started": self.server.service.press(index, page)})

    def _key_image(self, method: str, page: str, index: int) -> None:
        service = self.server.service
        if method == "GET":
            path = service.image_path(index, page)
            self._raw_file(path, f"key {index} has no image")
            return
        if method == "PUT":
            key = service.store_image(index, self._body(), page)
        elif method == "DELETE":
            key = service.delete_image(index, page)
        else:
            raise ApiError(HTTPStatus.METHOD_NOT_ALLOWED, f"{method} not allowed on image")
        self._json(HTTPStatus.OK, cfgmod.key_to_json(key))

    def _key_icon(self, method: str, page: str, index: int, alt: bool) -> None:
        service = self.server.service
        if method == "GET":
            service.window(index)
            self._raw_file(service.icon_path(index, page, alt), f"key {index} has no icon")
        elif method == "PUT":
            service.store_icon(index, self._body(), page, alt)
            self._json(HTTPStatus.OK, {"stored": True})
        elif method == "DELETE":
            service.delete_icon(index, page, alt)
            self._json(HTTPStatus.OK, {"stored": False})
        else:
            raise ApiError(HTTPStatus.METHOD_NOT_ALLOWED, f"{method} not allowed on icon")

    # --- helpers ---------------------------------------------------------

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

    def _send(self, status: HTTPStatus, content_type: str, body: bytes, headers: dict[str, str] | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def _raw_file(self, path: Path | None, missing: str) -> None:
        if path is None or not path.is_file():
            raise ApiError(HTTPStatus.NOT_FOUND, missing)
        self._send(HTTPStatus.OK, "application/octet-stream", path.read_bytes())

    def _json(self, status: HTTPStatus, data: Any, headers: dict[str, str] | None = None) -> None:
        self._send(status, "application/json", json.dumps(data).encode(), headers)

    json_response = _json

    def _static(self, path: str) -> None:
        root = self.server.root.resolve()
        target = (root / (path.lstrip("/") or "index.html")).resolve()
        if not target.is_relative_to(root) or not target.is_file():
            raise ApiError(HTTPStatus.NOT_FOUND, f"no file {path}")
        content_type = CONTENT_TYPES.get(target.suffix, "application/octet-stream")
        self._send(HTTPStatus.OK, content_type, target.read_bytes())


class DeckWebServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self,
        address: tuple[str, int],
        service: HostService,
        root: Path,
        extra_hosts: tuple[str, ...],
        auth: AuthSettings,
    ):
        super().__init__(address, DeckHandler)
        self.service = service
        self.root = root
        self.auth = AuthGate(auth)
        port = self.server_address[1]
        names = {address[0], "localhost", "127.0.0.1"}
        self.allowed_hosts = {f"{n}:{port}" for n in names} | set(extra_hosts)
        if auth.public_origin:
            self.allowed_hosts.add(auth.public_host)

    def origin_for(self, host: str) -> str:
        """The Origin a browser sends for this server: the configured public one, else derived from Host."""
        return self.auth.settings.public_origin or f"http://{host}"


def start_web(  # noqa: PLR0913, PLR0917 - one parameter per setting
    service: HostService,
    host: str,
    port: int,
    root: Path | None = None,
    extra_hosts: tuple[str, ...] = (),
    auth: AuthSettings | None = None,
) -> DeckWebServer:
    """Start the web server in a background thread; stop it with `server.shutdown()`."""
    server = DeckWebServer((host, port), service, root or web_root(), extra_hosts, auth or AuthSettings())
    threading.Thread(target=server.serve_forever, args=(SHUTDOWN_POLL_S,), name="deck-web", daemon=True).start()
    log.info("web UI on http://%s:%d/", host, server.server_address[1])
    return server

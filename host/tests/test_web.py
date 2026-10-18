from __future__ import annotations

import http.client
import json
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from d200_deck import config as cfgmod
from d200_deck.device.protocol import parse_info
from d200_deck.service.host import HostService, ServiceSettings
from d200_deck.web.server import DeckWebServer, start_web

from .conftest import INFO_PAYLOAD

KEY0 = 196 * 196 * 2
MUTATE = {"X-Deck-Request": "1"}


class Web:
    def __init__(self, server: DeckWebServer, service: HostService) -> None:
        self.server = server
        self.service = service
        self.port = server.server_address[1]

    def request(
        self, method: str, path: str, body: bytes | None = None, headers: dict[str, str] | None = None
    ) -> tuple[int, str, bytes]:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            conn.request(method, path, body=body, headers=headers or {})
            res = conn.getresponse()
            return res.status, res.getheader("Content-Type", ""), res.read()
        finally:
            conn.close()

    def json(self, method: str, path: str, data: Any = None, raw: bytes | None = None) -> tuple[int, Any]:
        body = raw if raw is not None else (json.dumps(data).encode() if data is not None else None)
        status, _, payload = self.request(method, path, body, MUTATE if method != "GET" else {})
        return status, json.loads(payload)


@pytest.fixture
def web(tmp_path: Path) -> Iterator[Web]:
    settings = ServiceSettings("127.0.0.1", 1, tmp_path / "config.json", tmp_path / "images")
    service = HostService(settings)
    service.info = parse_info(INFO_PAYLOAD)
    service.connected = True
    server = start_web(service, "127.0.0.1", 0)
    yield Web(server, service)
    server.shutdown()
    server.server_close()


def test_static_files(web: Web) -> None:
    status, ctype, body = web.request("GET", "/")
    assert status == 200
    assert ctype.startswith("text/html")
    assert b"D200 // Configurator" in body
    status, ctype, _ = web.request("GET", "/app.js")
    assert (status, ctype) == (200, "text/javascript; charset=utf-8")
    assert web.request("GET", "/favicon.svg")[:2] == (200, "image/svg+xml")
    status, ctype, body = web.request("GET", "/fonts/chakra-petch-700.woff2")
    assert (status, ctype, body[:4]) == (200, "font/woff2", b"wOF2")
    assert web.request("GET", "/fonts/OFL-ChakraPetch.txt")[1].startswith("text/plain")
    assert web.request("GET", "/../web.py")[0] == 404
    assert web.request("GET", "/%2e%2e/web.py")[0] == 404
    assert web.request("POST", "/", headers=MUTATE)[0] == 405


def test_info(web: Web) -> None:
    status, info = web.json("GET", "/api/info")
    assert status == 200
    assert info["connected"] is True
    assert info["actions"] == ["command", "bl_down", "bl_up", "disp_off", "disp_on", "page", "back"]
    assert info["brightness"] == {"level": 255, "percent": 100, "display_on": True}
    assert info["variable_types"] == ["command"]
    assert {"time", "date"} <= set(info["values"])
    assert info["screen"] == {"width": 1280, "height": 720}
    assert info["keys"][1] == {"index": 1, "code": 30, "x": 791, "y": 521, "width": 456, "height": 196}


def test_guards_against_csrf_and_rebinding(web: Web) -> None:
    body = json.dumps({"brightness": 10}).encode()
    assert web.request("PUT", "/api/config", body)[0] == 403
    status, _, _ = web.request("GET", "/api/info", headers={"Host": "evil.example:7280"})
    assert status == 403


def test_put_config_validates_and_saves(web: Web) -> None:
    status, saved = web.json("PUT", "/api/config", {"brightness": 42, "keys": [{"index": 0, "label": "A"}]})
    assert status == 200
    assert saved["brightness"] == 42
    assert cfgmod.load(web.service.settings.config_path).brightness == 42
    status, error = web.json("PUT", "/api/config", {"brightness": 999})
    assert status == 400
    assert "brightness" in error["error"]
    assert web.service.config.brightness == 42
    assert web.json("PUT", "/api/config", raw=b"{not json")[0] == 400


def test_key_image_lifecycle(web: Web) -> None:
    assert web.json("PUT", "/api/keys/0/image", raw=b"\x00" * 10)[0] == 400
    status, key = web.json("PUT", "/api/keys/0/image", raw=b"\x12\x34" * (KEY0 // 2))
    assert status == 200
    assert key["image"] == "key-0.rgb565"
    status, ctype, body = web.request("GET", "/api/keys/0/image")
    assert (status, ctype, len(body)) == (200, "application/octet-stream", KEY0)
    status, key = web.json("DELETE", "/api/keys/0/image")
    assert status == 200
    assert "image" not in key
    assert web.request("GET", "/api/keys/0/image")[0] == 404
    assert not (web.service.settings.images_dir / "key-0.rgb565").exists()


def test_icon_lifecycle(web: Web) -> None:
    assert web.request("GET", "/api/keys/0/icon")[0] == 404
    assert web.json("PUT", "/api/keys/0/icon", raw=b"\x00" * KEY0) == (200, {"stored": True})
    assert web.request("GET", "/api/keys/0/icon")[0] == 200
    assert web.service.config.key(0).image is None  # icons are never shown directly
    assert web.json("DELETE", "/api/keys/0/icon") == (200, {"stored": False})


def test_unknown_key(web: Web) -> None:
    assert web.json("PUT", "/api/keys/99/image", raw=b"")[0] == 404
    assert web.json("POST", "/api/keys/99/press")[0] == 404
    assert web.json("GET", "/api/nope")[0] == 404


def test_press_never_starts_commands(web: Web, tmp_path: Path) -> None:
    assert web.json("POST", "/api/keys/0/press") == (200, {"started": False})
    marker = tmp_path / "pressed"
    argv = [sys.executable, "-c", f"open({str(marker)!r}, 'w').write('x')"]
    web.json("PUT", "/api/config", {"keys": [{"index": 0, "action": {"type": "command", "argv": argv}}]})
    assert web.json("POST", "/api/keys/0/press") == (200, {"started": True})
    assert not marker.exists()  # the test press only reports the command


def test_events_stream(web: Web) -> None:
    conn = http.client.HTTPConnection("127.0.0.1", web.port, timeout=5)
    try:
        conn.request("GET", "/api/events")
        res = conn.getresponse()
        assert res.getheader("Content-Type") == "text/event-stream"
        assert res.readline() == b"event: status\n"
        assert json.loads(res.readline().removeprefix(b"data: ")) == {"connected": True}
        assert res.readline() == b"\n"
        assert res.readline() == b"event: brightness\n"
        assert json.loads(res.readline().removeprefix(b"data: "))["percent"] == 100
        assert res.readline() == b"\n"
        assert res.readline() == b"event: page\n"
        assert json.loads(res.readline().removeprefix(b"data: ")) == {"page": "root"}
        assert res.readline() == b"\n"
        assert res.readline() == b"event: values\n"
        assert {"time", "date"} <= set(json.loads(res.readline().removeprefix(b"data: ")))
        assert res.readline() == b"\n"
        web.service.hub.publish("key", {"index": 1, "pressed": True})
        assert res.readline() == b"event: key\n"
        assert json.loads(res.readline().removeprefix(b"data: ")) == {"index": 1, "pressed": True}
    finally:
        web.service.hub.close_all()
        conn.close()


PAGES = {
    "version": 2,
    "pages": [{"id": "root", "parent": None}, {"id": "lights", "name": "Lights", "parent": "root"}],
}


def test_pages_in_config_and_navigation(web: Web) -> None:
    status, saved = web.json("PUT", "/api/config", PAGES)
    assert status == 200
    assert [p["id"] for p in saved["pages"]] == ["root", "lights"]
    assert web.json("GET", "/api/info")[1]["page"] == "root"
    assert web.json("POST", "/api/page", {"page": "lights"}) == (200, {"page": "lights"})
    assert web.json("GET", "/api/info")[1]["page"] == "lights"
    assert web.json("POST", "/api/page", {"page": "nope"})[0] == 404
    assert web.json("POST", "/api/page", raw=b"{}")[0] == 400


def test_key_endpoints_per_page(web: Web) -> None:
    web.json("PUT", "/api/config", PAGES)
    status, key = web.json("PUT", "/api/pages/lights/keys/0/image", raw=b"\x00" * KEY0)
    assert (status, key["image"]) == (200, "lights-key-0.rgb565")
    assert web.request("GET", "/api/pages/lights/keys/0/image")[0] == 200
    assert web.request("GET", "/api/keys/0/image")[0] == 404  # the root page has no image
    assert web.json("PUT", "/api/pages/lights/keys/0/icon", raw=b"\x00" * KEY0) == (200, {"stored": True})
    assert web.json("PUT", "/api/pages/nope/keys/0/icon", raw=b"\x00" * KEY0)[0] == 404
    assert web.json("POST", "/api/pages/lights/keys/0/press") == (200, {"started": False})

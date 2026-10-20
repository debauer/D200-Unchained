from __future__ import annotations

import hashlib
import http.client
import json
import time
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest

from d200_deck.device.protocol import parse_info
from d200_deck.errors import ConfigError
from d200_deck.service.host import HostService, ServiceSettings
from d200_deck.web.auth.sessions import SessionStore, parse_cookie
from d200_deck.web.auth.settings import AuthSettings, load_settings, parse_settings, save_settings
from d200_deck.web.auth.throttle import PER_SOURCE, LoginThrottle
from d200_deck.web.auth.verifier import check_password, make_verifier, parse_verifier
from d200_deck.web.server import DeckWebServer, start_web

from .conftest import INFO_PAYLOAD

JSON = {"X-Deck-Request": "1", "Content-Type": "application/json"}
PASSWORD = 'correct horse "battery"'
VERIFIER = make_verifier(PASSWORD, 10_000)


class Site:
    def __init__(self, server: DeckWebServer) -> None:
        self.server = server
        self.port = server.server_address[1]

    def call(
        self, method: str, path: str, body: dict[str, object] | None = None, headers: dict[str, str] | None = None
    ) -> tuple[int, dict[str, str], dict[str, object]]:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            data = json.dumps(body).encode() if body is not None else None
            conn.request(method, path, body=data, headers={**(JSON if method != "GET" else {}), **(headers or {})})
            res = conn.getresponse()
            raw = res.read()
            is_json = res.getheader("Content-Type") == "application/json"
            return res.status, dict(res.getheaders()), json.loads(raw) if is_json else {}
        finally:
            conn.close()

    def login(self, password: str = PASSWORD, user: str = "admin") -> tuple[int, dict[str, str], dict[str, object]]:
        return self.call("POST", "/auth/login", {"username": user, "password": password})

    def cookie(self) -> dict[str, str]:
        status, headers, _ = self.login()
        assert status == 200
        return {"Cookie": headers["Set-Cookie"].split(";")[0]}


@pytest.fixture
def site_for(tmp_path: Path) -> Iterator[Callable[..., Site]]:
    servers: list[DeckWebServer] = []

    def make(**overrides: object) -> Site:
        settings = ServiceSettings("127.0.0.1", 1, tmp_path / "config.json", tmp_path / "images")
        service = HostService(settings)
        service.info = parse_info(INFO_PAYLOAD)
        auth = AuthSettings(enabled=True, username="admin", password_hash=VERIFIER, **overrides)  # type: ignore[arg-type]
        servers.append(start_web(service, "127.0.0.1", 0, auth=auth))
        return Site(servers[-1])

    yield make
    for server in servers:
        server.shutdown()
        server.server_close()


# --- verifier, settings, sessions, throttle ---------------------------------


def test_verifier_matches_the_esp32_format() -> None:
    rounds, salt, digest = parse_verifier(VERIFIER)
    assert digest == hashlib.pbkdf2_hmac("sha256", PASSWORD.encode(), salt, rounds)
    assert check_password(VERIFIER, PASSWORD)
    assert not check_password(VERIFIER, "wrong")
    for bad in ("", "x", VERIFIER.replace("10000", "9999"), VERIFIER.upper(), VERIFIER + "0"):
        with pytest.raises(ConfigError):
            parse_verifier(bad)


def test_settings_validation_and_file(tmp_path: Path) -> None:
    assert load_settings(tmp_path / "missing.json") == AuthSettings()
    good = {"enabled": True, "username": "a", "password_hash": VERIFIER, "public_origin": "https://Deck.Example/"}
    settings = parse_settings(good)
    assert (settings.public_origin, settings.cookie_name) == ("https://deck.example", "__Host-deck_session")
    assert parse_settings({**good, "public_origin": "http://h:1"}).cookie_name == "deck_session"
    bad_settings: list[object] = [
        [],
        {"nope": 1},
        {**good, "enabled": "yes"},
        {**good, "username": ""},
        {**good, "password_hash": "x"},
        {**good, "session_ttl_seconds": 5},
        {**good, "public_origin": "ftp://x"},
        {**good, "public_origin": "https://x/path"},
        {**good, "trusted_proxies": "10.0.0.1"},
    ]
    for bad in bad_settings:
        with pytest.raises(ConfigError):
            parse_settings(bad)
    path = tmp_path / "auth.json"
    save_settings(path, settings)
    assert path.stat().st_mode & 0o777 == 0o600
    assert load_settings(path) == settings
    path.chmod(0o644)
    with pytest.raises(ConfigError, match="chmod 600"):
        load_settings(path)


def test_cookie_parsing() -> None:
    token = "ab" * 32
    assert parse_cookie(f"x=1; deck_session={token}", "deck_session") == token
    assert parse_cookie(f"deck_session={token}; deck_session={token}", "deck_session") is None
    assert parse_cookie("deck_session=short", "deck_session") is None
    assert parse_cookie(f"deck_session={token.upper()}", "deck_session") is None
    assert parse_cookie("", "deck_session") is None


def test_session_store_expiry_revocation_and_capacity() -> None:
    store = SessionStore(60, capacity=2)
    first, second = store.create("a", "local:a"), store.create("b", "local:b")
    assert first != second
    assert store.lookup(first) is not None
    store.create("c", "local:c")  # the oldest makes room
    assert store.lookup(first) is None
    assert store.lookup(second) is not None
    store.revoke(second)
    assert store.lookup(second) is None
    assert store.lookup(None) is None
    short = SessionStore(60)
    token = short.create("a", "local:a")
    short.ttl_s = 0
    expired = short.create("a", "local:a")
    time.sleep(0.01)
    assert short.lookup(token) is not None
    assert short.lookup(expired) is None


def test_throttle_per_source() -> None:
    throttle = LoginThrottle()
    assert [throttle.attempt("a") for _ in range(PER_SOURCE)] == [0] * PER_SOURCE
    assert throttle.attempt("a") > 0
    assert throttle.attempt("b") == 0


# --- HTTP -----------------------------------------------------------------------


def test_public_session_and_protected_api(site_for: Callable[..., Site]) -> None:
    site = site_for()
    status, headers, body = site.call("GET", "/auth/session")
    assert (status, headers["Cache-Control"]) == (200, "no-store")
    assert body["required"] is True and body["authenticated"] is False and body["methods"] == ["password"]
    assert site.call("GET", "/api/info")[0] == 401
    assert site.call("GET", "/api/events")[0] == 401
    assert site.call("GET", "/api/pages/root/keys/0/image")[0] == 401
    assert site.call("PUT", "/api/config", {})[0] == 401
    assert site.call("GET", "/")[0] == 200  # the login shell is public


def test_login_logout(site_for: Callable[..., Site]) -> None:
    site = site_for()
    status, headers, body = site.login()
    assert status == 200 and body["authenticated"] is True and body["username"] == "admin"
    cookie = headers["Set-Cookie"]
    assert cookie.startswith("deck_session=") and "HttpOnly" in cookie and "SameSite=Lax" in cookie
    assert "Max-Age=172800" in cookie and "Secure" not in cookie
    auth = {"Cookie": cookie.split(";")[0]}
    assert site.call("GET", "/api/info", headers=auth)[0] == 200
    assert site.call("GET", "/auth/session", headers=auth)[2]["authenticated"] is True
    status, headers, _ = site.call("POST", "/auth/logout", {}, auth)
    assert status == 200 and "Max-Age=0" in headers["Set-Cookie"]
    assert site.call("GET", "/api/info", headers=auth)[0] == 401


def test_login_failures(site_for: Callable[..., Site]) -> None:
    site = site_for()
    wrong_password = site.login("nope")
    wrong_user = site.login(PASSWORD, "root")
    assert wrong_password[0] == wrong_user[0] == 401
    assert wrong_password[2] == wrong_user[2] == {"error": "invalid username or password"}
    assert "Set-Cookie" not in wrong_password[1]
    assert site.call("POST", "/auth/login", {"username": "admin"})[0] == 400
    assert site.call("POST", "/auth/login", {"username": "admin", "password": "x", "extra": 1})[0] == 400
    assert site.call("POST", "/auth/login", {"username": "admin", "password": "x" * 129})[0] == 400
    assert site.call("POST", "/auth/login", {}, {"X-Deck-Request": ""})[0] == 403
    assert site.call("GET", "/auth/login")[0] == 405


def test_login_is_throttled(site_for: Callable[..., Site]) -> None:
    site = site_for()
    for _ in range(PER_SOURCE):
        assert site.login("nope")[0] == 401
    status, headers, _ = site.login()
    assert status == 429 and int(headers["Retry-After"]) > 0


def test_forwarded_for_only_from_trusted_proxy(site_for: Callable[..., Site]) -> None:
    ignored = site_for()
    for i in range(PER_SOURCE + 1):
        status = ignored.call(
            "POST", "/auth/login", {"username": "a", "password": "b"}, {"X-Forwarded-For": f"9.9.9.{i}"}
        )[0]
    assert status == 429  # spoofed addresses do not give a fresh budget
    trusted = site_for(trusted_proxies=("127.0.0.1",))
    for i in range(PER_SOURCE + 1):
        status = trusted.call(
            "POST", "/auth/login", {"username": "a", "password": "b"}, {"X-Forwarded-For": f"9.9.9.{i}"}
        )[0]
    assert status == 401


def test_origin_and_public_host(site_for: Callable[..., Site]) -> None:
    site = site_for()
    auth = site.cookie()
    host = f"127.0.0.1:{site.port}"
    assert site.call("POST", "/api/page", {"page": "root"}, {**auth, "Origin": "https://evil.example"})[0] == 403
    assert site.call("POST", "/api/page", {"page": "root"}, {**auth, "Origin": f"http://{host}"})[0] == 200
    proxied = site_for(public_origin="https://deck.example")
    assert proxied.call("GET", "/auth/session", headers={"Host": "deck.example"})[0] == 200
    assert proxied.call("GET", "/auth/session", headers={"Host": "other.example"})[0] == 403
    status, headers, _ = proxied.call(
        "POST",
        "/auth/login",
        {"username": "admin", "password": PASSWORD},
        {"Host": "deck.example", "Origin": "https://deck.example"},
    )
    assert (
        status == 200 and headers["Set-Cookie"].startswith("__Host-deck_session=") and "Secure" in headers["Set-Cookie"]
    )
    refused = proxied.call(
        "POST",
        "/auth/login",
        {"username": "admin", "password": PASSWORD},
        {"Host": "deck.example", "Origin": "http://deck.example"},
    )
    assert refused[0] == 403


def test_event_stream_ends_with_logout(site_for: Callable[..., Site]) -> None:
    site = site_for()
    auth = site.cookie()
    conn = http.client.HTTPConnection("127.0.0.1", site.port, timeout=5)
    try:
        conn.request("GET", "/api/events", headers=auth)
        res = conn.getresponse()
        assert res.status == 200 and res.readline() == b"event: status\n"
        assert site.call("POST", "/auth/logout", {}, auth)[0] == 200
        deadline = time.monotonic() + 4
        while res.read(4096) and time.monotonic() < deadline:
            pass
        assert res.read(1) == b""
    finally:
        conn.close()


def test_without_auth_nothing_changes(tmp_path: Path) -> None:
    service = HostService(ServiceSettings("127.0.0.1", 1, tmp_path / "c.json", tmp_path / "i"))
    service.info = parse_info(INFO_PAYLOAD)
    server = start_web(service, "127.0.0.1", 0)
    try:
        site = Site(server)
        status, _, body = site.call("GET", "/auth/session")
        assert status == 200 and body["required"] is False and body["authenticated"] is True and body["methods"] == []
        assert site.call("GET", "/api/info")[0] == 200
        assert site.call("POST", "/auth/login", {})[0] == 404
    finally:
        server.shutdown()
        server.server_close()


def test_explicit_missing_auth_file_is_an_error(tmp_path: Path) -> None:
    with pytest.raises(ConfigError, match="does not exist"):
        load_settings(tmp_path / "typo.json", must_exist=True)


def test_dotted_client_id_is_one_key_in_the_role_path() -> None:
    from d200_deck.web.auth.oidc import claim_path  # noqa: PLC0415

    claims = {"resource_access": {"deck.home": {"roles": ["deck-admin"]}}}
    assert claim_path(claims, "resource_access.{client_id}.roles", "deck.home") == ["deck-admin"]


def test_sessions_carry_distinct_identities() -> None:
    store = SessionStore(60)
    local, oidc = store.create("admin", "local:admin"), store.create("admin", "oidc:https://i#sub")
    session_local, session_oidc = store.lookup(local), store.lookup(oidc)
    assert session_local and session_oidc and session_local.identity != session_oidc.identity

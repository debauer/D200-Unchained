from __future__ import annotations

import http.client
import json
import socket
import urllib.parse
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import pytest

from d200_deck.device.protocol import parse_info
from d200_deck.errors import ConfigError
from d200_deck.service.host import HostService, ServiceSettings
from d200_deck.web.auth.oidc import JWKS_MIN_REFRESH_S
from d200_deck.web.auth.settings import AuthSettings, OidcSettings, parse_settings
from d200_deck.web.server import DeckWebServer, start_web

from .conftest import INFO_PAYLOAD
from .idp import CLIENT_ID, FakeIdp

MUTATE = {"X-Deck-Request": "1", "Content-Type": "application/json"}


def cookies_of(headers: list[tuple[str, str]]) -> dict[str, str]:
    jar: dict[str, str] = {}
    for name, value in headers:
        if name.lower() == "set-cookie":
            pair = value.split(";")[0]
            key, _, val = pair.partition("=")
            if "Max-Age=0" in value:
                jar.pop(key, None)
            else:
                jar[key] = val
    return jar


class Browser:
    """Cookies and manual redirects, like the login in a real browser."""

    def __init__(self, port: int) -> None:
        self.port = port
        self.jar: dict[str, str] = {}

    def call(
        self, method: str, path: str, body: dict[str, Any] | None = None
    ) -> tuple[int, dict[str, str], dict[str, Any]]:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            headers = dict(MUTATE) if method != "GET" else {}
            if self.jar:
                headers["Cookie"] = "; ".join(f"{k}={v}" for k, v in self.jar.items())
            conn.request(method, path, body=json.dumps(body).encode() if body is not None else None, headers=headers)
            res = conn.getresponse()
            raw = res.read()
            self.jar.update(cookies_of(res.getheaders()))
            for name, value in res.getheaders():  # a cleared cookie leaves the jar
                if name.lower() == "set-cookie" and "Max-Age=0" in value:
                    self.jar.pop(value.split("=")[0], None)
            is_json = res.getheader("Content-Type") == "application/json"
            return res.status, dict(res.getheaders()), json.loads(raw) if is_json and raw else {}
        finally:
            conn.close()

    def login(self, idp: FakeIdp, user: str = "alice") -> tuple[int, str]:
        status, _, body = self.call("POST", "/auth/oidc/start", {})
        assert status == 200
        code, state = idp.authorize(body["redirect"], user)
        status, headers, _ = self.call("GET", f"/auth/oidc/callback?code={code}&state={state}")
        return status, headers["Location"]


@pytest.fixture
def idp() -> Iterator[FakeIdp]:
    provider = FakeIdp()
    yield provider
    provider.close()


@pytest.fixture
def site_for(tmp_path: Path, idp: FakeIdp) -> Iterator[Callable[..., Browser]]:
    servers: list[DeckWebServer] = []

    def make(local: bool = True, **oidc: Any) -> Browser:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        service = HostService(ServiceSettings("127.0.0.1", 1, tmp_path / "config.json", tmp_path / "images"))
        service.info = parse_info(INFO_PAYLOAD)
        auth = AuthSettings(
            enabled=True,
            username="admin" if local else "",
            password_hash="pbkdf2_sha256$10000$" + "00" * 16 + "$" + "00" * 32 if local else "",
            public_origin=f"http://127.0.0.1:{port}",
            oidc=OidcSettings(issuer=idp.issuer, client_id=CLIENT_ID, **oidc),
        )
        servers.append(start_web(service, "127.0.0.1", port, auth=auth))
        return Browser(port)

    yield make
    for server in servers:
        server.shutdown()
        server.server_close()


def test_login_with_the_required_role(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for()
    assert browser.call("GET", "/auth/session")[2]["methods"] == ["password", "oidc"]
    assert browser.call("GET", "/api/info")[0] == 401
    assert browser.login(idp) == (302, "/")
    assert "deck_oidc" not in browser.jar and "deck_session" in browser.jar
    assert browser.call("GET", "/api/info")[0] == 200
    session = browser.call("GET", "/auth/session")[2]
    assert session["authenticated"] is True and session["username"] == "alice"
    assert browser.call("POST", "/auth/logout", {})[0] == 200
    assert browser.call("GET", "/api/info")[0] == 401


def test_oidc_only_has_no_password_login(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for(local=False)
    assert browser.call("GET", "/auth/session")[2]["methods"] == ["oidc"]
    assert browser.call("POST", "/auth/login", {"username": "a", "password": "b"})[0] != 200
    assert browser.login(idp) == (302, "/")


@pytest.mark.parametrize(
    ("claims", "options", "expected"),
    [
        ({"resource_access": {CLIENT_ID: {"roles": ["viewer"]}}}, {}, "forbidden"),
        ({"resource_access": {}}, {}, "forbidden"),
        ({"resource_access": {"other": {"roles": ["deck-admin"]}}}, {}, "forbidden"),
        ({"resource_access": {}}, {"required_role": ""}, None),
        ({"resource_access": {CLIENT_ID: {"roles": ["operator"]}}}, {"required_role": "operator"}, None),
        ({"groups": ["ops"]}, {"required_role": "ops", "role_claim": "groups"}, None),
        ({"iss": "http://evil.example/realms/test"}, {}, "token"),
        ({"aud": "other"}, {}, "token"),
        ({"aud": [CLIENT_ID, "other"], "azp": "other"}, {}, "token"),
        ({"azp": "other"}, {}, "token"),
        ({"nonce": "wrong"}, {}, "token"),
        ({"exp": 1}, {}, "token"),
        ({"iat": 4102444800}, {}, "token"),
        ({"sub": None}, {}, "token"),
    ],
)
def test_id_token_and_role_policy(
    site_for: Callable[..., Browser],
    idp: FakeIdp,
    claims: dict[str, Any],
    options: dict[str, Any],
    expected: str | None,
) -> None:
    browser = site_for(**options)
    idp.claims = claims
    status, location = browser.login(idp)
    assert status == 302
    if expected is None:
        assert location == "/" and browser.call("GET", "/api/info")[0] == 200
    else:
        assert location == f"/?auth_error={expected}"
        assert "deck_session" not in browser.jar
        assert browser.call("GET", "/api/info")[0] == 401


def test_only_rs256_is_accepted(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for()
    idp.alg = "RS512"
    assert browser.login(idp)[1] == "/?auth_error=token"


def test_foreign_signature_is_rejected(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for()
    assert browser.login(idp)[1] == "/"  # keys are now cached
    other = FakeIdp()
    try:
        other.key = idp.key.generate_key(2048, parameters={"kid": "k1"})  # same kid, different key
        idp.key = other.key
        assert Browser(browser.port).login(idp)[1] == "/?auth_error=token"
    finally:
        other.close()


def test_callback_is_bound_to_the_browser_and_used_once(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for()
    _, _, body = browser.call("POST", "/auth/oidc/start", {})
    code, state = idp.authorize(body["redirect"])
    foreign = Browser(browser.port)  # another browser without the transaction cookie
    assert (
        foreign.call("GET", f"/auth/oidc/callback?code={code}&state={state}")[1]["Location"] == "/?auth_error=expired"
    )
    wrong_state = browser.call("GET", f"/auth/oidc/callback?code={code}&state=forged")
    assert wrong_state[1]["Location"] == "/?auth_error=expired"
    # the transaction is gone, even for its own browser
    assert (
        browser.call("GET", f"/auth/oidc/callback?code={code}&state={state}")[1]["Location"] == "/?auth_error=expired"
    )
    assert "deck_session" not in browser.jar


def test_replayed_callback_and_provider_error(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for()
    _, _, body = browser.call("POST", "/auth/oidc/start", {})
    code, state = idp.authorize(body["redirect"])
    jar = dict(browser.jar)
    assert browser.call("GET", f"/auth/oidc/callback?code={code}&state={state}")[1]["Location"] == "/"
    browser.jar = jar  # a replay with the old transaction cookie
    assert (
        browser.call("GET", f"/auth/oidc/callback?code={code}&state={state}")[1]["Location"] == "/?auth_error=expired"
    )
    _, _, body = browser.call("POST", "/auth/oidc/start", {})
    state = urllib.parse.parse_qs(urllib.parse.urlsplit(body["redirect"]).query)["state"][0]
    denied = browser.call("GET", f"/auth/oidc/callback?error=access_denied&state={state}")
    assert denied[1]["Location"] == "/?auth_error=denied"


def test_authorization_request_uses_pkce_and_exact_callback(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for()
    body = browser.call("POST", "/auth/oidc/start", {})[2]
    query = {k: v[0] for k, v in urllib.parse.parse_qs(urllib.parse.urlsplit(body["redirect"]).query).items()}
    assert query["code_challenge_method"] == "S256" and len(query["code_challenge"]) == 43
    assert query["redirect_uri"] == f"http://127.0.0.1:{browser.port}/auth/oidc/callback"
    assert query["client_id"] == CLIENT_ID and query["scope"] == "openid"
    assert {"state", "nonce"} <= set(query)


def test_provider_outage_does_not_break_local_login(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for()
    idp.down = True
    assert browser.call("POST", "/auth/oidc/start", {})[0] == 502
    assert browser.call("GET", "/auth/session")[0] == 200


def test_key_rotation_refetches_once_and_is_rate_limited(site_for: Callable[..., Browser], idp: FakeIdp) -> None:
    browser = site_for()
    assert browser.login(idp)[1] == "/" and idp.jwks_requests == 1
    idp.rotate("k2")
    assert Browser(browser.port).login(idp)[1] == "/"  # unknown kid: keys are fetched again
    assert idp.jwks_requests == 2
    idp.rotate("k3")
    assert Browser(browser.port).login(idp)[1] == "/?auth_error=token"  # too soon for another fetch
    assert idp.jwks_requests == 2
    assert JWKS_MIN_REFRESH_S > 0


def test_oidc_settings_validation() -> None:
    good = {
        "enabled": True,
        "public_origin": "https://deck.example",
        "oidc": {"issuer": "https://id/realms/x/", "client_id": "deck"},
    }
    settings = parse_settings(good)
    assert settings.oidc == OidcSettings("https://id/realms/x", "deck")
    assert settings.password_login is False
    open_to_realm = parse_settings(
        {**good, "oidc": {"issuer": "https://id/realms/x", "client_id": "d", "required_role": ""}}
    )
    assert open_to_realm.oidc is not None and open_to_realm.oidc.required_role == ""
    for bad in (
        {**good, "oidc": {"issuer": "https://id", "client_id": ""}},
        {**good, "oidc": {"issuer": "id.example", "client_id": "deck"}},
        {**good, "oidc": {"issuer": "https://id", "client_id": "deck", "extra": 1}},
        {**good, "oidc": []},
        {**{k: v for k, v in good.items() if k != "public_origin"}},
        {**good, "username": "admin"},  # local login half configured
    ):
        with pytest.raises(ConfigError):
            parse_settings(bad)

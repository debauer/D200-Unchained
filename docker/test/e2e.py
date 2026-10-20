#!/usr/bin/env python3
"""Login test against the running test stack: local login, Keycloak login with and without the role.

Plain standard library. The names deck.localtest.me and keycloak.localtest.me are mapped to 127.0.0.1 here,
so no DNS or /etc/hosts entry is needed. Usage: e2e.py [--ca out/certs/ca.pem]"""

from __future__ import annotations

import argparse
import html
import http.cookiejar
import json
import re
import socket
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request

DECK = "https://deck.localtest.me:8443"
LOCAL_USER, LOCAL_PASSWORD = "admin", "local-test-password"
ALICE = ("alice", "alice-test-password")  # has the client role deck-admin
BOB = ("bob", "bob-test-password")  # a realm user without it

_getaddrinfo = socket.getaddrinfo


def _local_names(host: str, *args: object, **kwargs: object) -> list[tuple[object, ...]]:
    return _getaddrinfo("127.0.0.1" if host.endswith(".localtest.me") else host, *args, **kwargs)  # type: ignore[arg-type]


socket.getaddrinfo = _local_names  # type: ignore[assignment]


class Browser:
    def __init__(self, ca: str) -> None:
        context = ssl.create_default_context(cafile=ca)
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar), urllib.request.HTTPSHandler(context=context)
        )

    def call(
        self, url: str, data: bytes | None = None, headers: dict[str, str] | None = None, method: str | None = None
    ) -> tuple[int, str, str]:
        """(status, final URL after redirects, body); error statuses are returned, not raised."""
        request = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
        try:
            with self.opener.open(request, timeout=30) as res:
                return res.status, res.geturl(), res.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.geturl(), e.read().decode()

    def json(self, path: str, body: dict[str, object] | None = None) -> tuple[int, dict[str, object]]:
        headers = {"X-Deck-Request": "1", "Content-Type": "application/json", "Origin": DECK}
        data = json.dumps(body).encode() if body is not None else None
        status, _, text = self.call(DECK + path, data, headers if body is not None else None)
        return status, json.loads(text) if text.startswith("{") else {}

    def keycloak_login(self, user: str, password: str) -> str:
        """Start the OIDC login, fill in Keycloak's form, follow the redirects. Returns the final URL."""
        status, start = self.json("/auth/oidc/start", {})
        assert status == 200, f"oidc start: {status} {start}"
        _, url, page = self.call(str(start["redirect"]))
        action = re.search(r'<form[^>]+id="kc-form-login"[^>]+action="([^"]+)"', page)
        assert action, f"no Keycloak login form at {url}"
        form = urllib.parse.urlencode({"username": user, "password": password, "credentialId": ""}).encode()
        _, final, _ = self.call(html.unescape(action.group(1)), form)
        return final


def check(condition: bool, what: str) -> None:
    print(("PASS " if condition else "FAIL ") + what)
    if not condition:
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ca", default="out/certs/ca.pem")
    ca = parser.parse_args().ca

    browser = Browser(ca)
    status, session = browser.json("/auth/session")
    check(status == 200 and session["required"] is True, "auth is required")
    check(session["methods"] == ["password", "oidc"], f"host offers password and oidc: {session['methods']}")
    check(browser.json("/api/config")[0] == 401, "API refuses without a session")

    check(browser.json("/auth/login", {"username": LOCAL_USER, "password": "wrong"})[0] == 401, "wrong password refused")
    status, session = browser.json("/auth/login", {"username": LOCAL_USER, "password": LOCAL_PASSWORD})
    check(status == 200 and session["username"] == LOCAL_USER, "local login works")
    check(browser.json("/api/config")[0] == 200, "API works with the local session")
    check(browser.json("/auth/logout", {})[0] == 200, "logout")
    check(browser.json("/api/config")[0] == 401, "API refuses after logout")

    final = browser.keycloak_login(*ALICE)
    check(final == DECK + "/", f"keycloak login with the role ends on the UI: {final}")
    status, session = browser.json("/auth/session")
    check(session["authenticated"] is True and session["username"] == ALICE[0], "session belongs to alice")
    check(browser.json("/api/config")[0] == 200, "API works with the OIDC session")
    browser.json("/auth/logout", {})

    browser = Browser(ca)
    final = browser.keycloak_login(*BOB)
    check("auth_error=forbidden" in final, f"user without deck-admin is refused: {final}")
    check(browser.json("/api/config")[0] == 401, "refused user has no session")
    print("all checks passed")


if __name__ == "__main__":
    main()

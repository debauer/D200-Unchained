"""Request-level authentication: `/auth/*` endpoints and the guard in front of `/api/*`."""

from __future__ import annotations

import json
import secrets
import time
from http import HTTPStatus
from typing import TYPE_CHECKING, Any

from d200_deck.web.apierror import ApiError
from d200_deck.web.auth.oidc_login import CALLBACK_PATH, OidcLogin
from d200_deck.web.auth.sessions import Session, SessionStore, parse_cookie
from d200_deck.web.auth.settings import AuthSettings
from d200_deck.web.auth.throttle import LoginThrottle
from d200_deck.web.auth.verifier import check_password

if TYPE_CHECKING:
    from d200_deck.web.server import DeckHandler

MAX_LOGIN_BODY = 512
MAX_PASSWORD = 128
# Compared against when the user name is wrong, so the response time does not reveal valid names.
_DUMMY_VERIFIER = "pbkdf2_sha256$120000$" + "00" * 16 + "$" + "00" * 32


class AuthGate:
    def __init__(self, settings: AuthSettings) -> None:
        self.settings = settings
        self.store = SessionStore(settings.session_ttl_seconds)
        self.throttle = LoginThrottle()
        self.oidc = OidcLogin(settings, self.store) if settings.enabled and settings.oidc else None

    @property
    def enabled(self) -> bool:
        return self.settings.enabled

    def token(self, handler: DeckHandler) -> str | None:
        return parse_cookie(handler.headers.get("Cookie", ""), self.settings.cookie_name)

    def session(self, handler: DeckHandler) -> Session | None:
        return self.store.lookup(self.token(handler))

    def guard(self, handler: DeckHandler, path: str) -> None:
        if self.enabled and path.startswith("/api/") and self.session(handler) is None:
            raise ApiError(HTTPStatus.UNAUTHORIZED, "authentication required")

    def source(self, handler: DeckHandler) -> str:
        peer = str(handler.client_address[0])
        forwarded = handler.headers.get("X-Forwarded-For", "")
        if peer in self.settings.trusted_proxies and forwarded:
            return forwarded.split(",")[-1].strip()
        return peer

    def handle(self, handler: DeckHandler, method: str, path: str) -> None:
        """Serve `/auth/*`; `/auth/session` is public, the rest needs authentication to be enabled."""
        if path == "/auth/session" and method == "GET":
            self._status(handler, self.session(handler))
        elif not self.enabled:
            raise ApiError(HTTPStatus.NOT_FOUND, "authentication disabled")
        elif path == "/auth/login" and method == "POST" and self.settings.password_login:
            self._login(handler)
        elif self.oidc and path == "/auth/oidc/start" and method == "POST":
            self.oidc.start(handler, self.source(handler))
        elif self.oidc and path == CALLBACK_PATH and method == "GET":
            self.oidc.callback(handler)
        elif path == "/auth/logout" and method == "POST":
            self.store.revoke(self.token(handler))
            handler.json_response(HTTPStatus.OK, {"logged_out": True}, {"Set-Cookie": self._cookie("", 0)})
        elif path in ("/auth/session", "/auth/login", "/auth/logout"):
            raise ApiError(HTTPStatus.METHOD_NOT_ALLOWED, f"{method} not allowed on {path}")
        else:
            raise ApiError(HTTPStatus.NOT_FOUND, f"no endpoint {method} {path}")

    def status_json(self, session: Session | None) -> dict[str, Any]:
        left = max(0, int(session.expires - time.monotonic()) + 1) if session else 0
        return {
            "required": self.enabled,
            "authenticated": not self.enabled or session is not None,
            "session_ttl_seconds": self.settings.session_ttl_seconds,
            "expires_in": left,
            "methods": [m for m, on in (("password", self.settings.password_login), ("oidc", self.oidc)) if on],
            "username": session.username if session else None,
            "identity": session.identity if session else None,
        }

    def _status(self, handler: DeckHandler, session: Session | None) -> None:
        handler.json_response(HTTPStatus.OK, self.status_json(session))

    def _cookie(self, value: str, max_age: int) -> str:
        secure = "; Secure" if self.settings.secure else ""
        return f"{self.settings.cookie_name}={value}; Path=/; HttpOnly; SameSite=Lax; Max-Age={max_age}{secure}"

    def _credentials(self, handler: DeckHandler) -> tuple[str, str]:
        try:
            data = json.loads(handler.read_body(MAX_LOGIN_BODY))
        except ValueError as e:
            raise ApiError(HTTPStatus.BAD_REQUEST, "expected username and password") from e
        user = data.get("username") if isinstance(data, dict) else None
        password = data.get("password") if isinstance(data, dict) else None
        if not (isinstance(user, str) and isinstance(password, str) and len(data) == 2) or len(password) > MAX_PASSWORD:
            raise ApiError(HTTPStatus.BAD_REQUEST, "expected username and password")
        return user, password

    def _login(self, handler: DeckHandler) -> None:
        retry = self.throttle.attempt(self.source(handler))
        if retry:
            raise ApiError(HTTPStatus.TOO_MANY_REQUESTS, "too many login attempts", {"Retry-After": str(retry)})
        user, password = self._credentials(handler)
        user_ok = secrets.compare_digest(user.encode(), self.settings.username.encode())
        password_ok = check_password(self.settings.password_hash if user_ok else _DUMMY_VERIFIER, password)
        if not (user_ok and password_ok):
            raise ApiError(HTTPStatus.UNAUTHORIZED, "invalid username or password")
        self.store.revoke(self.token(handler))
        token = self.store.create(self.settings.username, f"local:{self.settings.username}")
        session = self.store.lookup(token)
        handler.json_response(
            HTTPStatus.OK,
            self.status_json(session),
            {"Set-Cookie": self._cookie(token, self.settings.session_ttl_seconds)},
        )

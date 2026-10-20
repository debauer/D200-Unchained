"""The browser side of the OIDC login: `POST /auth/oidc/start` and `GET /auth/oidc/callback`.

A transaction (state, nonce, PKCE verifier) lives in RAM, is bound to the browser by a short-lived cookie,
and is used once. The callback ends in a redirect to the UI; failures carry only a generic `auth_error` code."""

from __future__ import annotations

import base64
import hashlib
import hmac
import logging
import secrets
import threading
import time
import urllib.parse
from http import HTTPStatus
from typing import TYPE_CHECKING

from d200_deck.errors import DeckError
from d200_deck.web.apierror import ApiError
from d200_deck.web.auth.oidc import OidcClient, OidcError
from d200_deck.web.auth.sessions import SessionStore, parse_cookie
from d200_deck.web.auth.settings import AuthSettings
from d200_deck.web.auth.throttle import LoginThrottle

if TYPE_CHECKING:
    from d200_deck.web.server import DeckHandler

log = logging.getLogger(__name__)

TRANSACTION_TTL_S = 600
MAX_TRANSACTIONS = 32
CALLBACK_PATH = "/auth/oidc/callback"


class Transactions:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._open: dict[str, tuple[float, str, str, str]] = {}  # id -> expiry, state, nonce, verifier

    def create(self, state: str, nonce: str, verifier: str) -> str:
        now = time.monotonic()
        tx_id = secrets.token_hex(32)
        with self._lock:
            self._open = {k: v for k, v in self._open.items() if v[0] > now}
            while len(self._open) >= MAX_TRANSACTIONS:
                del self._open[min(self._open, key=lambda k: self._open[k][0])]
            self._open[tx_id] = (now + TRANSACTION_TTL_S, state, nonce, verifier)
        return tx_id

    def take(self, tx_id: str | None) -> tuple[str, str, str] | None:
        """The transaction, removed: a callback can only be replayed once."""
        with self._lock:
            entry = self._open.pop(tx_id, None) if tx_id else None
        if entry is None or entry[0] <= time.monotonic():
            return None
        return entry[1:]


class OidcLogin:
    def __init__(self, settings: AuthSettings, store: SessionStore, client: OidcClient | None = None) -> None:
        assert settings.oidc is not None
        self.settings = settings
        self.store = store
        self.client = client or OidcClient(settings.oidc, settings.public_origin + CALLBACK_PATH)
        self.transactions = Transactions()
        self.throttle = LoginThrottle()
        prefix = "__Host-" if settings.secure else ""
        self.cookie_name = f"{prefix}deck_oidc"

    def _cookie(self, name: str, value: str, max_age: int) -> str:
        secure = "; Secure" if self.settings.secure else ""
        return f"{name}={value}; Path=/; HttpOnly; SameSite=Lax; Max-Age={max_age}{secure}"

    def start(self, handler: DeckHandler, source: str) -> None:
        retry = self.throttle.attempt(source)
        if retry:
            raise ApiError(HTTPStatus.TOO_MANY_REQUESTS, "too many login attempts", {"Retry-After": str(retry)})
        state, nonce, verifier = (secrets.token_urlsafe(32), secrets.token_urlsafe(32), secrets.token_urlsafe(64))
        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
        try:
            url = self.client.authorization_url(state, nonce, challenge)
        except OidcError as e:
            log.warning("OIDC start failed: %s", e)
            raise ApiError(HTTPStatus.BAD_GATEWAY, "identity provider not reachable") from e
        tx_id = self.transactions.create(state, nonce, verifier)
        cookie = self._cookie(self.cookie_name, tx_id, TRANSACTION_TTL_S)
        handler.json_response(HTTPStatus.OK, {"redirect": url}, {"Set-Cookie": cookie})

    def callback(self, handler: DeckHandler) -> None:
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(handler.path).query)
        clear = self._cookie(self.cookie_name, "", 0)
        cookies: tuple[str, ...] = (clear,)
        try:
            token = self._finish(handler, query)
            session_cookie = self._session_cookie(token)
            handler.redirect("/", {"Set-Cookie": (clear, session_cookie)})
        except OidcError as e:
            log.warning("OIDC login refused (%s): %s", e.code, e)
            handler.redirect(f"/?auth_error={e.code}", {"Set-Cookie": cookies})

    def _finish(self, handler: DeckHandler, query: dict[str, list[str]]) -> str:
        tx = self.transactions.take(parse_cookie(handler.headers.get("Cookie", ""), self.cookie_name))
        state = query.get("state", [""])[0]
        if tx is None or not hmac.compare_digest(tx[0].encode(), state.encode()):
            raise OidcError("expired", "unknown, expired or foreign login transaction")
        if "error" in query:
            raise OidcError("denied", f"provider returned {query['error'][0][:40]!r}")
        code = query.get("code", [""])[0]
        if not code:
            raise OidcError("token", "callback without code")
        try:
            claims = self.client.verify(self.client.exchange(code, tx[2]), tx[1])
            username, identity = self.client.identity(claims)
        except OidcError:
            raise
        except DeckError as e:
            raise OidcError("provider", str(e)) from e
        self.store.revoke(parse_cookie(handler.headers.get("Cookie", ""), self.settings.cookie_name))
        return self.store.create(username, identity)

    def _session_cookie(self, token: str) -> str:
        return self._cookie(self.settings.cookie_name, token, self.settings.session_ttl_seconds)

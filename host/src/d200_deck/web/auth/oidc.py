"""OpenID Connect client for Keycloak: discovery, code exchange and ID token verification.

Public client, Authorization Code with PKCE; only RS256 ID tokens are accepted. Needs the `oidc` extra
(joserfc, cryptography), which is imported when the first provider request is made."""

from __future__ import annotations

import base64
import importlib.util
import json
import ssl
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

from d200_deck.errors import ConfigError, DeckError
from d200_deck.web.auth.settings import AuthSettings, OidcSettings

TIMEOUT_S = 5.0
MAX_RESPONSE = 256 * 1024
DISCOVERY_TTL_S = 3600.0
JWKS_TTL_S = 3600.0
JWKS_MIN_REFRESH_S = 30.0  # unknown key ids cannot make the host hammer the provider
MAX_KEYS = 16
LEEWAY_S = 60
ALGORITHM = "RS256"


class OidcError(DeckError):
    """Login through the provider failed; `code` is the generic reason shown to the browser."""

    def __init__(self, code: str, detail: str) -> None:
        super().__init__(detail)
        self.code = code


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_args: Any, **_kwargs: Any) -> None:
        return None


def _segment(token: str, index: int) -> dict[str, Any]:
    try:
        raw = token.split(".")[index]
        value = json.loads(base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4)))
    except (IndexError, ValueError) as e:
        raise OidcError("token", "malformed ID token") from e
    if not isinstance(value, dict):
        raise OidcError("token", "malformed ID token")
    return value


def check_oidc_dependencies(settings: AuthSettings) -> None:
    """Fail at startup, not in the callback, when OIDC is on but the `oidc` extra is not installed."""
    if settings.enabled and settings.oidc and importlib.util.find_spec("joserfc") is None:
        raise ConfigError("auth: oidc needs the optional dependency joserfc: pip install 'd200-deck[oidc]'")


def claim_path(claims: dict[str, Any], path: str, client_id: str = "") -> Any:
    """Follow a dotted path; `{client_id}` stands for one whole key, even if the id itself holds dots."""
    value: Any = claims
    for part in (client_id if p == "{client_id}" else p for p in path.split(".")):
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return value


class OidcClient:
    def __init__(self, settings: OidcSettings, redirect_uri: str) -> None:
        self.settings = settings
        self.redirect_uri = redirect_uri
        self._lock = threading.Lock()
        self._discovery: tuple[float, dict[str, str]] | None = None
        self._jwks: tuple[float, float, Any] | None = None  # fetched at, last forced refresh, key set
        context = ssl.create_default_context(cafile=settings.ca_file or None)
        self._opener = urllib.request.build_opener(_NoRedirect, urllib.request.HTTPSHandler(context=context))

    # --- provider requests ---------------------------------------------------

    def _request(self, url: str, data: dict[str, str] | None = None) -> dict[str, Any]:
        body = urllib.parse.urlencode(data).encode() if data is not None else None
        request = urllib.request.Request(url, data=body, headers={"Accept": "application/json"})
        try:
            with self._opener.open(request, timeout=TIMEOUT_S) as res:
                raw = res.read(MAX_RESPONSE + 1)
        except urllib.error.HTTPError as e:
            raise OidcError("token" if data is not None else "provider", f"{url}: HTTP {e.code}") from e
        except (OSError, ValueError) as e:  # URLError, TLS and timeouts are OSErrors
            raise OidcError("provider", f"{url}: {e}") from e
        if len(raw) > MAX_RESPONSE:
            raise OidcError("provider", f"{url}: response too large")
        try:
            value = json.loads(raw)
        except ValueError as e:
            raise OidcError("provider", f"{url}: not JSON") from e
        if not isinstance(value, dict):
            raise OidcError("provider", f"{url}: unexpected response")
        return value

    def endpoints(self) -> dict[str, str]:
        with self._lock:
            if self._discovery and time.monotonic() - self._discovery[0] < DISCOVERY_TTL_S:
                return self._discovery[1]
        doc = self._request(f"{self.settings.issuer}/.well-known/openid-configuration")
        scheme = urllib.parse.urlsplit(self.settings.issuer).scheme
        found = {k: doc.get(k) for k in ("issuer", "authorization_endpoint", "token_endpoint", "jwks_uri")}
        if found["issuer"] != self.settings.issuer:
            raise OidcError("provider", "discovery issuer does not match the configured issuer")
        for name, url in found.items():
            if not isinstance(url, str) or urllib.parse.urlsplit(url).scheme != scheme:
                raise OidcError("provider", f"discovery {name} missing or not {scheme}")
        endpoints = {k: str(v) for k, v in found.items()}
        with self._lock:
            self._discovery = (time.monotonic(), endpoints)
        return endpoints

    def _key_set(self, kid: str | None) -> Any:
        """The provider's signing keys; refetched once (rate limited) when `kid` is not known."""
        from joserfc.jwk import KeySet  # noqa: PLC0415 - optional dependency

        with self._lock:
            cached = self._jwks
        now = time.monotonic()
        known = cached is not None and (kid is None or any(k.get("kid") == kid for k in cached[2]["keys"]))
        stale = cached is None or now - cached[0] >= JWKS_TTL_S
        forced = not known and (cached is None or now - cached[1] >= JWKS_MIN_REFRESH_S)
        if cached is None or stale or forced:
            doc = self._request(self.endpoints()["jwks_uri"])
            keys = [k for k in doc.get("keys", []) if isinstance(k, dict) and k.get("kty") == "RSA"]
            if not keys or len(keys) > MAX_KEYS:
                raise OidcError("provider", "jwks has no usable RSA keys or too many keys")
            last_forced = cached[1] if cached else float("-inf")  # the first fetch does not use up the budget
            cached = (now, now if forced and cached else last_forced, {"keys": keys})
            with self._lock:
                self._jwks = cached
        serialized: Any = cached[2]
        return KeySet.import_key_set(serialized)

    # --- the flow ---------------------------------------------------------------

    def authorization_url(self, state: str, nonce: str, challenge: str) -> str:
        query = urllib.parse.urlencode(
            {
                "response_type": "code",
                "client_id": self.settings.client_id,
                "redirect_uri": self.redirect_uri,
                "scope": self.settings.scopes,
                "state": state,
                "nonce": nonce,
                "code_challenge": challenge,
                "code_challenge_method": "S256",
            }
        )
        return f"{self.endpoints()['authorization_endpoint']}?{query}"

    def exchange(self, code: str, verifier: str) -> str:
        reply = self._request(
            self.endpoints()["token_endpoint"],
            {
                "grant_type": "authorization_code",
                "code": code,
                "redirect_uri": self.redirect_uri,
                "client_id": self.settings.client_id,
                "code_verifier": verifier,
            },
        )
        token = reply.get("id_token")
        if not isinstance(token, str):
            raise OidcError("token", "token response has no id_token")
        return token

    def verify(self, token: str, nonce: str) -> dict[str, Any]:
        """The claims of a valid ID token for this client; anything else raises OidcError."""
        from joserfc import jwt  # noqa: PLC0415 - optional dependency
        from joserfc.errors import JoseError  # noqa: PLC0415

        header = _segment(token, 0)
        if header.get("alg") != ALGORITHM:
            raise OidcError("token", f"ID token algorithm must be {ALGORITHM}")
        kid = header.get("kid")
        try:
            decoded = jwt.decode(token, self._key_set(kid if isinstance(kid, str) else None), algorithms=[ALGORITHM])
            registry = jwt.JWTClaimsRegistry(
                now=int(time.time()),
                leeway=LEEWAY_S,
                iss={"essential": True, "value": self.settings.issuer},
                aud={"essential": True, "value": self.settings.client_id},
                sub={"essential": True},
                exp={"essential": True},
                iat={"essential": True},
                nonce={"essential": True, "value": nonce},
            )
            registry.validate(decoded.claims)
        except JoseError as e:
            raise OidcError("token", f"ID token rejected: {e}") from e
        claims: dict[str, Any] = decoded.claims
        azp = claims.get("azp")
        audience = claims["aud"]
        if (azp is not None or (isinstance(audience, list) and len(audience) > 1)) and azp != self.settings.client_id:
            raise OidcError("token", "ID token azp does not match the client")
        return claims

    def identity(self, claims: dict[str, Any]) -> tuple[str, str]:
        """(user name, stable identity from issuer and sub) after the role check (`required_role`; empty: none)."""
        role = self.settings.required_role
        if role:
            roles = claim_path(claims, self.settings.role_claim, self.settings.client_id)
            if not isinstance(roles, list) or role not in roles:
                raise OidcError("forbidden", f"user lacks the role {role!r}")
        name = claims.get(self.settings.username_claim) or claims["sub"]
        return str(name)[:64], f"oidc:{self.settings.issuer}#{claims['sub']}"

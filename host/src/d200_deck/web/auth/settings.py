"""Authentication settings: `auth.json`, apart from config.json (which is exported and served by the API)."""

from __future__ import annotations

import json
import os
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from d200_deck.errors import ConfigError
from d200_deck.web.auth.verifier import parse_verifier

DEFAULT_TTL_S = 172_800
MIN_TTL_S = 60
MAX_TTL_S = 30 * 24 * 3600
KEYS = {"enabled", "username", "password_hash", "session_ttl_seconds", "public_origin", "trusted_proxies", "oidc"}
OIDC_KEYS = {"issuer", "client_id", "required_role", "role_claim", "username_claim", "scopes", "ca_file"}
MAX_USERNAME = 64
DEFAULT_ROLE = "deck-admin"
DEFAULT_ROLE_CLAIM = "resource_access.{client_id}.roles"  # Keycloak: client roles in the ID token


@dataclass(frozen=True)
class OidcSettings:
    """Keycloak (or another OpenID provider) as login; the client is public, the flow Authorization Code + PKCE."""

    issuer: str
    client_id: str
    required_role: str = DEFAULT_ROLE  # empty: every user of the provider's realm may log in
    role_claim: str = DEFAULT_ROLE_CLAIM  # dotted path into the ID token; {client_id} is replaced
    username_claim: str = "preferred_username"
    scopes: str = "openid"
    ca_file: str = ""  # PEM bundle to trust for the provider's TLS certificate (default: system store)


@dataclass(frozen=True)
class AuthSettings:
    enabled: bool = False
    username: str = ""
    password_hash: str = ""
    session_ttl_seconds: int = DEFAULT_TTL_S
    public_origin: str = ""  # scheme://host[:port] as seen by the browser; empty: derived from the Host header
    trusted_proxies: tuple[str, ...] = ()  # peers whose X-Forwarded-For is believed
    oidc: OidcSettings | None = None

    @property
    def password_login(self) -> bool:
        return self.enabled and bool(self.password_hash)

    @property
    def secure(self) -> bool:
        return self.public_origin.startswith("https://")

    @property
    def cookie_name(self) -> str:
        return "__Host-deck_session" if self.secure else "deck_session"

    @property
    def public_host(self) -> str:
        return urlsplit(self.public_origin).netloc if self.public_origin else ""


def _origin(value: Any) -> str:
    if not isinstance(value, str):
        raise ConfigError("auth: public_origin must be a string")
    if not value:
        return ""
    parts = urlsplit(value)
    if parts.scheme not in ("http", "https") or not parts.hostname or parts.path not in ("", "/") or parts.query:
        raise ConfigError("auth: public_origin must look like https://deck.example or http://host:7280")
    return f"{parts.scheme}://{parts.netloc.lower()}"


def _text(data: dict[str, Any], key: str, default: str) -> str:
    value = data.get(key, default)
    if not isinstance(value, str):
        raise ConfigError(f"auth: oidc.{key} must be a string")
    return value.strip()


def _oidc(data: Any, public_origin: str) -> OidcSettings | None:
    if data is None:
        return None
    if not isinstance(data, dict):
        raise ConfigError("auth: oidc must be an object")
    unknown = set(data) - OIDC_KEYS
    if unknown:
        raise ConfigError(f"auth: unknown oidc keys {sorted(unknown)}")
    defaults = OidcSettings("", "")
    values = {k: _text(data, k, getattr(defaults, k)) for k in OIDC_KEYS}
    parts = urlsplit(values["issuer"])
    if parts.scheme not in ("http", "https") or not parts.hostname or parts.query or parts.fragment:
        raise ConfigError("auth: oidc.issuer must be the provider's issuer URL, e.g. https://id.example/realms/deck")
    if not values["client_id"]:
        raise ConfigError("auth: oidc.client_id is required")
    if not public_origin:
        raise ConfigError("auth: oidc needs public_origin (the callback URL is built from it)")
    return OidcSettings(**{**values, "issuer": values["issuer"].rstrip("/")})


def parse_settings(data: Any) -> AuthSettings:
    if not isinstance(data, dict):
        raise ConfigError("auth settings must be a JSON object")
    unknown = set(data) - KEYS
    if unknown:
        raise ConfigError(f"auth: unknown keys {sorted(unknown)}")
    enabled = data.get("enabled", False)
    ttl = data.get("session_ttl_seconds", DEFAULT_TTL_S)
    username = data.get("username", "")
    verifier = data.get("password_hash", "")
    proxies = data.get("trusted_proxies", [])
    if not isinstance(enabled, bool):
        raise ConfigError("auth: enabled must be true or false")
    if not isinstance(ttl, int) or isinstance(ttl, bool) or not MIN_TTL_S <= ttl <= MAX_TTL_S:
        raise ConfigError(f"auth: session_ttl_seconds must be {MIN_TTL_S}-{MAX_TTL_S}")
    if not isinstance(username, str) or not isinstance(verifier, str):
        raise ConfigError("auth: username and password_hash must be strings")
    if not isinstance(proxies, list) or not all(isinstance(p, str) for p in proxies):
        raise ConfigError("auth: trusted_proxies must be a list of addresses")
    origin = _origin(data.get("public_origin", ""))
    oidc = _oidc(data.get("oidc"), origin)
    if enabled and (verifier or username or oidc is None):  # local login: configured completely, or OIDC alone
        if not 0 < len(username) <= MAX_USERNAME:
            raise ConfigError(f"auth: username must be 1-{MAX_USERNAME} characters when auth is enabled")
        parse_verifier(verifier)
    return AuthSettings(enabled, username, verifier, ttl, origin, tuple(proxies), oidc)


def to_json(settings: AuthSettings) -> dict[str, Any]:
    data = asdict(settings)
    data["trusted_proxies"] = list(settings.trusted_proxies)
    return data


def load_settings(path: Path, *, must_exist: bool = False) -> AuthSettings:
    """Read the settings; a missing file means authentication is off unless `must_exist` (an explicit path)."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        if must_exist:
            raise ConfigError(f"auth file {path} does not exist") from None
        return AuthSettings()
    except OSError as e:
        raise ConfigError(f"cannot read {path}: {e}") from e
    if path.stat().st_mode & 0o077:
        raise ConfigError(f"{path} holds the password verifier: chmod 600 it")
    try:
        return parse_settings(json.loads(text))
    except json.JSONDecodeError as e:
        raise ConfigError(f"{path}: invalid JSON: {e}") from e


def save_settings(path: Path, settings: AuthSettings) -> None:
    """Write atomically with mode 0600."""
    path.parent.mkdir(parents=True, exist_ok=True)
    data = to_json(settings)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".auth-", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)

"""Strict, portable parsing of local web/auth build inputs (never prints secrets)."""

import json
import re
from pathlib import Path
from urllib.parse import urlsplit

DEFAULT_TTL = 172800
DEFAULT_ROUNDS = 120000


def verifier_parts(value):
    if not isinstance(value, str):
        raise ValueError("password_hash must be a PBKDF2 verifier")
    match = re.fullmatch(r"pbkdf2_sha256\$(\d+)\$([0-9a-f]{32})\$([0-9a-f]{64})", value)
    if not match or not 10000 <= int(match[1]) <= 200000:
        raise ValueError("invalid PBKDF2 verifier (10000-200000 rounds, 16-byte salt, 32-byte hash)")
    return int(match[1]), bytes.fromhex(match[2]), bytes.fromhex(match[3])


def _object(value, name, allowed):
    if not isinstance(value, dict) or set(value) - allowed:
        raise ValueError(f"invalid {name} fields")
    return value


def _tls_material(project, web):
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization

    paths = []
    for name in ("certificate", "private_key"):
        value = web.get(name)
        if not isinstance(value, str) or not value:
            raise ValueError(f"HTTPS requires web.{name}")
        path = (project / value).resolve()
        if not path.is_file():
            raise ValueError(f"HTTPS {name} file is missing")
        paths.append(path)
    try:
        cert = x509.load_pem_x509_certificate(paths[0].read_bytes())
        key = serialization.load_pem_private_key(paths[1].read_bytes(), password=None)
        public = lambda k: k.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        if public(cert.public_key()) != public(key.public_key()):
            raise ValueError("key mismatch")
    except (ValueError, TypeError) as error:
        raise ValueError("invalid HTTPS certificate/private key or key mismatch") from error
    return paths


def load(project, config_path=None):
    project = Path(project)
    path = project / (config_path or "auth.local.json")
    if config_path and not path.is_file():
        raise ValueError("explicit auth configuration file is missing")
    raw = json.loads(path.read_text()) if path.exists() else {}
    _object(raw, "configuration", {"web", "auth"})
    web = _object(raw.get("web", {}), "web", {"transport", "public_origin", "certificate", "private_key"})
    auth = _object(raw.get("auth", {}), "auth", {"enabled", "username", "password_hash", "session_ttl_seconds"})
    transport = web.get("transport", "http")
    if transport not in ("http", "https"):
        raise ValueError("web.transport must be http or https")
    origin = web.get("public_origin", "")
    if not isinstance(origin, str) or any(ord(c) <= 32 or ord(c) >= 127 for c in origin):
        raise ValueError("public_origin must be an ASCII origin")
    host, port = "", 443 if transport == "https" else 80
    if origin:
        url = urlsplit(origin)
        if (url.scheme != transport or not url.hostname or url.username is not None or url.password is not None
                or url.path not in ("", "/") or url.query or url.fragment
                or len(url.hostname) > 120 or not re.fullmatch(r"[a-z0-9.-]+", url.hostname)):
            raise ValueError("public_origin must match transport and contain only a hostname/IP and optional port")
        host = url.hostname
        port = url.port if url.port is not None else port
        if not 1 <= port <= 65535:
            raise ValueError("invalid public_origin port")
        default = 443 if transport == "https" else 80
        origin = f"{transport}://{host}" + (f":{port}" if port != default else "")
    enabled = auth.get("enabled", False)
    if not isinstance(enabled, bool):
        raise ValueError("auth.enabled must be true or false")
    ttl = auth.get("session_ttl_seconds", DEFAULT_TTL)
    if type(ttl) is not int or not 1 <= ttl <= 2147483647:
        raise ValueError("session_ttl_seconds must be an integer 1-2147483647")
    username = auth.get("username", "")
    if not isinstance(username, str) or len(username.encode()) > 64 or any(ord(c) < 32 for c in username):
        raise ValueError("invalid auth.username (at most 64 UTF-8 bytes)")
    rounds, salt, digest = 0, bytes(16), bytes(32)
    if enabled or "password_hash" in auth:
        rounds, salt, digest = verifier_parts(auth.get("password_hash"))
    if enabled and (not username or not origin):
        raise ValueError("enabled authentication requires username and public_origin")
    cert, key = _tls_material(project, web) if transport == "https" else (None, None)
    return dict(tls=transport == "https", port=port, origin=origin, host=host, enabled=enabled,
                ttl=ttl, username=username, rounds=rounds, salt=salt, digest=digest, cert=cert, key=key)


def header(config):
    def quote(value):
        return '"' + "".join(chr(b) if 32 <= b < 127 and b not in (34, 92) else f"\\{b:03o}" for b in value.encode()) + '"'
    array = lambda value: "{" + ",".join(f"0x{b:02x}" for b in value) + "}"
    return "#pragma once\n" + "\n".join((
        f"#define WEB_TLS {int(config['tls'])}",
        f"#define WEB_PORT {config['port']}",
        f"#define WEB_ORIGIN {quote(config['origin'])}",
        f"#define WEB_HOST {quote(config['host'])}",
        f"#define WEB_AUTH_ENABLED {int(config['enabled'])}",
        f"#define WEB_SESSION_TTL {config['ttl']}U",
        f"#define WEB_AUTH_USER {quote(config['username'])}",
        f"#define WEB_AUTH_ROUNDS {config['rounds']}",
        f"#define WEB_AUTH_SALT {array(config['salt'])}",
        f"#define WEB_AUTH_DIGEST {array(config['digest'])}",
        '#define WEB_COOKIE_NAME "' + ("__Host-deck_session" if config["tls"] else "deck_session") + '"',
    )) + "\n"

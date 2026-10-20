"""Password verifiers `pbkdf2_sha256$rounds$salt32hex$hash64hex`, the format of the ESP32 build tooling."""

from __future__ import annotations

import hashlib
import hmac
import re
import secrets

from d200_deck.errors import ConfigError

SCHEME = "pbkdf2_sha256"
MIN_ROUNDS = 10_000
MAX_ROUNDS = 200_000
DEFAULT_ROUNDS = 120_000
SALT_BYTES = 16
_FORMAT = re.compile(rf"^{SCHEME}\$(\d{{1,6}})\$([0-9a-f]{{{SALT_BYTES * 2}}})\$([0-9a-f]{{64}})$")


def make_verifier(password: str, rounds: int = DEFAULT_ROUNDS) -> str:
    salt = secrets.token_bytes(SALT_BYTES)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, rounds)
    return f"{SCHEME}${rounds}${salt.hex()}${digest.hex()}"


def parse_verifier(verifier: str) -> tuple[int, bytes, bytes]:
    match = _FORMAT.match(verifier)
    if match is None:
        raise ConfigError(f"password_hash must look like {SCHEME}$rounds$salt{SALT_BYTES * 2}hex$hash64hex")
    rounds = int(match.group(1))
    if not MIN_ROUNDS <= rounds <= MAX_ROUNDS:
        raise ConfigError(f"password_hash rounds must be {MIN_ROUNDS}-{MAX_ROUNDS}")
    return rounds, bytes.fromhex(match.group(2)), bytes.fromhex(match.group(3))


def check_password(verifier: str, password: str) -> bool:
    rounds, salt, digest = parse_verifier(verifier)
    candidate = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, rounds)
    return hmac.compare_digest(candidate, digest)

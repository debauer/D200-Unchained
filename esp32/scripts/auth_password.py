"""Print a salted verifier for auth.local.json; the password never enters argv or a file."""

import getpass
import hashlib
import secrets

from auth_config import DEFAULT_ROUNDS


def make_verifier(password, rounds=DEFAULT_ROUNDS):
    data = password.encode("utf-8")
    if not 1 <= len(data) <= 128 or b"\0" in data:
        raise ValueError("password must contain 1-128 UTF-8 bytes")
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", data, salt, rounds, dklen=32)
    return f"pbkdf2_sha256${rounds}${salt.hex()}${digest.hex()}"


def main():
    first = getpass.getpass("Password: ")
    if first != getpass.getpass("Repeat password: "):
        raise SystemExit("Passwords do not match")
    try:
        print(make_verifier(first))
    except ValueError as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()

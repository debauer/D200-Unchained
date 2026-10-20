"""Independent RS256 vectors and a throwaway signing key for the native OIDC tests."""

import base64
import json
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    numbers = key.public_key().public_numbers()
    integer = lambda n: b64(n.to_bytes((n.bit_length() + 7) // 8, "big"))
    jwks = {"keys": [{"kid": "k1", "kty": "RSA", "use": "sig", "alg": "RS256",
                      "n": integer(numbers.n), "e": integer(numbers.e)}]}
    claims = {"iss": "http://id.example/realms/test", "sub": "subject-alice", "aud": "deck", "nonce": "nonce",
              "iat": 1700000990, "exp": 1700001600, "preferred_username": "alice",
              "resource_access": {"deck": {"roles": ["deck-admin"]}}}
    cases = []

    def add(name, changes=None, expected="ok", header=None, payload=None):
        protected = json.dumps(header or {"alg": "RS256", "kid": "k1"}, separators=(",", ":")).encode()
        raw = payload if payload is not None else json.dumps({**claims, **(changes or {})}, separators=(",", ":")).encode()
        signing = (b64(protected) + "." + b64(raw)).encode()
        token = signing.decode() + "." + b64(key.sign(signing, padding.PKCS1v15(), hashes.SHA256()))
        cases.append({"name": name, "token": token, "expected": expected})

    add("valid")
    for name, change in [
        ("wrong issuer", {"iss": "http://evil.example"}), ("wrong audience", {"aud": "elsewhere"}),
        ("wrong nonce", {"nonce": "wrong"}), ("expired", {"exp": 1700000000}),
        ("future iat", {"iat": 1700002000}), ("future nbf", {"nbf": 1700002000}),
        ("null sub", {"sub": None}), ("empty sub", {"sub": ""}), ("long sub", {"sub": "x" * 256}),
        ("non-integer exp", {"exp": 1700001600.5}), ("boolean exp", {"exp": True}),
        ("wrong azp", {"azp": "elsewhere"}), ("missing azp", {"aud": ["deck", "other"]}),
        ("bad aud type", {"aud": ["deck", 123]}),
    ]:
        add(name, change, "token")
    add("multiple audiences", {"aud": ["deck", "other"], "azp": "deck"})
    add("missing role", {"resource_access": {}}, "forbidden")
    add("wrong client role", {"resource_access": {"other": {"roles": ["deck-admin"]}}}, "forbidden")
    add("role is not array", {"resource_access": {"deck": {"roles": "deck-admin"}}}, "forbidden")
    add("renamed display", {"preferred_username": "renamed"})
    add("other subject same name", {"sub": "subject-other"})
    for name, head in [
        ("none", {"alg": "none", "kid": "k1"}), ("hs confusion", {"alg": "HS256", "kid": "k1"}),
        ("critical extension", {"alg": "RS256", "kid": "k1", "crit": ["x"]}),
        ("remote key", {"alg": "RS256", "kid": "k1", "jku": "http://evil.example"}),
        ("unknown kid", {"alg": "RS256", "kid": "other"}),
    ]:
        add(name, expected="token", header=head)
    raw = json.dumps(claims).encode()
    add("duplicate claim", expected="token", payload=b'{"iss":"evil",' + raw[1:])
    add("embedded nul", {"sub": "subject\0ignored"}, "token")
    add("bad utf8", expected="token", payload=raw.replace(b"alice", b"ali\xffce"))
    token = cases[0]["token"]
    first, payload, sig = token.split(".")
    cases.append({"name": "bad signature", "token": ".".join((first, payload, ("A" if sig[0] != "A" else "B") + sig[1:])), "expected": "token"})
    (output / "vectors.json").write_text(json.dumps({"jwks": jwks, "cases": cases}))
    (output / "jwks.json").write_text(json.dumps(jwks))
    (output / "key.pem").write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                                       serialization.NoEncryption()))


if __name__ == "__main__":
    generate(Path(__file__).resolve().parents[1] / ".pio/oidc-fixtures")

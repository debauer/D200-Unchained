"""Generate synthetic, ignored build fixtures. Never flash these known test credentials."""

import hashlib
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def main():
    project = Path(__file__).resolve().parents[1]
    output = project / ".pio/auth-fixtures"
    output.mkdir(parents=True, exist_ok=True)
    salt = bytes(range(16))
    digest = hashlib.pbkdf2_hmac("sha256", b"test-only-password", salt, 120000).hex()
    auth = {"enabled": True, "username": "admin", "password_hash": f"pbkdf2_sha256$120000${salt.hex()}${digest}"}
    http = {"web": {"transport": "http", "public_origin": "http://deck.example"}, "auth": auth}
    (output / "http.json").write_text(json.dumps(http))
    (output / "disabled.json").write_text("{}")
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "deck.example")])
    now = datetime.now(timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(1).not_valid_before(now).not_valid_after(now + timedelta(days=2))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName("deck.example")]), critical=False)
            .sign(key, hashes.SHA256()))
    (output / "cert.pem").write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (output / "key.pem").write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                    serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    https = {"web": {"transport": "https", "public_origin": "https://deck.example",
                      "certificate": str(output / "cert.pem"), "private_key": str(output / "key.pem")}, "auth": auth}
    (output / "https.json").write_text(json.dumps(https))


if __name__ == "__main__":
    main()

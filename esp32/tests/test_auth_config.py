"""Run with the ESP32 tooling Python: python -m unittest discover -s tests -p 'test_*.py'."""

import hashlib
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from auth_config import DEFAULT_TTL, header, load, verifier_parts
from auth_password import make_verifier


class AuthConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.project = Path(self.temp.name)

    def config(self, value):
        (self.project / "auth.local.json").write_text(json.dumps(value))
        return load(self.project)

    def test_unconfigured_build_is_explicitly_disabled_http(self):
        value = load(self.project)
        self.assertFalse(value["enabled"])
        self.assertFalse(value["tls"])
        self.assertEqual(value["ttl"], DEFAULT_TTL)
        self.assertIn("#define WEB_AUTH_ENABLED 0", header(value))
        with self.assertRaises(ValueError):
            load(self.project, "missing.json")

    def test_enabled_http_and_custom_lifetime(self):
        password = 'correct horse "battery"'
        verifier = make_verifier(password)
        rounds, salt, digest = verifier_parts(verifier)
        self.assertEqual(digest, hashlib.pbkdf2_hmac("sha256", password.encode(), salt, rounds))
        value = self.config({"web": {"transport": "http", "public_origin": "http://Deck.Example:8080/"},
                             "auth": {"enabled": True, "username": "admin", "password_hash": verifier,
                                      "session_ttl_seconds": 7200}})
        self.assertEqual(value["origin"], "http://deck.example:8080")
        self.assertEqual(value["port"], 8080)
        self.assertEqual(value["ttl"], 7200)
        self.assertNotIn(password, header(value))
        self.assertIn('#define WEB_COOKIE_NAME "deck_session"', header(value))
        with self.assertRaises(ValueError):
            make_verifier("bad\0password")

    def test_bad_inputs_are_rejected(self):
        for value in [[], {"unknown": True}, {"web": {"transport": "ftp"}},
                      {"auth": {"enabled": "yes"}}, {"auth": {"session_ttl_seconds": True}},
                      {"auth": {"session_ttl_seconds": 0}}, {"auth": {"session_ttl_seconds": 2147483648}},
                      {"auth": {"enabled": True, "username": "admin"}},
                      {"web": {"public_origin": "https://deck.example"}},
                      {"web": {"public_origin": "http://user:pass@deck.example"}},
                      {"web": {"public_origin": "http://deck.example/path"}},
                      {"web": {"public_origin": "http://deck.example:0"}},
                      {"web": {"public_origin": "http://deck.example\n"}},
                      {"auth": {"password_hash": "plaintext"}}]:
            with self.subTest(value=value), self.assertRaises((ValueError, TypeError)):
                self.config(value)

    def test_username_is_utf8_c_escaped(self):
        value = self.config({"auth": {"username": 'a"\\\u00e4\U0001f600'}})
        result = header(value)
        self.assertIn(r'\042\134\303\244\360\237\230\200', result)
        self.assertNotIn(r"\ud83d", result)

    def test_https_requires_matching_material_and_sets_secure_cookie_name(self):
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.x509.oid import NameOID

        key = ec.generate_private_key(ec.SECP256R1())
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "deck.example")])
        now = datetime.now(timezone.utc)
        cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                .serial_number(1).not_valid_before(now).not_valid_after(now + timedelta(days=1))
                .add_extension(x509.SubjectAlternativeName([x509.DNSName("deck.example")]), critical=False)
                .sign(key, hashes.SHA256()))
        (self.project / "cert.pem").write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        (self.project / "key.pem").write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                              serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        config = {"web": {"transport": "https", "public_origin": "https://deck.example:443",
                          "certificate": "cert.pem", "private_key": "key.pem"}}
        value = self.config(config)
        self.assertTrue(value["tls"])
        self.assertEqual(value["origin"], "https://deck.example")
        self.assertIn('"__Host-deck_session"', header(value))
        (self.project / "key.pem").write_bytes(b"invalid")
        with self.assertRaises(ValueError):
            load(self.project)


if __name__ == "__main__":
    unittest.main()

"""A minimal OpenID provider on 127.0.0.1 for tests: discovery, JWKS and a token endpoint that checks PKCE."""

from __future__ import annotations

import base64
import hashlib
import json
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from joserfc import jwt
from joserfc.jwk import RSAKey

CLIENT_ID = "deck"
REALM = "/realms/test"


class FakeIdp:
    def __init__(self) -> None:
        self.key = RSAKey.generate_key(2048, parameters={"kid": "k1"})
        self.claims: dict[str, Any] = {}  # overrides for the next ID tokens
        self.alg = "RS256"
        self.codes: dict[str, dict[str, str]] = {}
        self.jwks_requests = 0
        self.down = False
        handler = type("Handler", (_Handler,), {"idp": self})
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.issuer = f"http://127.0.0.1:{self.server.server_address[1]}{REALM}"
        threading.Thread(target=self.server.serve_forever, args=(0.05,), daemon=True).start()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def authorize(self, url: str, user: str = "alice") -> tuple[str, str]:
        """What the browser does at the provider: log in, get redirected with a code. Returns (code, state)."""
        query = {k: v[0] for k, v in urllib.parse.parse_qs(urllib.parse.urlsplit(url).query).items()}
        assert query["code_challenge_method"] == "S256" and query["response_type"] == "code"
        code = f"code-{len(self.codes)}"
        self.codes[code] = {**query, "user": user}
        return code, query["state"]

    def rotate(self, kid: str) -> None:
        self.key = RSAKey.generate_key(2048, parameters={"kid": kid})

    def id_token(self, request: dict[str, str]) -> str:
        now = int(time.time())
        claims: dict[str, Any] = {
            "iss": self.issuer,
            "aud": CLIENT_ID,
            "azp": CLIENT_ID,
            "sub": "user-1",
            "preferred_username": request["user"],
            "iat": now,
            "exp": now + 300,
            "nonce": request["nonce"],
            "resource_access": {CLIENT_ID: {"roles": ["deck-admin"]}},
        }
        claims.update(self.claims)
        return jwt.encode({"alg": self.alg, "kid": self.key.kid}, claims, self.key, algorithms=[self.alg])


class _Handler(BaseHTTPRequestHandler):
    idp: FakeIdp

    def log_message(self, *args: Any) -> None:
        pass

    def _reply(self, status: int, body: dict[str, Any]) -> None:
        raw = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:
        idp = self.idp
        if idp.down:
            return self._reply(503, {})
        if self.path == f"{REALM}/.well-known/openid-configuration":
            base = idp.issuer
            self._reply(
                200,
                {
                    "issuer": base,
                    "authorization_endpoint": f"{base}/auth",
                    "token_endpoint": f"{base}/token",
                    "jwks_uri": f"{base}/certs",
                },
            )
        elif self.path == f"{REALM}/certs":
            idp.jwks_requests += 1
            self._reply(200, {"keys": [idp.key.as_dict(private=False)]})
        else:
            self._reply(404, {})

    def do_POST(self) -> None:
        idp = self.idp
        form = {
            k: v[0]
            for k, v in urllib.parse.parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode()).items()
        }
        request = idp.codes.pop(form.get("code", ""), None)
        digest = hashlib.sha256(form.get("code_verifier", "").encode()).digest()
        challenge = base64.urlsafe_b64encode(digest).rstrip(b"=").decode()
        if (
            self.path != f"{REALM}/token"
            or request is None
            or challenge != request["code_challenge"]
            or form.get("redirect_uri") != request["redirect_uri"]
            or form.get("client_id") != CLIENT_ID
        ):
            return self._reply(400, {"error": "invalid_grant"})
        self._reply(200, {"id_token": idp.id_token(request), "access_token": "unused", "token_type": "Bearer"})

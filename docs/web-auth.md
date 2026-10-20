# ESP32 web authentication

Current implementation: local password login, cookie sessions, configurable
HTTP/HTTPS and session lifetime, plus native Keycloak OIDC. See
[esp32-oidc.md](esp32-oidc.md) for realm/client configuration and the flow.
The firmware advertises `password` and, when configured, `oidc`.

## Configure and build

In `esp32/`:

```sh
uv sync
cp auth.local.example.json auth.local.json
uv run python scripts/auth_password.py
```

Put the printed verifier into `auth.password_hash` in `auth.local.json`, set
the username and the actual device origin, then build normally with
`uv run pio run`. Neither the plaintext password nor the verifier belongs in
Git. `auth.local.json` and the generated `main/web_local.h` are ignored.
Password generation prompts twice; it does not accept a password on the
command line. It embeds a salted PBKDF2-HMAC-SHA256 verifier with 120000 rounds.
On-device password checks run in a separate, bounded worker. Their latency and
watchdog/USB interaction still need hardware measurement.

Example (replace the verifier placeholder):

```json
{
  "web": {
    "transport": "http",
    "public_origin": "http://192.168.1.50"
  },
  "auth": {
    "enabled": true,
    "username": "admin",
    "password_hash": "pbkdf2_sha256$...",
    "session_ttl_seconds": 172800
  }
}
```

`public_origin` is the canonical origin: scheme, hostname/IPv4 and optional
port, with no path. DNS or a DHCP reservation must make it stable. Requests
with other Host headers are rejected; changing requests with an Origin header
must match this origin exactly and carry `X-Deck-Request: 1`.

Without `auth.local.json`, the existing HTTP development mode has no login and
logs `authentication DISABLED`. A present invalid configuration fails the build.
`DECK_AUTH_CONFIG=<file>` can explicitly select another build configuration;
a missing explicitly selected file is an error, not a downgrade to no auth.

## HTTPS is optional

To use HTTPS, change the web section to:

```json
{
  "transport": "https",
  "public_origin": "https://deck.home.example",
  "certificate": "tls-cert.local.pem",
  "private_key": "tls-key.local.pem"
}
```

Supply a PEM certificate chain and matching unencrypted PEM private key. The
build validates the leaf/key pair and embeds both; browser trust and certificate
renewal remain deployment responsibilities. `*.local.pem` is ignored by Git.
HTTP does not need certificates and does not redirect to HTTPS or enable HSTS.
HTTP carries credentials/cookies unencrypted. HTTPS cookies have `Secure` and
the `__Host-` prefix; HTTP cookies omit both. Both use `HttpOnly`, `SameSite=Lax`
and `Path=/` with no Domain attribute.

HTTPS reserves two of its four inbound sockets for API/static traffic and
allows at most two SSE clients. Stream writes retain ESP-IDF's TLS transport;
they never use raw socket sends on HTTPS. Hardware RAM/concurrency testing is
still required.

## Sessions and API

- Default duration: 48 hours (172800 seconds), configurable from 1 to
  2147483647 seconds. Absolute expiry; API polling and SSE do not extend it.
- Eight sessions in RAM, random 256-bit IDs, only SHA-256 token hashes stored.
  Reboot invalidates all sessions. A full table rejects new sessions with 503.
- Login rotates the requesting browser's old session. Logout revokes the
  session and closes its SSE clients. Expired streams are checked before each
  send and at the 15-second keepalive.
- Login budget: five attempts/source and twenty globally per minute; 429 with
  Retry-After when exhausted. IPv4 source comes from the socket, not proxy headers.
- All `/api/*` routes require a valid session when enabled, including reads,
  image/icon downloads, logs and events. Missing/expired session: 401 JSON.
- Login shell/static resources and `GET /auth/session` are public. API/auth
  responses use `Cache-Control: no-store`. Auth settings are not in deck JSON
  configuration or its export.

| Method | Path | Behavior |
|---|---|---|
| GET | `/auth/session` | `required`, `authenticated`, `methods`, `username`, opaque `identity`, `expires_in`, `session_ttl_seconds` |
| POST | `/auth/login` | JSON `username` and `password`; 200 with session state and Set-Cookie, 401 for invalid credentials |
| POST | `/auth/logout` | Revoke current cookie session and clear cookie; idempotent when expired |
| POST | `/auth/oidc/start` | Configured Keycloak login: JSON redirect URL and transaction cookie |
| GET | `/auth/oidc/callback` | One-use browser callback; 302 to the UI on success/error |

All POST endpoints need `X-Deck-Request: 1`. The local login has full panel
permissions. The shared browser login supports these endpoints on both
backends: it prompts for credentials before loading the panel, provides
logout and handles expired sessions without submitting pending edits as
another user. Firmware and the shared web partition must both be updated.

## Verification

- `uv run python -m unittest discover -s tests -p 'test_*.py'` in `esp32/`:
  build configuration, malformed inputs, verifier generation and TLS material.
- `scripts/test.sh`: native session lifetime/capacity/revocation, cookie parsing,
  throttling and Origin checks, plus existing firmware regressions.
- `bash scripts/test_auth.sh`: builds the SDK's existing mbedTLS for the PC and
  runs actual password/session/login/logout code with mocked HTTP/RTOS adapters,
  checking HTTP and HTTPS cookie policies. Set `PLATFORMIO_CORE_DIR` if needed.
- `tests/build_auth_fixtures.py` generates synthetic configs/certificates under
  ignored `.pio/auth-fixtures/` for compile checks. Never flash these known test
  credentials. Both authenticated HTTP and HTTPS firmware builds are checked.

`bash scripts/test_oidc.sh` additionally checks the complete native OIDC flow,
RS256/claims, replay and role checks, rotation and provider failures. No real
device was flashed. A real ESP32/Keycloak deployment, certificate provisioning
and on-device transport/heap tests remain separate deployment checks.

# Keycloak OIDC on the ESP32

The ESP32 is the OIDC client: no Linux host or authentication proxy is needed.
The browser signs in at Keycloak; the ESP32 exchanges the code, verifies the
ID token and creates its own cookie session. Local password login remains
available in parallel. Session duration defaults to 48 hours and is configured
by `auth.session_ttl_seconds` for both methods.

## Firmware configuration

Copy `esp32/auth.oidc.example.json` to the ignored `esp32/auth.local.json`.
Generate the local password verifier as described in [web-auth.md](web-auth.md).
Example:

```json
{
  "web": {
    "transport": "http",
    "public_origin": "http://deck.home.example"
  },
  "auth": {
    "enabled": true,
    "username": "admin",
    "password_hash": "pbkdf2_sha256$...",
    "session_ttl_seconds": 172800,
    "oidc": {
      "issuer": "https://keycloak.home.example/realms/home",
      "client_id": "deck",
      "scopes": "openid profile"
    }
  }
}
```

- `issuer` is the exact Keycloak realm issuer, without a trailing slash. The
  ESP32 and browser must both be able to resolve/reach it. A Docker-internal
  name or `localhost` on your PC is not the ESP32's Keycloak address.
- `client_id` is the public Keycloak client ID. No client secret is embedded.
- `scopes` defaults to `openid profile`; `openid` is required.
- Optional `ca_file` points to a PEM CA certificate/bundle for a private
  Keycloak CA. It is embedded at build time; without it ESP-IDF's public CA
  bundle is used. Certificate/hostname verification stays enabled for HTTPS.
- The device's web transport and the issuer transport are independent. Both
  HTTP and HTTPS are supported; HTTPS is not enforced. HTTP does not provide
  transport encryption. Keycloak must allow the chosen callback scheme.
- Omitting `auth.oidc` disables OIDC, not local login. Auth settings are never
  part of `/api/config`, its export, or `/auth/session`.

Build as usual. OIDC builds require `CONFIG_HTTPD_MAX_URI_LEN=2048` (the default
in `sdkconfig.defaults`). For an existing `sdkconfig.esp32s3`, update this
setting with `uv run pio run -t menuconfig`; defaults do not override existing
settings. Otherwise the OIDC build stops with an explicit error. Runtime-only
URI size changes cannot increase ESP-IDF's fixed request URI array.

Flash both firmware and the shared `www` partition. Open the configured
`public_origin`: the form offers local login and **Sign in with Keycloak**.

## Keycloak client and role

Create a client with:

- Client ID matching `client_id`; Client authentication off (public client).
- Standard Flow on, PKCE code challenge method **S256**.
- Exact valid redirect URI, e.g.:
  `http://deck.home.example/auth/oidc/callback`.
- RS256 ID-token signing. Implicit Flow and Direct Access Grants are not used.
- A client role named **deck-admin**, assigned only to users who may configure
  the panel. A realm role or a role on a different client does not qualify.

The role must be in the **signed ID token**, not just the access token.
Add a User Client Role protocol mapper for the deck client with:

```json
{
  "usermodel.clientRoleMapping.clientId": "deck",
  "claim.name": "resource_access.${client_id}.roles",
  "jsonType.label": "String",
  "multivalued": "true",
  "id.token.claim": "true"
}
```

With client ID `deck`, the resulting ID-token claim must contain:

```json
{"resource_access":{"deck":{"roles":["deck-admin"]}}}
```

The policy is fixed on ESP32: a verified token without this client role is
refused. See `docker/test/realm/deck-test-realm.json` for an existing mapper
example, adapting its hostnames and redirect URI for your real ESP32.

## Flow and behavior

1. `POST /auth/oidc/start` requires `X-Deck-Request: 1` and a matching Origin
   when supplied. It returns `{"redirect":"..."}` and sets a browser-bound,
   HttpOnly transaction cookie (Secure only for HTTPS).
2. Keycloak authenticates the user and redirects the browser back with a code
   and state. The callback is a GET and does not require the mutation header.
3. The ESP32 consumes the transaction once, checks state and optional response
   issuer, exchanges the code with its PKCE verifier and verifies the token.
4. Success rotates the old local session, clears the transaction cookie, sets
   the normal session cookie and redirects to `/`. No provider token is sent
   to the browser or retained for refresh.
5. Failure clears only the transaction cookie and redirects to
   `/?auth_error=<code>`. Codes are `denied`, `expired`, `forbidden`, `token`,
   `provider`, `clock` or `capacity`; provider-controlled error text is not
  echoed. A failed OIDC login does not revoke an existing device session.

Auth mode limits SDK HTTP protocol logging to errors, because verbose header
and URI traces contain session cookies and callback codes. The firmware does
not log provider tokens or authorization request bodies.

RS256 verification checks configured JWKS keys (2048-4096-bit RSA), issuer,
audience, authorized party where applicable, nonce, subject and integer
timestamps (exp/iat/optional nbf), with 60 seconds of clock leeway. Supported
JWT types, when present, are `JWT` and `application/jwt`. JSON has bounded
nesting/size (12 levels, 128 fields/object, 512 entries/array, 2048 nodes), unique
object keys and valid UTF-8. Header-supplied key URLs are
not followed. An unset clock prevents OIDC; local login
does not depend on NTP or provider availability.

Discovery and JWKS are cached for one hour. An unknown signing key can trigger
one additional JWKS refresh per 30 seconds. Discovered endpoints must stay on
the configured issuer's origin/scheme. HTTP redirects are not followed.
Network work runs in a separate bounded worker, with socket timeouts and a
15-second response-processing budget; DNS/TLS/header setup is also subject to
the SDK's network timeouts. Response caps: 16 KiB discovery, 32 KiB token reply,
64 KiB JWKS; at most 16 keys and 16 KiB compact ID tokens. Client IDs are limited
to 128 ASCII bytes, issuer URLs to 255 and endpoint URLs to 511 bytes.

Four pending transactions fit in RAM and expire after ten minutes. The start
endpoint has a separate five/source, twenty/global attempts-per-minute budget.
An authorization request that cannot be completed does not silently fall back
to an authenticated session. The local account remains an explicit login choice.

Sessions expose an opaque `identity` separately from the display `username`.
For OIDC it is derived from issuer/subject; local accounts have a separate
namespace. The UI uses this identity for pending-edit recovery, so two users
with the same display name cannot inherit each other's unsaved work. Leaving
the page for Keycloak loses in-memory edits after the existing confirmation.

Logout ends the deck session. It does not end Keycloak SSO, refresh tokens,
or immediately propagate a later role withdrawal to an already-issued local
session. Such sessions expire according to the configured local lifetime.

## Verification

From `esp32/`, after installing the build tooling:

```sh
uv run python -m unittest discover -s tests -p 'test_*.py'
PYTHON="$(pwd)/.venv/bin/python" bash scripts/test_oidc.sh
```

`PLATFORMIO_CORE_DIR` can select an alternate PlatformIO installation. Tests
generate throwaway RSA keys under ignored `.pio/oidc-fixtures/`. They exercise
real firmware flow/crypto code with HTTP/RTOS/provider adapters: independently
signed tokens, PKCE, browser binding, replay/expiry, role denial, wrong claims,
signature failures, key rotation, local fallback and both cookie policies.
Separate adapter tests cover partial HTTP reads/writes, bounded responses,
truncation, timeouts, redirect rejection and CA-bundle/custom-CA selection.

The browser tests run with `node --test tests/web/*.test.mjs` from `host/`.
`tests/build_auth_fixtures.py` provides OIDC-enabled HTTP/HTTPS compile profiles
with known test credentials; do not flash those fixtures.

No real ESP32/Keycloak deployment has been exercised in this environment.
Hardware TLS/heap measurements, a real realm, and simultaneous USB/MQTT/browser
load remain required deployment verification.

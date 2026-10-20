# ESP32 web authentication plan

Agreed on 2026-10-20. Scope: ESP32 first; keep the shared web UI usable with
the existing Linux host. This is an implementation plan, not a statement that
all features below are already available.

## Decisions

- Local firmware-defined username/password and native OIDC against Keycloak
  are available in parallel. Local login works without Keycloak.
- HTTP and HTTPS are explicitly configurable. HTTP remains the default; HTTPS
  is not a prerequisite enforced by the firmware for local login or OIDC.
- Configure a canonical `public_origin` (scheme, address and optional port).
  Use it for Host/Origin validation and the exact OIDC callback URI.
- HTTPS requires a supplied PEM certificate chain/private key. Certificate
  provisioning is still open; no automatic redirect to HTTPS or HSTS in HTTP mode.
- The Keycloak connection has its own issuer URL and trust configuration,
  independent of the ESP32 web transport. Verify certificates when using HTTPS.
- Session duration is configurable; default 48 hours (172800 seconds).
  Absolute expiry, not renewed by API polling or SSE. RAM sessions end on reboot.
- Authentication settings and credentials are separate from deck configuration,
  MQTT settings and configuration exports.

## 1. Build configuration and transport

- Local ignored auth configuration, checked-in example, strict build-time
  validation and generated ignored build inputs. No default username/password.
- Embed username and salted PBKDF2-HMAC-SHA256 verifier, not plaintext password.
  Provide a password helper that prompts without putting the password in argv.
- `web.transport`: `http` or `https`; validate against `web.public_origin`.
- `auth.session_ttl_seconds`: positive bounded integer, default 172800.
- Missing/invalid credentials or HTTPS material fail an enabled auth build.
  Existing unconfigured development builds remain explicitly unauthenticated.
- Use ESP-IDF HTTPS server for HTTPS, existing HTTP server for HTTP.
- Adapt SSE: its current raw socket send override must not bypass TLS.
  Keep publishers independent of socket I/O; bound sender delays and resources.
- Measure TLS/socket/heap budget under USB, MQTT and browser load.

## 2. Sessions and local login

- Central authorization before every protected route, including reads, images,
  logs and SSE. Public: login shell/static assets and required auth endpoints.
- Random 256-bit session IDs; bounded RAM table, server-side token hashes,
  monotonic expiration, fresh ID on login, server-side revocation on logout.
- Cookies: HttpOnly, SameSite=Lax, Path=/; Secure only for HTTPS. No __Host-
  prefix in HTTP mode. Cookie Max-Age matches configured session duration.
- Generic credential errors, constant-time verifier comparison, global and
  per-source login throttling. Bounded worker for expensive password checks.
- Recheck sessions for active SSE clients and close streams on logout/expiry.
- Protected responses use Cache-Control: no-store. Never log passwords, cookie
  values, authorization codes or tokens.
- Keep X-Deck-Request: 1; check Origin on changing requests when supplied.

## 3. Shared login UI and API

| Endpoint | Purpose |
|---|---|
| GET /auth/session | Public authentication capabilities/current session |
| POST /auth/login | Local password login |
| POST /auth/logout | Revoke current local session |
| POST /auth/oidc/start | Create OIDC transaction and return redirect URL |
| GET /auth/oidc/callback | Validate Keycloak callback and create session |

- 401 for missing/expired authentication; 403 for denied authorization.
- Local login form, Keycloak button when implemented/configured, logout button.
- Stop autosave and SSE retries on authentication loss; retain unsaved edits
  for the same user, or discard explicitly. Do not submit old edits as another user.
- Existing Linux host stays usable; auth is a backend capability, not a frontend
  assumption that silently downgrades a protected server.

## 4. Native Keycloak OIDC

- Authorization Code Flow with mandatory PKCE/S256, state and nonce.
- Public Keycloak client; exact configured callback URI, Standard Flow enabled.
  If HTTP is selected, Keycloak redirect and realm SSL policies must permit it.
- Browser-bound, short-lived, one-use transactions held in bounded RAM.
- Server-side discovery, code exchange and JWKS fetch with bounded workers,
  deadlines, response sizes and cache. No tokens in browser localStorage.
- Initially RS256 only: verify signature, issuer, audience, azp where applicable,
  exp, iat, optional nbf, nonce and sub. Reject algorithm/key confusion.
- Validate time before OIDC token acceptance; local login remains independent
  of NTP/Keycloak availability.
- Refresh JWKS once on an unknown kid, with rate limiting and bounded keys.
- Proposed authorization: explicit `deck-admin` Keycloak client role included
  in the signed ID token; realm membership alone is not enough. Confirm this
  policy before implementing the role check.
- Successful OIDC login creates the same local session as password login.
- Initial logout ends the device session; Keycloak SSO remains active. Global
  logout and immediate propagation of role withdrawal are later extensions.
- Prefer suitable maintained verification components where compatible; agree
  any new dependency before introduction. Existing crypto primitives: mbedTLS.

## Verification

- Native tests for config validation, password verification, cookie parsing,
  session capacity/expiry/revocation, throttling and route/Origin checks.
- Tests proving SSE authorization ends with the session and transport remains
  TLS-aware. HTTP and HTTPS firmware builds; keep existing regression suites.
- JavaScript tests for login/logout, expiry, pending edits and Linux compatibility.
- OIDC fixtures: tampered signature/claims, wrong issuer/audience/nonce,
  replayed callback, missing role, key rotation, missing time and provider outage.
- Keycloak integration test realm/client; hardware tests with concurrent USB,
  MQTT, multiple tabs, network outages and constrained TLS memory.

## Implementation progress

- [x] Build configuration, verifier helper and configurable HTTP/HTTPS.
- [x] Sessions, local login, route protection and SSE revocation.
- [x] Shared login/logout UI and authentication-loss handling.
- [ ] Keycloak OIDC and authorization policy.
- [ ] Integration/hardware verification and final documentation.

Commit each verified implementation stage. Update this progress section with
the actual tests and remaining limitations; do not mark unimplemented OIDC
as available.

### Backend stage, 2026-10-20

Implemented local login, configurable 48-hour sessions and HTTP/HTTPS. See
[web-auth.md](web-auth.md) for configuration and the API. Verified with five
Python configuration tests, native core/Origin regression tests and real
mbedTLS login/logout/cookie API tests in both cookie modes. Authenticated HTTP
and HTTPS firmware profiles compile. Hardware verification remains open.
The browser login and Keycloak OIDC are not included in this backend stage.

The shared browser login was subsequently added in parallel in commit
`e9881dd` (Linux authentication backend: `b35698a`). Both backends use the
same `/auth/session`, `/auth/login`, `/auth/logout` contract and cookie
policies. All 30 JavaScript tests pass, including missing-session recovery,
pending-edit retention for the same user and logout behavior. Keycloak OIDC
is still unimplemented; its role policy and certificate provisioning remain
open as described above.

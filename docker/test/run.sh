#!/bin/sh
# Build and start the test stack, run the login checks, tear it down. Needs docker compose and python3.
# KEEP=1 leaves the stack running (UI: https://deck.localtest.me:8443, Keycloak admin: admin/admin).
set -eu
cd "$(dirname "$0")"
mkdir -p out/certs
docker compose up -d --build --wait keycloak deck proxy
status=0
python3 e2e.py --ca out/certs/ca.pem || status=$?
if [ "$status" -ne 0 ]; then docker compose logs --tail=60 deck keycloak proxy; fi
[ "${KEEP:-0}" = 1 ] || docker compose down -v
exit "$status"

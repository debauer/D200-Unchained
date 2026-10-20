#!/bin/sh
# Test CA and a server certificate for the two test host names (written to /certs, kept if present).
set -eu
cd /certs
[ -f server.pem ] && exit 0
apk add --no-cache openssl >/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj "/CN=deck test CA" -keyout ca.key -out ca.pem
openssl req -newkey rsa:2048 -nodes -subj "/CN=deck.localtest.me" -keyout server.key -out server.csr
printf 'subjectAltName=DNS:deck.localtest.me,DNS:keycloak.localtest.me\n' > san.ext
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 30 -extfile san.ext -out server.pem
chmod 644 ca.pem server.pem server.key

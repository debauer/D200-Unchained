#!/bin/sh
# Renew twice a day; certbot only acts when a certificate is due. nginx reloads on its own schedule.
set -eu
while :; do
    certbot renew --quiet
    sleep 12h
done

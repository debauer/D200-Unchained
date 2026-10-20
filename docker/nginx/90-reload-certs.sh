#!/bin/sh
# Picks up renewed certificates: reload nginx every 6 hours (started in the background before nginx).
(while :; do sleep 6h; nginx -s reload; done) &

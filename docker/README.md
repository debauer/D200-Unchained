# Docker Compose: deck host behind nginx with Let's Encrypt (DNS-01 via INWX)

Three long-running services plus a one-off:

| Service | Role |
|---|---|
| `deck` | `deck serve` with `adb`; reaches the D200 through `/dev/bus/usb` (no privileged mode). Not published, only the proxy talks to it |
| `proxy` | nginx: TLS on 443 (80 redirects), forwards to `deck:7280`, SSE unbuffered, reloads renewed certificates every 6 h |
| `certbot` | renews certificates twice a day (`certbot-dns-inwx`) |
| `certbot-issue` | one-off first issuance (profile `setup`) |

TLS ends at nginx; the web login (docs/web-auth-plan.md) is the host's own. Never publish port 7280 directly.

## Setup

1. **USB access on the host.** Stop any host adb server (`adb kill-server`, `systemctl --user stop deck`): only one
   adb server can own the device. Then:
   ```sh
   sudo groupadd -r usb-d200
   sudo cp udev/99-ulanzi-d200.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
   getent group usb-d200        # the numeric id is D200_USB_GID
   ```
   The rule matches the USB vendor id 0x2207 only (from the device's gadget script); replug the D200.
2. `cp .env.example .env` and fill in the domain, e-mail and `D200_USB_GID`.
3. `cp secrets/inwx.ini.example secrets/inwx.ini`, `chmod 600 secrets/inwx.ini`, enter the INWX API login.
4. Issue the certificate (needs the domain's zone at INWX; keep `LETSENCRYPT_STAGING=1` until it works):
   ```sh
   docker compose run --rm certbot-issue
   ```
   To switch from staging to a real certificate: set `LETSENCRYPT_STAGING=` and run
   `docker compose run --rm --entrypoint certbot certbot delete --cert-name "$DECK_DOMAIN"`, then issue again.
5. Install `deckd` (ARMv7, built by `../install.sh` or `cargo build` in `../device`) into the data volume:
   ```sh
   docker compose up -d deck
   docker compose cp ../device/target/armv7-unknown-linux-musleabihf/release/deckd deck:/data/deckd
   ```
6. Set the login **before** exposing it. The proxy has a fixed address in the compose network:
   ```sh
   docker compose exec deck deck passwd -u admin --public-origin "https://$DECK_DOMAIN" --trusted-proxy 172.28.0.10
   docker compose restart deck
   ```
7. `docker compose up -d`, open `https://<domain>/`.

## Operation

- Logs: `docker compose logs -f deck`. D200 visible? `docker compose exec deck adb devices`.
- Config and login live in the `config` volume (`config.json`, `mqtt.json`, `auth.json`); deckd, images and the adb key in `data`.
- Change the password: repeat step 6 (other settings are kept), then `docker compose restart deck`.
- Update: `git pull && docker compose build && docker compose up -d`.

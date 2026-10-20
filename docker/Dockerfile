# Host service (`deck serve`) with adb for the D200. Build context: the repository root (public/).
FROM python:3.12-slim

ARG EXTRAS="[oidc]"
RUN apt-get update \
    && apt-get install -y --no-install-recommends android-tools-adb \
    && rm -rf /var/lib/apt/lists/*

COPY host /src/host
RUN pip install --no-cache-dir "/src/host${EXTRAS}" && rm -rf /src

# config.json, mqtt.json and auth.json live in /config; deckd, images and the adb key in /data.
RUN useradd --uid 1000 --create-home --home-dir /data/home deck \
    && mkdir -p /config /data \
    && chown -R deck /config /data
USER deck
ENV XDG_CONFIG_HOME=/config XDG_DATA_HOME=/data HOME=/data/home
VOLUME ["/config", "/data"]

EXPOSE 7280
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s \
    CMD python -c "import urllib.request as u; u.urlopen('http://127.0.0.1:7280/auth/session', timeout=3)"
CMD ["deck", "serve", "--web", "0.0.0.0:7280"]

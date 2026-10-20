"""Build-time Keycloak settings; local credentials remain enabled alongside OIDC."""

from pathlib import Path
from urllib.parse import urlsplit


def load(value, project, auth_enabled):
    if value is None:
        return dict(enabled=False, issuer="", client_id="", scopes="openid profile", ca=None)
    if not auth_enabled or not isinstance(value, dict) or set(value) - {"issuer", "client_id", "scopes", "ca_file"}:
        raise ValueError("OIDC requires enabled authentication and valid oidc fields")
    issuer, client = value.get("issuer"), value.get("client_id")
    if not isinstance(issuer, str) or not issuer or len(issuer) > 255 or any(ord(c) <= 32 or ord(c) >= 127 for c in issuer):
        raise ValueError("OIDC issuer must be an HTTP/HTTPS URL (at most 255 ASCII bytes)")
    url = urlsplit(issuer)
    if (url.scheme not in ("http", "https") or not url.hostname or url.username is not None or url.password is not None
            or url.query or url.fragment or "\\" in issuer or (url.port is not None and not 1 <= url.port <= 65535)):
        raise ValueError("invalid OIDC issuer")
    if not isinstance(client, str) or not 1 <= len(client) <= 128 or any(ord(c) < 33 or ord(c) >= 127 for c in client):
        raise ValueError("invalid OIDC client_id")
    scopes = value.get("scopes", "openid profile")
    if (not isinstance(scopes, str) or len(scopes) > 128 or "openid" not in scopes.split()
            or any(ord(c) < 32 or ord(c) >= 127 for c in scopes)):
        raise ValueError("OIDC scopes must include openid")
    ca = None
    if "ca_file" in value:
        from cryptography import x509
        if not isinstance(value["ca_file"], str) or not value["ca_file"]:
            raise ValueError("invalid OIDC ca_file")
        ca = (Path(project) / value["ca_file"]).resolve()
        try:
            x509.load_pem_x509_certificate(ca.read_bytes())
        except (OSError, ValueError) as error:
            raise ValueError("invalid OIDC CA certificate file") from error
    return dict(enabled=True, issuer=issuer.rstrip("/"), client_id=client, scopes=scopes, ca=ca)

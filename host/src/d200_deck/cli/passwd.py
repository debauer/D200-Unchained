"""`deck passwd`: set the web login (auth.json) without putting the password on the command line."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from typing import Annotated

import typer

from d200_deck import paths
from d200_deck.errors import ConfigError
from d200_deck.web.auth.settings import load_settings, parse_settings, save_settings, to_json
from d200_deck.web.auth.verifier import make_verifier

MIN_PASSWORD = 8


def passwd(
    username: Annotated[str, typer.Option("--username", "-u", prompt=True, help="Login name.")],
    auth_file: Annotated[
        Path | None, typer.Option("--auth-file", help="Settings file (default: $XDG_CONFIG_HOME/deck/auth.json).")
    ] = None,
    public_origin: Annotated[
        str | None, typer.Option("--public-origin", help="URL browsers use, e.g. https://deck.example.org.")
    ] = None,
    ttl: Annotated[int | None, typer.Option("--ttl", help="Session lifetime in seconds (default 172800).")] = None,
    trusted_proxy: Annotated[
        list[str] | None, typer.Option("--trusted-proxy", help="Proxy address whose X-Forwarded-For is believed.")
    ] = None,
) -> None:
    """Enable the web login with this user and a prompted password; other settings are kept."""
    path = auth_file or paths.auth_file()
    password = typer.prompt("Password", hide_input=True, confirmation_prompt=True)
    if len(password) < MIN_PASSWORD:
        typer.echo(f"error: use at least {MIN_PASSWORD} characters", err=True)
        raise typer.Exit(code=1)
    try:
        old = load_settings(path)
        new = replace(
            old,
            enabled=True,
            username=username,
            password_hash=make_verifier(password),
            public_origin=public_origin if public_origin is not None else old.public_origin,
            session_ttl_seconds=ttl if ttl is not None else old.session_ttl_seconds,
            trusted_proxies=tuple(trusted_proxy) if trusted_proxy else old.trusted_proxies,
        )
        parse_settings(to_json(new))  # validate before writing
        save_settings(path, new)
    except ConfigError as e:
        typer.echo(f"error: {e}", err=True)
        raise typer.Exit(code=1) from e
    typer.echo(f"login for {username!r} written to {path}; restart `deck serve`")

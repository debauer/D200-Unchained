"""`deck serve`: the long-running host service."""

from __future__ import annotations

import logging
import shutil
import signal
from collections.abc import Callable
from functools import partial
from pathlib import Path
from types import FrameType
from typing import TYPE_CHECKING, Annotated

import typer

from d200_deck import paths
from d200_deck.errors import ConfigError
from d200_deck.service.bootstrap import BootstrapSettings, ensure_deckd
from d200_deck.service.host import HostService, ServiceSettings
from d200_deck.web.auth.oidc import check_oidc_dependencies
from d200_deck.web.auth.settings import AuthSettings, load_settings
from d200_deck.web.server import DeckWebServer, start_web

DEFAULT_WEB = "127.0.0.1:7280"
LOOPBACK = {"127.0.0.1", "localhost", "::1"}

if TYPE_CHECKING:
    from d200_deck.cli.main import Target


def serve(  # noqa: PLR0913, PLR0917 - one parameter per CLI option
    ctx: typer.Context,
    config: Annotated[
        Path | None, typer.Option("--config", "-c", help="Config file (default: $XDG_CONFIG_HOME/deck/config.json).")
    ] = None,
    images: Annotated[
        Path | None, typer.Option("--images", help="Key image directory (default: $XDG_DATA_HOME/deck/images).")
    ] = None,
    web: Annotated[
        str, typer.Option("--web", help="Web UI address HOST:PORT; other hosts than 127.0.0.1 expose key commands.")
    ] = DEFAULT_WEB,
    no_web: Annotated[bool, typer.Option("--no-web", help="Do not start the web UI.")] = False,
    auth_file: Annotated[
        Path | None, typer.Option("--auth-file", help="Web login settings (default: $XDG_CONFIG_HOME/deck/auth.json).")
    ] = None,
    web_allow_host: Annotated[
        list[str] | None, typer.Option("--web-allow-host", help="Extra Host header value to accept (e.g. name:7280).")
    ] = None,
    adb: Annotated[
        bool, typer.Option("--adb/--no-adb", help="Start deckd on the D200 via adb when it is not reachable.")
    ] = True,
    adb_path: Annotated[str, typer.Option("--adb-path", help="adb executable.")] = "adb",
    deckd_binary: Annotated[
        Path | None,
        typer.Option(
            "--deckd-binary",
            exists=True,
            dir_okay=False,
            help="deckd to push if not running (default: $XDG_DATA_HOME/deck/deckd).",
        ),
    ] = None,
    verbose: Annotated[bool, typer.Option("--verbose", "-v", help="Log every key event.")] = False,
) -> None:
    """Run the host service: draw keys from the config, run key actions, reload on change."""
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s"
    )
    target: Target = ctx.obj
    settings = ServiceSettings(
        host=target.host,
        port=target.port,
        config_path=config or paths.config_file(),
        images_dir=images or paths.images_dir(),
        bootstrap=_bootstrap(adb, adb_path, target, deckd_binary),
    )
    try:
        service = HostService(settings)
    except ConfigError as e:
        typer.echo(f"error: {e}", err=True)
        raise typer.Exit(code=1) from e

    def on_stop(signum: int, frame: FrameType | None) -> None:
        logging.getLogger(__name__).info("stopping (signal %d)", signum)
        service.stop.set()

    def on_hup(signum: int, frame: FrameType | None) -> None:
        service.request_reload()

    signal.signal(signal.SIGTERM, on_stop)
    signal.signal(signal.SIGINT, on_stop)
    signal.signal(signal.SIGHUP, on_hup)
    log = logging.getLogger(__name__)
    log.info("config %s, images %s", settings.config_path, settings.images_dir)
    try:
        auth = load_settings(auth_file or paths.auth_file(), must_exist=auth_file is not None)
        check_oidc_dependencies(auth)
    except ConfigError as e:
        typer.echo(f"error: {e}", err=True)
        raise typer.Exit(code=1) from e
    server = None if no_web else _start_web(service, web, tuple(web_allow_host or ()), auth)
    try:
        service.run()
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()


def _bootstrap(enabled: bool, adb_path: str, target: Target, binary: Path | None) -> Callable[[], None] | None:
    log = logging.getLogger(__name__)
    if not enabled:
        return None
    if target.host not in LOOPBACK:
        log.info("deckd at %s is remote: not starting it via adb", target.host)
        return None
    if shutil.which(adb_path) is None:
        log.warning("%s not found: deckd will not be started automatically", adb_path)
        return None
    if binary is None and paths.deckd_binary().is_file():
        binary = paths.deckd_binary()
    return partial(ensure_deckd, BootstrapSettings(adb_path, target.port, binary))


def _start_web(service: HostService, address: str, extra_hosts: tuple[str, ...], auth: AuthSettings) -> DeckWebServer:
    host, sep, port = address.rpartition(":")
    if not sep or not port.isdigit():
        raise typer.BadParameter(f"expected HOST:PORT, got {address!r}", param_hint="--web")
    if host not in LOOPBACK and not auth.enabled:
        logging.getLogger(__name__).warning(
            "web UI on %s is reachable from the network without login; anyone who can open it can configure commands"
            " (see `deck passwd`)",
            host,
        )
    try:
        return start_web(service, host, int(port), extra_hosts=extra_hosts, auth=auth)
    except OSError as e:
        typer.echo(f"error: cannot start the web UI on {address}: {e}", err=True)
        raise typer.Exit(code=1) from e

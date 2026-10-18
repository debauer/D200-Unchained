"""`deck` command line interface."""

from __future__ import annotations

import json
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Annotated

import typer

from d200_deck import __version__
from d200_deck.cli.demo import PALETTE, gradient
from d200_deck.cli.serve import serve
from d200_deck.constants import DEFAULT_HOST, DEFAULT_PORT
from d200_deck.device.client import DeckClient
from d200_deck.device.protocol import Color
from d200_deck.errors import DeckError, InvalidColorError
from d200_deck.render.images import Fit, load_key_image

BLACK = Color(0, 0, 0)

EPILOG = """\
Examples:\n
  adb forward tcp:7200 tcp:7200\n
  deck info\n
  deck brightness 255\n
  deck fill 0 ff8800\n
  deck image 0 icon.png --fit cover --label Home\n
  deck demo\n
  deck serve --verbose
"""

app = typer.Typer(
    name="deck",
    help="Control the Ulanzi D200 through deckd (display, keys, backlight).",
    epilog=EPILOG,
    no_args_is_help=True,
)


@dataclass(frozen=True)
class Target:
    host: str
    port: int


@contextmanager
def session(ctx: typer.Context) -> Iterator[DeckClient]:
    """Connect to deckd and turn client errors into a clean exit code 1."""
    target: Target = ctx.obj
    try:
        with DeckClient.connect(target.host, target.port) as client:
            yield client
    except (DeckError, OSError) as e:
        typer.echo(f"error: {e}", err=True)
        raise typer.Exit(code=1) from e


def parse_color(text: str) -> Color:
    try:
        return Color.from_hex(text)
    except InvalidColorError as e:
        raise typer.BadParameter(str(e)) from e


def _version(value: bool) -> None:
    if value:
        typer.echo(__version__)
        raise typer.Exit()


BYTE_UNITS = ("B", "KiB", "MiB", "GiB", "TiB")


def human_bytes(value: int) -> str:
    size = float(value)
    for unit in BYTE_UNITS:
        if size < 1024 or unit == BYTE_UNITS[-1]:
            break
        size /= 1024
    return f"{size:.1f}".removesuffix(".0") + f" {unit}"


def human_uptime(seconds: int) -> str:
    days, rest = divmod(seconds, 86400)
    hours, rest = divmod(rest, 3600)
    parts = []
    if days:
        parts.append(f"{days}d")
    if hours or days:
        parts.append(f"{hours}h")
    parts.append(f"{rest // 60}m")
    return " ".join(parts)


def hardware_rows(hw: dict[str, str]) -> list[str]:
    """`deck info` rows for the SYSINFO facts; unreadable facts are skipped."""
    rows: list[tuple[str, str | None]] = []

    def size(key: str) -> int | None:
        value = hw.get(key)
        return int(value) if value is not None else None

    cores, model = hw.get("cpu_cores"), hw.get("cpu_model")
    rows.append(("cpu", f"{cores}x {model}" if cores and model else model or cores))

    kernel, arch = hw.get("kernel"), hw.get("arch")
    rows.append(("kernel", f"{kernel} ({arch})" if kernel and arch else kernel or arch))

    available, total = size("mem_available_kb"), size("mem_total_kb")
    memory = None
    if available is not None and total is not None:
        memory = f"{human_bytes(available * 1024)} available of {human_bytes(total * 1024)}"
    rows.append(("memory", memory))

    storage: list[str] = []
    for mount, prefix in (("/userdata", "userdata"), ("/", "root")):
        free, space = size(f"{prefix}_free_bytes"), size(f"{prefix}_total_bytes")
        if free is not None and space is not None:
            storage.append(f"{mount} {human_bytes(free)} free of {human_bytes(space)}")
    rows.append(("storage", "; ".join(storage) if storage else None))

    uptime = size("uptime_s")
    rows.append(("uptime", human_uptime(uptime) if uptime is not None else None))
    rows.append(("load", hw.get("loadavg")))
    temp = size("temp_mc")
    rows.append(("temp", f"{temp / 1000:.1f} C" if temp is not None else None))
    rows.append(("serial", hw.get("serial")))

    return [f"  {name:<8} {value}" for name, value in rows if value]


@app.callback()
def main_options(
    ctx: typer.Context,
    host: Annotated[str, typer.Option("--host", "-H", envvar="DECK_HOST", help="deckd host.")] = DEFAULT_HOST,
    port: Annotated[int, typer.Option("--port", "-p", envvar="DECK_PORT", help="deckd TCP port.")] = DEFAULT_PORT,
    version: Annotated[
        bool, typer.Option("--version", callback=_version, is_eager=True, help="Show the version and exit.")
    ] = False,
) -> None:
    ctx.obj = Target(host, port)


@app.command()
def info(
    ctx: typer.Context,
    as_json: Annotated[bool, typer.Option("--json", help="Print machine-readable JSON.")] = False,
) -> None:
    """Show protocol version, screen size, key windows and hardware facts."""
    with session(ctx) as client:
        dev = client.hello()
        hw = client.sysinfo()
    if as_json:
        typer.echo(json.dumps(asdict(dev) | {"hardware": hw}))
        return
    typer.echo(f"protocol {dev.version}, screen {dev.width}x{dev.height}, {len(dev.keys)} keys")
    for k in dev.keys:
        typer.echo(f"  key {k.index:2} (hw {k.code:2}): x={k.x:4} y={k.y:3} {k.width}x{k.height}")
    typer.echo("hardware:")
    for row in hardware_rows(hw):
        typer.echo(row)


@app.command()
def brightness(
    ctx: typer.Context,
    value: Annotated[int, typer.Argument(min=0, max=255, help="Backlight level 0-255.")],
) -> None:
    """Set the backlight level."""
    with session(ctx) as client:
        client.set_brightness(value)


@app.command()
def clear(
    ctx: typer.Context,
    color: Annotated[Color, typer.Argument(parser=parse_color, help="Colour as RRGGBB.")],
) -> None:
    """Fill the whole screen with one colour."""
    with session(ctx) as client:
        client.clear(color)


@app.command()
def fill(
    ctx: typer.Context,
    key: Annotated[int, typer.Argument(help="Key number (see `deck info`).")],
    color: Annotated[Color, typer.Argument(parser=parse_color, help="Colour as RRGGBB.")],
) -> None:
    """Fill one key window with a colour."""
    with session(ctx) as client:
        client.fill_key(key, color)


@app.command()
def image(  # noqa: PLR0913, PLR0917
    ctx: typer.Context,
    key: Annotated[int, typer.Argument(help="Key number (see `deck info`).")],
    path: Annotated[Path, typer.Argument(exists=True, dir_okay=False, help="Image file (PNG, JPEG, ...).")],
    fit: Annotated[Fit, typer.Option("--fit", "-f", help="How to fit the image into the key window.")] = Fit.CONTAIN,
    background: Annotated[
        Color, typer.Option("--background", "-b", parser=parse_color, help="Background/transparency colour RRGGBB.")
    ] = "000000",  # type: ignore[assignment]  # parsed by parse_color
    label: Annotated[str, typer.Option("--label", "-l", help="Text drawn in a strip under the image.")] = "",
    label_color: Annotated[
        Color, typer.Option("--label-color", parser=parse_color, help="Label text colour RRGGBB.")
    ] = "ffffff",  # type: ignore[assignment]  # parsed by parse_color
    label_height: Annotated[
        int, typer.Option("--label-height", min=12, max=98, help="Height of the label strip in pixels.")
    ] = 44,
    font: Annotated[
        Path | None, typer.Option("--font", exists=True, dir_okay=False, help="TTF font for the label.")
    ] = None,
) -> None:
    """Show an image file on one key, scaled to its window; optional `--label` under it."""
    with session(ctx) as client:
        try:
            window = client.hello().key(key)
        except KeyError as e:
            typer.echo(f"error: unknown key {key}", err=True)
            raise typer.Exit(code=1) from e
        pixels = load_key_image(
            path,
            window,
            fit,
            background,
            label=label,
            label_color=label_color,
            label_height=label_height,
            font=font,
        )
        client.set_key_image(key, window.width, window.height, pixels)


@app.command()
def events(
    ctx: typer.Context,
    as_json: Annotated[bool, typer.Option("--json", help="Print one JSON object per event.")] = False,
) -> None:
    """Print key presses and releases until Ctrl-C."""
    with session(ctx) as client:
        client.hello()
        try:
            while True:
                ev = client.next_event()
                line = json.dumps(asdict(ev)) if as_json else f"{ev.index} {'down' if ev.pressed else 'up'}"
                typer.echo(line)
        except KeyboardInterrupt:
            return


@app.command()
def demo(ctx: typer.Context) -> None:
    """Draw a gradient on every key; a pressed key turns white until released."""
    white = Color(255, 255, 255)
    with session(ctx) as client:
        dev = client.hello()
        client.clear(BLACK)
        images = {k.index: gradient(k, PALETTE[i % len(PALETTE)]) for i, k in enumerate(dev.keys)}
        for k in dev.keys:
            client.set_key_image(k.index, k.width, k.height, images[k.index])
        typer.echo("demo running, press keys (Ctrl-C to stop)", err=True)
        try:
            while True:
                ev = client.next_event()
                key = dev.key(ev.index)
                if ev.pressed:
                    client.fill_key(key.index, white)
                else:
                    client.set_key_image(key.index, key.width, key.height, images[key.index])
                typer.echo(f"{ev.index} {'down' if ev.pressed else 'up'}")
        except KeyboardInterrupt:
            return


app.command()(serve)


def main() -> int:
    app()
    return 0

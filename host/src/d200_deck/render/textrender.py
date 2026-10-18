"""Render key text on the host, for keys whose text changes (placeholders, clock).

Mirrors web/compose.js so dynamic keys look like the ones the browser renders:
same font, size ratios, wrapping, positions and the shadow over images."""

from __future__ import annotations

from datetime import datetime
from functools import cache
from importlib import resources
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

from d200_deck.device.protocol import Color, rgb565_to_rgb888, rgb888_to_rgb565

MAX_LINES = 3
FONT_RATIO = 0.12
MIN_FONT_RATIO = 0.075
PAD_X_RATIO = 0.055
PAD_Y_RATIO = 0.07
LINE_HEIGHT = 1.15
# Clock in the info window: time and date size relative to the window height.
CLOCK_TIME_RATIO = 0.45
CLOCK_DATE_RATIO = 0.13
CYAN = Color(0, 240, 255)
MAGENTA = Color(255, 43, 214)


def _font_path(weight: int) -> Path:
    return Path(str(resources.files("d200_deck") / "web" / "static" / "fonts" / f"chakra-petch-{weight}.woff2"))


@cache
def font(size: int, weight: int = 700) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(_font_path(weight)), size)


def _base(width: int, height: int, color: Color, icon: bytes | None) -> Image.Image:
    if icon is not None:
        return Image.frombytes("RGB", (width, height), rgb565_to_rgb888(icon))
    return Image.new("RGB", (width, height), (color.r, color.g, color.b))


def wrap(text: str, fnt: ImageFont.FreeTypeFont, width: float) -> list[str]:
    """Greedy word wrap; words wider than a line are broken anywhere."""
    lines: list[str] = []
    for paragraph in text.split("\n"):
        line = ""
        for word in paragraph.split():
            candidate = f"{line} {word}" if line else word
            if fnt.getlength(candidate) <= width:
                line = candidate
                continue
            if line:
                lines.append(line)
            line = ""
            for ch in word:
                if line and fnt.getlength(line + ch) > width:
                    lines.append(line)
                    line = ""
                line += ch
        if line:
            lines.append(line)
    return lines


def _ellipsize(line: str, fnt: ImageFont.FreeTypeFont, width: float) -> str:
    while line and fnt.getlength(f"{line}...") > width:
        line = line[:-1].rstrip()
    return f"{line}..."


def layout(text: str, width: float, height: int, scale: float = 1.0) -> tuple[ImageFont.FreeTypeFont, list[str]]:
    """Largest font that fits MAX_LINES; at the minimum size the last line is cut."""
    size = max(1, round(height * FONT_RATIO * scale))
    minimum = min(size, round(height * MIN_FONT_RATIO))
    while True:
        fnt = font(size)
        lines = wrap(text, fnt, width)
        if len(lines) <= MAX_LINES:
            return fnt, lines
        if size <= minimum:
            kept = lines[:MAX_LINES]
            kept[-1] = _ellipsize(kept[-1], fnt, width)
            return fnt, kept
        size -= 1


def render_key(  # noqa: PLR0913 - one parameter per key property
    width: int,
    height: int,
    *,
    color: Color,
    icon: bytes | None,
    text: str,
    text_color: Color,
    position: str,
    size: int = 100,
) -> bytes:
    """Key image as RGB565: background or icon, then `text` at top/center/bottom;
    `size` in percent of the automatic text size."""
    img = _base(width, height, color, icon)
    text = text.strip()
    if text:
        pad_x, pad_y = round(height * PAD_X_RATIO), round(height * PAD_Y_RATIO)
        fnt, lines = layout(text, width - 2 * pad_x, height, size / 100)
        line_h = fnt.size * LINE_HEIGHT
        total = line_h * len(lines)
        top = pad_y if position == "top" else (height - total) / 2 if position == "center" else height - pad_y - total
        centers = [(width / 2, top + line_h * (i + 0.5)) for i in range(len(lines))]
        if icon is not None:  # shadow keeps text readable over pictures
            shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
            sd = ImageDraw.Draw(shadow)
            dy = max(1, round(height * 0.01))
            for (x, y), line in zip(centers, lines, strict=True):
                sd.text((x, y + dy), line, font=fnt, fill=(0, 0, 0, 204), anchor="mm")
            shadow = shadow.filter(ImageFilter.GaussianBlur(max(1, round(height * 0.03)) / 2))
            img.paste(shadow, (0, 0), shadow)
        draw = ImageDraw.Draw(img)
        for (x, y), line in zip(centers, lines, strict=True):
            draw.text((x, y), line, font=fnt, fill=(text_color.r, text_color.g, text_color.b), anchor="mm")
    return rgb888_to_rgb565(img.tobytes())


def _spaced(  # noqa: PLR0913 - drawing parameters
    draw: ImageDraw.ImageDraw,
    center: tuple[float, float],
    text: str,
    fnt: ImageFont.FreeTypeFont,
    *,
    fill: tuple[int, ...],
    spacing: float,
) -> None:
    """Draw `text` centred with extra letter spacing (CSS letter-spacing)."""
    widths = [fnt.getlength(ch) for ch in text]
    total = sum(widths) + spacing * (len(text) - 1)
    x = center[0] - total / 2
    for ch, w in zip(text, widths, strict=True):
        draw.text((x, center[1]), ch, font=fnt, fill=fill, anchor="lm")
        x += w + spacing


# Fixed English names, independent of the host locale, so the web UI (which formats
# the same way) and the device always agree.
WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")


def date_label(now: datetime) -> str:
    """'Fri 17.10.' - the {date} placeholder and the clock's date line."""
    return f"{WEEKDAYS[now.weekday()]} {now:%d.%m.}"


def render_clock(  # noqa: PLR0913 - one parameter per colour
    width: int, height: int, color: Color, now: datetime, *, time_color: Color = CYAN, date_color: Color = MAGENTA
) -> bytes:
    """Info-window clock: big time with glow in `time_color`, small date in `date_color`."""
    img = _base(width, height, color, None)
    time_font = font(round(height * CLOCK_TIME_RATIO))
    date_font = font(round(height * CLOCK_DATE_RATIO), 500)
    time_rgb, date_rgb = (time_color.r, time_color.g, time_color.b), (date_color.r, date_color.g, date_color.b)
    time_text, date_text = f"{now:%H:%M}", date_label(now).upper()
    gap = height * 0.03
    block = time_font.size + gap + date_font.size
    time_y = (height - block) / 2 + time_font.size / 2
    date_y = time_y + time_font.size / 2 + gap + date_font.size / 2
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    _spaced(
        ImageDraw.Draw(glow),
        (width / 2, time_y),
        time_text,
        time_font,
        fill=(*time_rgb, 200),
        spacing=time_font.size * 0.08,
    )
    glow = glow.filter(ImageFilter.GaussianBlur(height * 0.03))
    img.paste(glow, (0, 0), glow)
    draw = ImageDraw.Draw(img)
    _spaced(draw, (width / 2, time_y), time_text, time_font, fill=time_rgb, spacing=time_font.size * 0.08)
    _spaced(draw, (width / 2, date_y), date_text, date_font, fill=date_rgb, spacing=date_font.size * 0.2)
    return rgb888_to_rgb565(img.tobytes())

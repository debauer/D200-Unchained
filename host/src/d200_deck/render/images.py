"""Turn image files into key-sized RGB565 data (Pillow)."""

from __future__ import annotations

from enum import StrEnum
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps, UnidentifiedImageError

from d200_deck.device.protocol import Color, KeyInfo, rgb888_to_rgb565
from d200_deck.errors import ImageLoadError


class Fit(StrEnum):
    """How an image is fitted into a key window."""

    CONTAIN = "contain"  # keep aspect ratio, whole image visible, background fills the rest
    COVER = "cover"  # keep aspect ratio, fill the window, crop the overflow
    STRETCH = "stretch"  # ignore aspect ratio


DEFAULT_FONT = "DejaVuSans-Bold.ttf"
FONT_DIRS = (Path("/usr/share/fonts"), Path.home() / ".fonts")
LABEL_PADDING = 8
Font = ImageFont.FreeTypeFont | ImageFont.ImageFont


def find_font(name: str = DEFAULT_FONT) -> Path | None:
    """First matching font file below the common font directories, if any."""
    for directory in FONT_DIRS:
        if directory.is_dir():
            match = next(directory.rglob(name), None)
            if match is not None:
                return match
    return None


def load_font(font: Path | None, size: int) -> Font:
    """Load `font` or a default font at `size`; falls back to Pillow's built-in font."""
    path = font if font is not None else find_font()
    if path is not None:
        try:
            return ImageFont.truetype(str(path), size)
        except OSError as e:
            raise ImageLoadError(f"cannot load font {path}: {e}") from e
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1 only has a fixed-size bitmap font
        return ImageFont.load_default()


def render(  # noqa: PLR0913
    image: Image.Image,
    width: int,
    height: int,
    fit: Fit,
    background: Color,
    *,
    label: str = "",
    label_color: Color | None = None,
    label_height: int = 44,
    font: Path | None = None,
) -> Image.Image:
    """Return an RGB image of exactly width x height; transparency is blended onto `background`.
    A non-empty `label` is centred in a strip of `label_height` pixels at the bottom."""
    bg = (background.r, background.g, background.b)
    label = label.strip()
    strip = min(max(label_height, 0), height // 2) if label else 0
    icon_h = height - strip
    rgba = image.convert("RGBA")
    if fit is Fit.COVER:
        rgba = ImageOps.fit(rgba, (width, icon_h), Image.Resampling.LANCZOS)
    elif fit is Fit.STRETCH:
        rgba = rgba.resize((width, icon_h), Image.Resampling.LANCZOS)
    else:
        rgba = ImageOps.contain(rgba, (width, icon_h), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (width, height), bg)
    canvas.paste(rgba, ((width - rgba.width) // 2, (icon_h - rgba.height) // 2), rgba)
    if label:
        draw_label(canvas, label, icon_h, strip, label_color or Color(255, 255, 255), font_path=font)
    return canvas


def draw_label(  # noqa: PLR0913
    canvas: Image.Image, text: str, top: int, strip: int, color: Color, *, font_path: Path | None
) -> None:
    """Draw `text` centred in the strip [top, top + strip); shrink the font to fit, then ellipsize."""
    font, text = fit_label(text, canvas.width, strip, font_path)
    draw = ImageDraw.Draw(canvas)
    left, text_top, right, bottom = draw.textbbox((0, 0), text, font=font)
    x = (canvas.width - (right - left)) // 2 - left
    y = top + (strip - (bottom - text_top)) // 2 - text_top
    draw.text((x, y), text, font=font, fill=(color.r, color.g, color.b))


def fit_label(text: str, width: int, strip: int, font_path: Path | None) -> tuple[Font, str]:
    """Largest font size (down to 9 px) at which `text` fits `width - padding`; ellipsize the rest."""
    limit = max(width - LABEL_PADDING, 1)
    size = max(strip - LABEL_PADDING, 9)
    font = load_font(font_path, size)
    while size > 9 and font.getlength(text) > limit:
        size -= 1
        font = load_font(font_path, size)
    if font.getlength(text) > limit:
        text = ellipsize(text, font, limit)
    return font, text


def ellipsize(text: str, font: Font, limit: float) -> str:
    """Shorten `text` with a trailing `...` until its width fits `limit`."""
    for i in range(len(text) - 1, 0, -1):
        candidate = text[:i].rstrip() + "..."
        if font.getlength(candidate) <= limit:
            return candidate
    return "..."


def to_rgb565(image: Image.Image) -> bytes:
    return rgb888_to_rgb565(image.convert("RGB").tobytes())


def load_key_image(  # noqa: PLR0913
    path: Path,
    key: KeyInfo,
    fit: Fit = Fit.CONTAIN,
    background: Color | None = None,
    *,
    label: str = "",
    label_color: Color | None = None,
    label_height: int = 44,
    font: Path | None = None,
) -> bytes:
    """Load `path` and return RGB565 pixels filling the whole window of `key`."""
    try:
        with Image.open(path) as img:
            img.load()
            rendered = render(
                img,
                key.width,
                key.height,
                fit,
                background or Color(0, 0, 0),
                label=label,
                label_color=label_color,
                label_height=label_height,
                font=font,
            )
    except (OSError, UnidentifiedImageError) as e:
        raise ImageLoadError(f"cannot load image {path}: {e}") from e
    return to_rgb565(rendered)

"""Key image files: names per page, atomic writes, clean-up of deleted pages.

The root page keeps the names used before pages existed (`key-<n>.rgb565`,
`key-<n>-icon.rgb565`, `key-<n>-alt.rgb565`) so existing images and icons stay valid; other pages use
`<page>-key-<n>...`."""

from __future__ import annotations

import logging
import os
import tempfile
from pathlib import Path

from d200_deck.config import ROOT

log = logging.getLogger(__name__)


def _prefix(page: str) -> str:
    return "" if page == ROOT else f"{page}-"


class KeyStore:
    def __init__(self, images_dir: Path) -> None:
        self.dir = images_dir

    def image_name(self, page: str, index: int) -> str:
        return f"{_prefix(page)}key-{index}.rgb565"

    def icon_path(self, page: str, index: int) -> Path:
        """Unlabelled icon kept for the web UI (never shown directly)."""
        return self.dir / f"{_prefix(page)}key-{index}-icon.rgb565"

    def alt_icon_path(self, page: str, index: int) -> Path:
        """Unlabelled icon shown instead of the icon while a colour rule matches."""
        return self.dir / f"{_prefix(page)}key-{index}-alt.rgb565"

    def path(self, name: str) -> Path:
        return self.dir / name

    def write(self, path: Path, data: bytes) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".img-")
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)

    def purge_pages(self, pages: set[str]) -> None:
        """Remove the image and icon files of deleted (non-root) pages."""
        for page in pages - {ROOT}:
            for path in self.dir.glob(f"{page}-key-*.rgb565"):
                path.unlink(missing_ok=True)
                log.info("removed %s (page %r deleted)", path.name, page)

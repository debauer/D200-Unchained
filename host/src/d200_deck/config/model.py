"""Data model of the host configuration: pages of keys, each key with an action."""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from typing import Any

from d200_deck.device.protocol import Color
from d200_deck.errors import UnknownPageError

CONFIG_VERSION = 2
ROOT = "root"
ACTION_COMMAND = "command"
# Device actions without parameters, executed by the host service.
ACTION_BL_UP = "bl_up"
ACTION_BL_DOWN = "bl_down"
ACTION_DISP_OFF = "disp_off"
ACTION_DISP_ON = "disp_on"
DEVICE_ACTIONS = (ACTION_BL_DOWN, ACTION_BL_UP, ACTION_DISP_OFF, ACTION_DISP_ON)
# Navigation: open a subpage / go to the parent page.
ACTION_PAGE = "page"
ACTION_BACK = "back"
NAV_ACTIONS = (ACTION_PAGE, ACTION_BACK)
LABEL_POSITIONS = ("top", "center", "bottom")
LABEL_SIZE_RANGE = (50, 250)
VARIABLE_COMMAND = "command"
DEFAULT_INTERVAL_S = 10
BLACK = Color(0, 0, 0)
WHITE = Color(255, 255, 255)


@dataclass(frozen=True)
class Action:
    """What a key press triggers.

    `command` runs `argv`; `page` opens `target` (empty: not chosen yet); the other
    known types take no parameters. Types this host does not know (e.g. `http`
    from an ESP32 config) are kept with their fields in `extra`, so the config
    survives a round trip, and are ignored on press."""

    type: str
    argv: tuple[str, ...] = ()
    target: str = ""
    extra: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class KeyConfig:
    index: int
    label: str = ""
    color: Color | None = None  # None: the page's default (see DeckConfig.key)
    image: str | None = None  # RGB565 file in the images dir, exactly the key window size
    action: Action | None = None
    label_color: Color | None = None  # None: the page's default; used when the key image is rendered
    label_pos: str = "bottom"
    label_size: int = 100  # percent of the automatic text size


@dataclass(frozen=True)
class ResolvedKey(KeyConfig):
    """A key with its colours resolved (own, else page default, else global/white)."""

    color: Color = BLACK
    label_color: Color = WHITE


@dataclass(frozen=True)
class Variable:
    """A value for `{name}` placeholders in key text.

    `command` is a shell command line run every `interval` seconds; its stdout is the
    value. Types this
    host does not know (e.g. `mqtt` from an ESP32 config) are kept in `extra`."""

    name: str
    type: str = VARIABLE_COMMAND
    command: str = ""
    interval: int = DEFAULT_INTERVAL_S
    extra: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class Page:
    id: str
    name: str = "Default"
    parent: str | None = None
    keys: tuple[KeyConfig, ...] = ()
    color: Color | None = None  # default key background of this page (None: the global background)
    label_color: Color | None = None  # default text colour of this page (None: white)
    accent_color: Color | None = None  # default secondary text colour, e.g. the clock's date (None: magenta)

    def with_key(self, key: KeyConfig) -> Page:
        """Copy with `key` replacing the entry of the same index (or added)."""
        others = tuple(k for k in self.keys if k.index != key.index)
        return replace(self, keys=tuple(sorted((*others, key), key=lambda k: k.index)))


@dataclass(frozen=True)
class DeckConfig:
    brightness: int = 255
    background: Color = BLACK
    pages: tuple[Page, ...] = (Page(ROOT),)
    variables: tuple[Variable, ...] = ()

    def page(self, page_id: str) -> Page:
        for p in self.pages:
            if p.id == page_id:
                return p
        raise UnknownPageError(f"unknown page {page_id!r}")

    def has_page(self, page_id: str) -> bool:
        return any(p.id == page_id for p in self.pages)

    def stored_key(self, index: int, page: str = ROOT) -> KeyConfig:
        """The key as stored: colours not set stay None (inherit the page default)."""
        for k in self.page(page).keys:
            if k.index == index:
                return k
        return KeyConfig(index)

    def key(self, index: int, page: str = ROOT) -> ResolvedKey:
        """The key with its colours resolved: own, else the page default, else global/white."""
        k, p = self.stored_key(index, page), self.page(page)
        return ResolvedKey(
            **{
                **vars(k),
                "color": k.color or p.color or self.background,
                "label_color": k.label_color or p.label_color or WHITE,
            }
        )

    def with_key(self, key: KeyConfig, page: str = ROOT) -> DeckConfig:
        return self.with_page(self.page(page).with_key(key))

    def with_page(self, page: Page) -> DeckConfig:
        """Copy with `page` replacing the page of the same id (or added at the end)."""
        if self.has_page(page.id):
            return replace(self, pages=tuple(page if p.id == page.id else p for p in self.pages))
        return replace(self, pages=(*self.pages, page))

    def children(self, page_id: str) -> tuple[Page, ...]:
        return tuple(p for p in self.pages if p.parent == page_id)

    def subtree(self, page_id: str) -> set[str]:
        """`page_id` and all pages below it."""
        found = {page_id}
        grew = True
        while grew:
            below = {p.id for p in self.pages if p.parent in found} - found
            found |= below
            grew = bool(below)
        return found

    def without_subtree(self, page_id: str) -> DeckConfig:
        """Copy without `page_id` and its subpages; `page` actions aiming at them are cleared."""
        if page_id == ROOT:
            raise ValueError("the root page cannot be deleted")
        gone = self.subtree(page_id)

        def unlink(k: KeyConfig) -> KeyConfig:
            a = k.action
            if a is not None and a.type == ACTION_PAGE and a.target in gone:
                return replace(k, action=replace(a, target=""))
            return k

        kept = tuple(replace(p, keys=tuple(unlink(k) for k in p.keys)) for p in self.pages if p.id not in gone)
        return replace(self, pages=kept)

    def linked_pages(self) -> set[str]:
        """Pages some `page` key leads to (the UI marks the others as unreachable)."""
        return {
            k.action.target
            for p in self.pages
            for k in p.keys
            if k.action is not None and k.action.type == ACTION_PAGE and k.action.target
        }

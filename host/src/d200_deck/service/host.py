"""`deck serve`: draw keys from the config, run actions, reload and reconnect."""

from __future__ import annotations

import logging
import threading
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from d200_deck import config as cfgmod
from d200_deck.config import ACTION_PAGE, ROOT, Action, DeckConfig
from d200_deck.config.keystore import KeyStore
from d200_deck.device.client import DeckClient
from d200_deck.device.events import KeyDispatcher
from d200_deck.device.layout import d200_layout
from d200_deck.device.protocol import DeviceInfo, KeyEvent
from d200_deck.errors import (
    ConfigError,
    DeckError,
    UnknownPageError,
)
from d200_deck.render.dynamic import DynamicKeys, builtin_values
from d200_deck.render.renderer import KeyRenderer
from d200_deck.service.brightness import Brightness
from d200_deck.service.hub import EventHub
from d200_deck.service.keyactions import ActionRunner
from d200_deck.service.keyfiles import KeyFiles
from d200_deck.service.variables import VariablePoller

log = logging.getLogger(__name__)

Connect = Callable[[str, int], DeckClient]


@dataclass(frozen=True)
class ServiceSettings:
    host: str
    port: int
    config_path: Path
    images_dir: Path
    poll_s: float = 0.5
    reconnect_s: float = 2.0
    bootstrap: Callable[[], None] | None = None  # bring deckd up (adb) before reconnecting


def _connect(host: str, port: int) -> DeckClient:
    return DeckClient.connect(host, port)


def _mtime(path: Path) -> int | None:
    try:
        return path.stat().st_mtime_ns
    except FileNotFoundError:
        return None


class HostService(KeyFiles):
    def __init__(self, settings: ServiceSettings, connect: Connect = _connect) -> None:
        self.settings = settings
        self._connect = connect
        self.stop = threading.Event()
        self._reload = threading.Event()
        self.config = cfgmod.load(settings.config_path)  # fail fast on a broken config
        self._config_mtime = _mtime(settings.config_path)
        self.hub = EventHub()
        self.info: DeviceInfo = d200_layout()  # last reported layout; the measured one until connected
        self.connected = False
        self._lock = threading.Lock()  # serialises config read-modify-write from web threads
        # Runtime backlight state; the config value is only the start level.
        self.brightness = Brightness(self.config.brightness)
        self.actions = ActionRunner(self.hub, self.brightness, self._set_level, self._navigate)
        self.renderer = KeyRenderer(settings.images_dir)
        self.store = KeyStore(settings.images_dir)
        self.page = ROOT  # page shown on the device
        self._render_due = threading.Event()
        self.poller = VariablePoller(self._values_changed)
        self.dynamic = DynamicKeys(self.store)
        self._minute = -1  # last minute rendered, for {time} and the clock

    def request_reload(self) -> None:
        """Re-read the config at the next poll (e.g. from a SIGHUP handler)."""
        self._reload.set()

    def render(self, client: DeckClient, info: DeviceInfo) -> None:
        """Bring the device up to date; only changed keys are sent (no flicker)."""
        cfg, page, now = self.config, self.page, datetime.now()
        self._minute = now.minute
        self.poller.want(self.dynamic.variables(cfg, page))
        dynamic = self.dynamic.pixels(cfg, info, page, self.poller.values(), now)
        self.renderer.render(client, info, cfg, self.brightness.snapshot().effective, page=page, dynamic=dynamic)

    def values(self) -> dict[str, str]:
        """Current placeholder values, built-ins included (for the web UI)."""
        return {**self.poller.values(), **builtin_values(datetime.now())}

    def _values_changed(self, values: dict[str, str]) -> None:
        self.hub.publish("values", {**values, **builtin_values(datetime.now())})
        self._render_due.set()

    def show_page(self, page_id: str) -> None:
        """Show `page_id` on the device (drawn by the event loop, changed keys only)."""
        if not self.config.has_page(page_id):
            raise UnknownPageError(f"unknown page {page_id!r}")
        self.page = page_id
        self.hub.publish("page", {"page": page_id})
        self._render_due.set()

    def _navigate(self, action: Action) -> str:
        """Run a page/back action; returns the log line (starting with '!' if nothing happened)."""
        if action.type == ACTION_PAGE:
            if not self.config.has_page(action.target):
                return "! page not set"
            self.show_page(action.target)
            return f"-> {self.config.page(action.target).name}"
        parent = self.config.page(self.page).parent
        if parent is None:
            return "! already on the top page"
        self.show_page(parent)
        return f"<- {self.config.page(parent).name}"

    def dispatcher(self, client: DeckClient) -> KeyDispatcher:
        d = KeyDispatcher(client)
        d.on_event(lambda event: self._on_key(event, client))
        return d

    def _on_key(self, event: KeyEvent, client: DeckClient) -> None:
        log.debug("key %d %s", event.index, "down" if event.pressed else "up")
        self.hub.publish("key", {"index": event.index, "pressed": event.pressed})
        # Look the action up at press time: a config saved a moment ago applies at once,
        # even before the event loop gets round to re-rendering.
        action = self.config.key(event.index, self.page).action
        if event.pressed and action is not None:
            self.actions.run(event.index, action, client)

    def _set_level(self, level: int, client: DeckClient | None) -> None:
        """Backlight from the event loop (its client) or from a web thread (own
        connection: deckd serves several clients, the loop's client is not shared)."""
        if client is not None:
            client.set_brightness(level)
            return
        with self._connect(self.settings.host, self.settings.port) as own:
            own.set_brightness(level)

    def _adopt(self, cfg: DeckConfig) -> None:
        """Make `cfg` current; a changed start brightness also sets the runtime level."""
        if cfg.brightness != self.config.brightness:
            self.hub.publish("brightness", self.brightness.set_level(cfg.brightness).to_json())
        self.config = cfg
        if not cfg.has_page(self.page):  # the shown page was deleted
            self.show_page(ROOT)

    def _reload_due(self) -> bool:
        mtime = _mtime(self.settings.config_path)
        changed = mtime != self._config_mtime
        if not (changed or self._reload.is_set()):
            return False
        self._reload.clear()
        self._config_mtime = mtime
        try:
            new = cfgmod.load(self.settings.config_path)
        except ConfigError as e:
            log.error("config not reloaded, keeping the previous one: %s", e)
            return False
        self._adopt(new)
        log.info("config reloaded")
        return True

    def run_session(self) -> None:
        """One connection: render, then dispatch events until stopped or disconnected."""
        s = self.settings
        with self._connect(s.host, s.port) as client:
            info = client.hello()
            log.info("connected to deckd at %s:%d (%d keys)", s.host, s.port, len(info.keys))
            self.info = info
            self._set_connected(True)
            self.renderer.forget()  # unknown device content: full redraw once
            try:
                self.render(client, info)
                dispatcher = self.dispatcher(client)
                while not self.stop.is_set():
                    reloaded = self._reload_due()
                    if reloaded or self._render_due.is_set() or datetime.now().minute != self._minute:
                        self._render_due.clear()
                        self.render(client, info)
                    try:
                        event = client.next_event(timeout=s.poll_s)
                    except TimeoutError:
                        continue
                    dispatcher.dispatch(event)
            finally:
                self._set_connected(False)

    def _set_connected(self, connected: bool) -> None:
        self.connected = connected
        self.hub.publish("status", {"connected": connected})

    def run(self) -> None:
        """Serve until `stop` is set, reconnecting after connection errors (and, if
        configured, bringing deckd up first). Repeated identical errors are logged once."""
        last_problem = ""
        threading.Thread(target=self.poller.run, args=(self.stop,), name="variables", daemon=True).start()
        if self.settings.bootstrap is not None:
            # Once at start even if deckd is reachable: a vendor app that came back
            # (or was never stopped) would keep drawing over deckd.
            try:
                self.settings.bootstrap()
            except (DeckError, OSError) as e:
                log.warning("preparing the device failed: %s", e)
        while not self.stop.is_set():
            try:
                self.run_session()
                last_problem = ""
            except (DeckError, OSError) as e:
                problem = f"deckd not reachable: {e}"
                if self.settings.bootstrap is not None:
                    # Always wait afterwards: a running but incompatible deckd (old protocol)
                    # would otherwise turn this into a busy loop of adb calls.
                    try:
                        self.settings.bootstrap()
                    except (DeckError, OSError) as be:
                        problem = f"{problem}; starting it failed: {be}"
                if problem != last_problem:
                    log.warning("%s (retrying every %.0f s)", problem, self.settings.reconnect_s)
                    last_problem = problem
                self.stop.wait(self.settings.reconnect_s)

        self.hub.close_all()

    # --- operations used by the web API ---------------------------------

    def update_config(self, cfg: DeckConfig) -> None:
        """Save `cfg` and apply it at the next poll."""
        with self._lock:
            self._save(cfg)

    def _save(self, cfg: DeckConfig) -> None:
        removed = {p.id for p in self.config.pages} - {p.id for p in cfg.pages}
        cfgmod.save(self.settings.config_path, cfg)
        self._adopt(cfg)
        self.store.purge_pages(removed)
        self.request_reload()

    def press(self, index: int, page: str = ROOT) -> bool:
        """Run the action of key `index` on `page` as if it was pressed (the web test
        mode): commands are not started, only reported. False if it has no action or
        this host cannot run its type."""
        self.window(index)
        action = self.config.key(index, page).action
        if action is None:
            return False
        return self.actions.run(index, action, None, commands=False)

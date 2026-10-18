"""Page navigation and per-page images in the host service."""

from __future__ import annotations

import json
import threading
import time
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest

from d200_deck.config import Action, KeyConfig, Page
from d200_deck.device.protocol import Msg, parse_info
from d200_deck.errors import UnknownPageError
from d200_deck.service.host import HostService, ServiceSettings

from .conftest import INFO_PAYLOAD, FakeDeck

KEY0 = 196 * 196 * 2
CONFIG = {
    "version": 2,
    "pages": [
        {
            "id": "root",
            "parent": None,
            "keys": [
                {"index": 0, "color": "ff0000", "action": {"type": "page", "target": "lights"}},
                {"index": 1, "color": "00ff00"},
            ],
        },
        {
            "id": "lights",
            "name": "Lights",
            "parent": "root",
            "keys": [{"index": 0, "color": "0000ff", "action": {"type": "back"}}, {"index": 1, "color": "00ff00"}],
        },
    ],
}


def fills(received: list[tuple[int, bytes]]) -> list[bytes]:
    return [p for t, p in received if t == Msg.FILL_KEY]


@pytest.fixture
def running(tmp_path: Path, fake_deck: Callable[..., FakeDeck]) -> Iterator[tuple[HostService, FakeDeck]]:
    cfg = tmp_path / "config.json"
    cfg.write_text(json.dumps(CONFIG))
    deck = fake_deck()
    service = HostService(ServiceSettings("127.0.0.1", deck.port, cfg, tmp_path / "images", poll_s=0.02))
    worker = threading.Thread(target=service.run)
    worker.start()
    try:  # stop the service even if setup fails, or its thread keeps pytest alive
        # key 0: fill; key 1 is the empty info window, which shows the clock (an image)
        deck.wait_for(lambda r: bool(fills(r)) and any(t == Msg.SET_KEY_IMAGE and p[0] == 1 for t, p in r))
        yield service, deck
    finally:
        service.stop.set()
        worker.join(timeout=2)


def test_page_and_back_keys_switch_pages_drawing_only_changes(running: tuple[HostService, FakeDeck]) -> None:
    service, deck = running
    events = service.hub.subscribe()
    before = len(deck.received)
    deck.push(Msg.KEY_EVENT, bytes([0, 0, 1]))  # "page -> lights" on the panel
    deck.wait_for(lambda r: bytes([0, 0, 0, 0, 255]) in fills(r[before:]))
    assert service.page == "lights"
    # key 1 (the clock) looks the same on both pages: not resent
    assert not any(t == Msg.SET_KEY_IMAGE and p[0] == 1 for t, p in deck.received[before:])
    deck.push(Msg.KEY_EVENT, bytes([0, 0, 1]))  # "back" on the lights page
    deck.wait_for(lambda r: bytes([0, 0, 255, 0, 0]) in fills(r[before:]))
    assert service.page == "root"
    seen = []
    while not events.empty():
        seen.append(events.get_nowait())
    assert ("page", {"page": "lights"}) in seen
    assert ("action", {"index": 0, "type": "back", "detail": "<- Default"}) in seen


def test_deleting_the_shown_page_falls_back_to_root(running: tuple[HostService, FakeDeck]) -> None:
    service, deck = running
    service.show_page("lights")
    deck.wait_for(lambda r: bytes([0, 0, 0, 0, 255]) in fills(r))
    service.update_config(service.config.without_subtree("lights"))
    assert service.page == "root"
    with pytest.raises(UnknownPageError):
        service.show_page("lights")


def test_back_on_root_and_unset_target_do_nothing(tmp_path: Path) -> None:
    service = HostService(ServiceSettings("127.0.0.1", 1, tmp_path / "c.json", tmp_path))
    service.info = parse_info(INFO_PAYLOAD)
    keys = (KeyConfig(0, action=Action("back")), KeyConfig(1, action=Action("page", target="")))
    service.update_config(service.config.with_page(Page("root", keys=keys)))
    assert service.press(0) is False
    assert service.press(1) is False
    assert service.page == "root"


def test_subpage_images_have_their_own_files_and_are_purged(tmp_path: Path) -> None:
    images = tmp_path / "images"
    cfg_path = tmp_path / "config.json"
    cfg_path.write_text(json.dumps(CONFIG))
    service = HostService(ServiceSettings("127.0.0.1", 1, cfg_path, images))
    service.info = parse_info(INFO_PAYLOAD)
    root_key = service.store_image(0, b"\x00" * KEY0)
    sub_key = service.store_image(0, b"\x00" * KEY0, page="lights")
    service.store_icon(0, b"\x00" * KEY0, page="lights")
    assert (root_key.image, sub_key.image) == ("key-0.rgb565", "lights-key-0.rgb565")
    assert service.config.key(0, "lights").image == "lights-key-0.rgb565"
    assert (images / "lights-key-0-icon.rgb565").exists()
    service.update_config(service.config.without_subtree("lights"))
    time.sleep(0.01)
    assert sorted(p.name for p in images.iterdir()) == ["key-0.rgb565"]

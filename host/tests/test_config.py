from __future__ import annotations

import json
from pathlib import Path

import pytest

from d200_deck import config as cfgmod
from d200_deck.config import Action, DeckConfig
from d200_deck.config.model import ResolvedKey
from d200_deck.device.protocol import Color
from d200_deck.errors import ConfigError

SAMPLE = {
    "version": 1,
    "brightness": 200,
    "background": "101010",
    "keys": [
        {"index": 0, "label": "Play", "color": "ff8800", "action": {"type": "command", "argv": ["playerctl", "play"]}},
        {"index": 3, "image": "k3.rgb565"},
    ],
}


def test_parse_sample() -> None:
    cfg = cfgmod.parse(SAMPLE)
    assert cfg.brightness == 200
    assert cfg.key(0) == ResolvedKey(
        0, "Play", Color(255, 136, 0), None, Action("command", ("playerctl", "play")), Color(255, 255, 255)
    )
    assert cfg.key(3).color == Color(16, 16, 16)  # falls back to the background
    assert cfg.key(3).image == "k3.rgb565"
    assert cfg.key(9) == ResolvedKey(9, color=Color(16, 16, 16), label_color=Color(255, 255, 255))


def test_roundtrip_through_file(tmp_path: Path) -> None:
    path = tmp_path / "deck" / "config.json"
    cfg = cfgmod.parse(SAMPLE)
    cfgmod.save(path, cfg)
    assert cfgmod.load(path) == cfg
    assert json.loads(path.read_text())["pages"][0]["keys"][0]["action"]["argv"] == ["playerctl", "play"]
    assert [p.name for p in path.parent.iterdir()] == ["config.json"]  # no temp file left behind


def test_missing_file_gives_defaults(tmp_path: Path) -> None:
    assert cfgmod.load(tmp_path / "nope.json") == DeckConfig()


@pytest.mark.parametrize(
    ("data", "message"),
    [
        ([], "JSON object"),
        ({"version": 3}, "unsupported config version"),
        ({"brightness": 300}, "brightness"),
        ({"background": "red"}, "background"),
        ({"keys": [{"label": "x"}]}, "integer 'index'"),
        ({"keys": [{"index": 0}, {"index": 0}]}, "only once"),
        ({"keys": [{"index": 0, "image": "../etc/passwd"}]}, "plain file name"),
        ({"keys": [{"index": 0, "action": {"type": "command", "argv": []}}]}, "non-empty list"),
        ({"keys": [{"index": 0, "action": {"argv": ["x"]}}]}, "action must be"),
        ({"keys": [{"index": 0, "label_pos": "left"}]}, "label_pos"),
        ({"keys": [{"index": 0, "label_color": "white"}]}, "keys"),
        ({"keys": [{"index": 0, "label_size": 20}]}, "label_size must be 50-250"),
    ],
)
def test_invalid_configs(data: object, message: str) -> None:
    with pytest.raises(ConfigError, match=message):
        cfgmod.parse(data)


def test_invalid_json_file(tmp_path: Path) -> None:
    path = tmp_path / "config.json"
    path.write_text("{broken")
    with pytest.raises(ConfigError, match="invalid JSON"):
        cfgmod.load(path)


def test_label_style_and_device_actions_roundtrip() -> None:
    data = {
        "keys": [
            {
                "index": 0,
                "label": "BL+",
                "label_color": "05070b",
                "label_pos": "center",
                "label_size": 180,
                "action": {"type": "bl_up"},
            },
            {"index": 1, "action": {"type": "disp_off"}},
        ]
    }
    cfg = cfgmod.parse(data)
    assert cfg.key(0).label_color == Color(5, 7, 11)
    assert cfg.key(0).label_pos == "center"
    assert cfg.key(0).label_size == 180
    assert cfg.key(0).action == Action("bl_up")
    assert cfg.key(1).label_pos == "bottom"
    assert cfg.key(1).label_color == Color(255, 255, 255)
    assert cfg.key(1).label_size == 100
    assert cfgmod.parse(cfgmod.to_json(cfg)) == cfg


def test_unknown_action_types_are_kept() -> None:
    """An ESP32 config with e.g. an http action must survive a round trip on Linux."""
    data = {"keys": [{"index": 0, "action": {"type": "http", "url": "http://lamp/on", "method": "POST"}}]}
    cfg = cfgmod.parse(data)
    assert cfg.key(0).action == Action("http", extra={"url": "http://lamp/on", "method": "POST"})
    action = cfgmod.to_json(cfg)["pages"][0]["keys"][0]["action"]
    assert action == {"type": "http", "url": "http://lamp/on", "method": "POST"}


PAGES = {
    "version": 2,
    "pages": [
        {
            "id": "root",
            "name": "Default",
            "parent": None,
            "keys": [{"index": 1, "action": {"type": "page", "target": "lights"}}],
        },
        {"id": "lights", "name": "Lights", "parent": "root", "keys": [{"index": 0, "action": {"type": "back"}}]},
        {
            "id": "studio",
            "name": "Studio",
            "parent": "lights",
            "keys": [{"index": 2, "action": {"type": "page", "target": ""}}],
        },
    ],
}


def test_pages_roundtrip_and_navigation_actions() -> None:
    cfg = cfgmod.parse(PAGES)
    assert [p.id for p in cfg.pages] == ["root", "lights", "studio"]
    assert cfg.key(1).action == Action("page", target="lights")
    assert cfg.key(0, "lights").action == Action("back")
    assert cfg.key(5, "studio").label == ""  # unset keys get defaults
    assert [p.id for p in cfg.children("root")] == ["lights"]
    assert cfg.subtree("lights") == {"lights", "studio"}
    assert cfg.linked_pages() == {"lights"}  # "studio" is not reachable: FREE in the UI
    assert cfgmod.parse(cfgmod.to_json(cfg)) == cfg


def test_version_1_becomes_the_root_page() -> None:
    cfg = cfgmod.parse({"version": 1, "keys": [{"index": 3, "label": "old"}]})
    assert [p.id for p in cfg.pages] == ["root"]
    assert cfg.key(3).label == "old"
    assert cfgmod.to_json(cfg)["version"] == 2


def test_deleting_a_page_removes_its_subtree_and_unlinks_keys() -> None:
    cfg = cfgmod.parse(PAGES).without_subtree("lights")
    assert [p.id for p in cfg.pages] == ["root"]
    assert cfg.key(1).action == Action("page", target="")
    with pytest.raises(ValueError, match="root page cannot be deleted"):
        cfg.without_subtree("root")


def _with(pages: list[dict[str, object]]) -> dict[str, object]:
    return {"version": 2, "pages": pages}


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (_with([{"id": "home", "parent": None}]), "page 'root' without parent"),
        (_with([{"id": "root"}, {"id": "a", "parent": "nope"}]), "parent 'nope' does not exist"),
        (_with([{"id": "root"}, {"id": "a", "parent": "b"}, {"id": "b", "parent": "a"}]), "cycle"),
        (_with([{"id": "root"}, {"id": "Bad Id", "parent": "root"}]), "id must match"),
        (_with([{"id": "root"}, {"id": "root"}]), "only once"),
        (_with([{"id": "root", "keys": [{"index": 0, "action": {"type": "page", "target": "x"}}]}]), "does not exist"),
        ({"version": 2, "pages": {"root": {}}}, "pages must be a list"),
    ],
)
def test_invalid_pages(data: object, message: str) -> None:
    with pytest.raises(ConfigError, match=message):
        cfgmod.parse(data)


def test_page_default_colors_apply_to_unset_and_empty_keys() -> None:
    cfg = cfgmod.parse(
        {
            "background": "101010",
            "pages": [
                {"id": "root"},
                {
                    "id": "p",
                    "parent": "root",
                    "color": "203040",
                    "label_color": "00f0ff",
                    "accent_color": "ff00ff",
                    "keys": [{"index": 0}, {"index": 1, "color": "ff0000", "label_color": "000000"}],
                },
            ],
        }
    )
    assert cfg.key(5, "root").color == Color(16, 16, 16)
    empty = cfg.key(5, "p")
    assert (empty.color, empty.label_color) == (Color(32, 48, 64), Color(0, 240, 255))
    assert cfg.page("p").accent_color == Color(255, 0, 255)
    assert cfg.key(0, "p").color == Color(32, 48, 64)
    assert cfg.key(1, "p").color == Color(255, 0, 0)
    assert cfg.key(1, "p").label_color == Color(0, 0, 0)
    assert cfgmod.parse(cfgmod.to_json(cfg)) == cfg
    assert "color" not in cfgmod.to_json(cfg)["pages"][0]
    assert "color" not in cfgmod.to_json(cfg)["pages"][1]["keys"][0]  # inherited, not copied
    changed = cfgmod.parse(
        {
            **cfgmod.to_json(cfg),
            "pages": [{"id": "root"}, {"id": "p", "parent": "root", "color": "000001", "keys": [{"index": 0}]}],
        }
    )
    assert changed.key(0, "p").color == Color(0, 0, 1)


def test_invalid_page_default_color() -> None:
    with pytest.raises(ConfigError, match=r"pages\[0\].*color"):
        cfgmod.parse({"pages": [{"id": "root", "color": "zz"}]})

"""Host configuration (JSON, same format on the Linux host and the ESP32): parse and
validate. The data model is in `model`, serialising and files in `store`."""

from __future__ import annotations

import re
import shlex
from typing import Any

from d200_deck.config.model import (
    ACTION_BACK,
    ACTION_COMMAND,
    ACTION_MQTT,
    ACTION_PAGE,
    CONFIG_VERSION,
    DEVICE_ACTIONS,
    LABEL_POSITIONS,
    LABEL_SIZE_RANGE,
    ROOT,
    VARIABLE_COMMAND,
    VARIABLE_MQTT,
    Action,
    ColorRule,
    DeckConfig,
    KeyConfig,
    Page,
    Variable,
)
from d200_deck.config.placeholders import BUILTINS, NAME
from d200_deck.device.protocol import Color
from d200_deck.errors import ConfigError, InvalidColorError

PAGE_ID = re.compile(r"[a-z0-9_-]{1,32}")
VARIABLE_NAME = re.compile(NAME)
MAX_INTERVAL_S = 3600
MAX_QOS = 2


def _color(value: Any, where: str) -> Color:
    if not isinstance(value, str):
        raise ConfigError(f"{where}: colour must be a string like 'ff8800'")
    try:
        return Color.from_hex(value)
    except InvalidColorError as e:
        raise ConfigError(f"{where}: {e}") from e


def _topic(value: Any, where: str) -> str:
    """An MQTT topic to publish to or read from: a string without wildcards (empty: not chosen yet)."""
    if not isinstance(value, str) or "+" in value or "#" in value:
        raise ConfigError(f"{where}: topic must be a string without the wildcards + and #")
    return value


def _mqtt_action(value: dict[str, Any], where: str) -> Action:
    payload, retain, qos = value.get("payload", ""), value.get("retain", False), value.get("qos", 0)
    if not isinstance(payload, str):
        raise ConfigError(f"{where}: payload must be a string")
    if not isinstance(retain, bool):
        raise ConfigError(f"{where}: retain must be true or false")
    if not isinstance(qos, int) or isinstance(qos, bool) or not 0 <= qos <= MAX_QOS:
        raise ConfigError(f"{where}: qos must be 0, 1 or 2")
    return Action(ACTION_MQTT, topic=_topic(value.get("topic", ""), where), payload=payload, retain=retain, qos=qos)


def _action(value: Any, where: str) -> Action | None:
    if value is None:
        return None
    if not isinstance(value, dict) or not isinstance(value.get("type"), str) or not value["type"]:
        raise ConfigError(f"{where}: action must be an object with a 'type'")
    kind = value["type"]
    if kind == ACTION_COMMAND:
        argv = value.get("argv")
        if not isinstance(argv, list) or not argv or not all(isinstance(a, str) for a in argv):
            raise ConfigError(f"{where}: argv must be a non-empty list of strings")
        return Action(ACTION_COMMAND, tuple(argv))
    if kind == ACTION_PAGE:
        target = value.get("target", "")
        if not isinstance(target, str):
            raise ConfigError(f"{where}: page target must be a page id")
        return Action(ACTION_PAGE, target=target)
    if kind == ACTION_MQTT:
        return _mqtt_action(value, where)
    if kind in DEVICE_ACTIONS or kind == ACTION_BACK:
        return Action(kind)
    return Action(kind, extra={k: v for k, v in value.items() if k != "type"})


def _rules(raw: Any, where: str) -> tuple[ColorRule, ...]:
    if not isinstance(raw, list):
        raise ConfigError(f"{where}: rules must be a list")
    rules = []
    for i, r in enumerate(raw):
        at = f"{where} rules[{i}]"
        if not isinstance(r, dict) or not isinstance(r.get("variable"), str) or not isinstance(r.get("value"), str):
            raise ConfigError(f"{at}: needs a 'variable' and a 'value' (strings)")
        color = _color(r["color"], at) if "color" in r else None
        label_color = _color(r["label_color"], at) if "label_color" in r else None
        rules.append(ColorRule(r["variable"], r["value"], color, label_color))
    return tuple(rules)


def _key(value: Any, where: str) -> KeyConfig:
    if not isinstance(value, dict) or not isinstance(value.get("index"), int):
        raise ConfigError(f"{where}: needs an integer 'index'")
    image = value.get("image")
    if image is not None and (not isinstance(image, str) or "/" in image or image.startswith(".")):
        raise ConfigError(f"{where}: image must be a plain file name in the images directory")
    label = value.get("label", "")
    if not isinstance(label, str):
        raise ConfigError(f"{where}: label must be a string")
    color = _color(value["color"], where) if "color" in value else None
    label_color = _color(value["label_color"], where) if "label_color" in value else None
    label_pos = value.get("label_pos", "bottom")
    if label_pos not in LABEL_POSITIONS:
        raise ConfigError(f"{where}: label_pos must be one of {', '.join(LABEL_POSITIONS)}")
    size = value.get("label_size", 100)
    if not isinstance(size, int) or not LABEL_SIZE_RANGE[0] <= size <= LABEL_SIZE_RANGE[1]:
        raise ConfigError(f"{where}: label_size must be {LABEL_SIZE_RANGE[0]}-{LABEL_SIZE_RANGE[1]} (percent)")
    action = _action(value.get("action"), where)
    rules = _rules(value.get("rules", []), where)
    return KeyConfig(value["index"], label, color, image, action, label_color, label_pos, size, rules)


def _keys(raw: Any, where: str) -> tuple[KeyConfig, ...]:
    if not isinstance(raw, list):
        raise ConfigError(f"{where}: keys must be a list")
    keys = tuple(_key(k, f"{where}keys[{i}]") for i, k in enumerate(raw))
    indices = [k.index for k in keys]
    if len(indices) != len(set(indices)):
        raise ConfigError(f"{where}each key index may appear only once")
    return keys


def _page(value: Any, pos: int) -> Page:
    where = f"pages[{pos}]."
    if not isinstance(value, dict):
        raise ConfigError(f"{where}: page must be an object")
    page_id, parent = value.get("id"), value.get("parent")
    if not isinstance(page_id, str) or not PAGE_ID.fullmatch(page_id):
        raise ConfigError(f"{where}id must match {PAGE_ID.pattern}")
    name = value.get("name", "Default" if page_id == ROOT else page_id)
    if not isinstance(name, str):
        raise ConfigError(f"{where}name must be a string")
    if parent is not None and not isinstance(parent, str):
        raise ConfigError(f"{where}parent must be a page id or null")
    color = _color(value["color"], f"{where}color") if "color" in value else None
    label_color = _color(value["label_color"], f"{where}label_color") if "label_color" in value else None
    keys = _keys(value.get("keys", []), where)
    accent_color = _color(value["accent_color"], f"{where}accent_color") if "accent_color" in value else None
    return Page(page_id, name, parent, keys, color, label_color, accent_color)


def _check_pages(pages: tuple[Page, ...]) -> None:
    ids = [p.id for p in pages]
    if len(ids) != len(set(ids)):
        raise ConfigError("each page id may appear only once")
    by_id = {p.id: p for p in pages}
    if ROOT not in by_id or by_id[ROOT].parent is not None:
        raise ConfigError(f"a page {ROOT!r} without parent is required")
    for p in pages:
        if p.id != ROOT and p.parent not in by_id:
            raise ConfigError(f"page {p.id!r}: parent {p.parent!r} does not exist")
        seen, cur = set(), p
        while cur.parent is not None:  # every chain must end at the root
            if cur.id in seen:
                raise ConfigError(f"page {p.id!r}: parent chain forms a cycle")
            seen.add(cur.id)
            cur = by_id[cur.parent]
        for k in p.keys:
            if k.action and k.action.type == ACTION_PAGE and k.action.target and k.action.target not in by_id:
                raise ConfigError(f"page {p.id!r} key {k.index}: target page {k.action.target!r} does not exist")


def _check_rules(pages: tuple[Page, ...], names: set[str]) -> None:
    known = names | set(BUILTINS)
    for p in pages:
        for k in p.keys:
            for r in k.rules:
                if r.variable not in known:
                    raise ConfigError(f"page {p.id!r} key {k.index}: rule uses unknown variable {r.variable!r}")


def _variable(value: Any, pos: int) -> Variable:
    where = f"variables[{pos}]"
    if not isinstance(value, dict) or not isinstance(value.get("name"), str):
        raise ConfigError(f"{where}: needs a 'name'")
    name, kind = value["name"], value.get("type", VARIABLE_COMMAND)
    if not VARIABLE_NAME.fullmatch(name) or name in BUILTINS:
        raise ConfigError(f"{where}: name must match {NAME} and not be one of {', '.join(BUILTINS)}")
    interval = value.get("interval", 10)
    if not isinstance(interval, int) or not 1 <= interval <= MAX_INTERVAL_S:
        raise ConfigError(f"{where}: interval must be 1-{MAX_INTERVAL_S} seconds")
    if not isinstance(kind, str) or not kind:
        raise ConfigError(f"{where}: type must be a string")
    if kind == VARIABLE_MQTT:
        json_path = value.get("json_path", "")
        if not isinstance(json_path, str):
            raise ConfigError(f"{where}: json_path must be a string")
        extra = {k: v for k, v in value.items() if k not in {"name", "type", "interval", "topic", "json_path"}}
        return Variable(
            name, kind, interval=interval, extra=extra, topic=_topic(value.get("topic", ""), where), json_path=json_path
        )
    if kind != VARIABLE_COMMAND:
        return Variable(
            name,
            kind,
            interval=interval,
            extra={k: v for k, v in value.items() if k not in {"name", "type", "interval"}},
        )
    command, argv = value.get("command"), value.get("argv")  # `argv`: format of earlier versions
    if command is None and isinstance(argv, list) and argv and all(isinstance(a, str) for a in argv):
        command = shlex.join(argv)
    if not isinstance(command, str) or not command.strip():
        raise ConfigError(f"{where}: command must be a non-empty string")
    return Variable(name, kind, command, interval)


def _variables(raw: Any) -> tuple[Variable, ...]:
    if not isinstance(raw, list):
        raise ConfigError("variables must be a list")
    variables = tuple(_variable(v, i) for i, v in enumerate(raw))
    names = [v.name for v in variables]
    if len(names) != len(set(names)):
        raise ConfigError("each variable name may appear only once")
    return variables


def parse(data: Any) -> DeckConfig:
    """Validate config JSON; version 1 (a flat key list) becomes the root page."""
    if not isinstance(data, dict):
        raise ConfigError("config must be a JSON object")
    # Files without a version are v1 unless they already have pages.
    version = data.get("version", CONFIG_VERSION if "pages" in data else 1)
    if version not in (1, CONFIG_VERSION):
        raise ConfigError(f"unsupported config version {version}, expected {CONFIG_VERSION}")
    brightness = data.get("brightness", 255)
    if not isinstance(brightness, int) or not 0 <= brightness <= 255:
        raise ConfigError("brightness must be an integer 0-255")
    background = _color(data.get("background", "000000"), "background")
    if version == 1:
        pages: tuple[Page, ...] = (Page(ROOT, "Default", None, _keys(data.get("keys", []), "")),)
    else:
        raw = data.get("pages", [{"id": ROOT}])
        if not isinstance(raw, list):
            raise ConfigError("pages must be a list")
        pages = tuple(_page(p, i) for i, p in enumerate(raw))
    _check_pages(pages)
    variables = _variables(data.get("variables", []))
    _check_rules(pages, {v.name for v in variables})
    return DeckConfig(brightness, background, pages, variables)

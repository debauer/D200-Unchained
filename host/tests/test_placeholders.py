from __future__ import annotations

import pytest

from d200_deck import config as cfgmod
from d200_deck.config import Variable
from d200_deck.config.placeholders import PENDING, fill, is_dynamic, names
from d200_deck.errors import ConfigError


def test_names_and_literal_braces() -> None:
    assert names("CPU {cpu}% at {time}") == {"cpu", "time"}
    assert names("{{not}} {Cpu} {9x} {}") == set()
    assert not is_dynamic("plain text")
    assert is_dynamic("{date}")


def test_fill_values_pending_and_unknown() -> None:
    text = "{cpu}% {ram} {typo} {{x}}"
    assert fill(text, {"cpu": "42"}, known={"cpu", "ram"}) == f"42% {PENDING} {{typo}} {{x}}"


VARS = {
    "variables": [
        {"name": "cpu", "type": "command", "command": "echo 42", "interval": 5},
        {"name": "lamp", "type": "mqtt", "topic": "home/lamp", "interval": 30},
        {"name": "other", "type": "zigbee", "id": "x"},
    ]
}


def test_legacy_argv_variables_become_a_shell_command() -> None:
    cfg = cfgmod.parse({"variables": [{"name": "v", "argv": ["echo", "a b"]}]})
    assert cfg.variables[0].command == "echo 'a b'"


def test_variables_roundtrip_and_unknown_types_kept() -> None:
    cfg = cfgmod.parse(VARS)
    assert cfg.variables == (
        Variable("cpu", "command", "echo 42", 5),
        Variable("lamp", "mqtt", interval=30, topic="home/lamp"),
        Variable("other", "zigbee", interval=10, extra={"id": "x"}),
    )
    assert cfgmod.parse(cfgmod.to_json(cfg)) == cfg
    assert cfgmod.parse({}).variables == ()


@pytest.mark.parametrize(
    ("variables", "message"),
    [
        ([{"name": "time", "command": "x"}], "not be one of time, date"),
        ([{"name": "Bad Name", "command": "x"}], "name must match"),
        ([{"name": "cpu", "command": " "}], "non-empty string"),
        ([{"name": "cpu", "command": "x", "interval": 0}], "interval must be 1-3600"),
        ([{"name": "cpu", "command": "x"}, {"name": "cpu", "command": "y"}], "only once"),
        ({"cpu": {}}, "variables must be a list"),
    ],
)
def test_invalid_variables(variables: object, message: str) -> None:
    with pytest.raises(ConfigError, match=message):
        cfgmod.parse({"variables": variables})

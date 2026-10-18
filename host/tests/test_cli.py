from __future__ import annotations

import json
from collections.abc import Callable

from typer.testing import CliRunner

from d200_deck.cli.main import app, hardware_rows, human_bytes, human_uptime
from d200_deck.device.protocol import Msg

from .conftest import FakeDeck

runner = CliRunner()


def test_info_json(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck()
    result = runner.invoke(app, ["--port", str(deck.port), "info", "--json"])
    assert result.exit_code == 0, result.output
    data = json.loads(result.stdout)
    assert data["width"] == 1280
    assert [k["index"] for k in data["keys"]] == [0, 1]
    assert data["hardware"]["cpu_cores"] == "4"
    assert data["hardware"]["mem_total_kb"] == "118520"


def test_info_shows_hardware(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck()
    result = runner.invoke(app, ["--port", str(deck.port), "info"])
    assert result.exit_code == 0, result.output
    assert "hardware:" in result.stdout
    assert "4x Test CPU" in result.stdout
    assert "kernel   5.10.160 (arm)" in result.stdout
    assert "available of 115.7 MiB" in result.stdout
    assert "/userdata 47 MiB free of 52 MiB" in result.stdout
    assert "42.1 C" in result.stdout
    assert "D200TEST" in result.stdout


def test_human_formatting() -> None:
    assert human_bytes(0) == "0 B"
    assert human_bytes(1024) == "1 KiB"
    assert human_bytes(49283072) == "47 MiB"
    assert human_bytes(118520 * 1024) == "115.7 MiB"
    assert human_uptime(3661) == "1h 1m"
    assert human_uptime(90061) == "1d 1h 1m"


def test_hardware_rows_skip_missing_facts() -> None:
    rows = hardware_rows({"cpu_cores": "4", "cpu_model": "Test CPU", "mem_total_kb": "1024"})
    assert "  cpu      4x Test CPU" in rows
    assert all("None" not in row for row in rows)


def test_fill_sends_colour(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck()
    result = runner.invoke(app, ["--port", str(deck.port), "fill", "1", "ff8800"])
    assert result.exit_code == 0, result.output
    assert deck.received == [(Msg.FILL_KEY, bytes([1, 0, 0xFF, 0x88, 0x00]))]


def test_invalid_colour_is_rejected() -> None:
    result = runner.invoke(app, ["fill", "30", "orange"])
    assert result.exit_code == 2
    assert "expected RRGGBB" in result.output


def test_device_error_exits_1(fake_deck: Callable[..., FakeDeck]) -> None:
    deck = fake_deck(lambda t, p: [(Msg.ERROR, b"unknown key code 99")])
    result = runner.invoke(app, ["--port", str(deck.port), "fill", "99", "000000"])
    assert result.exit_code == 1
    assert "unknown key code 99" in result.stderr


def test_connection_refused_exits_1() -> None:
    result = runner.invoke(app, ["--port", "1", "info"])
    assert result.exit_code == 1
    assert "error:" in result.stderr

from __future__ import annotations

import json
import shlex
import sys
import threading
from collections.abc import Callable
from datetime import datetime
from pathlib import Path

from d200_deck.config import DeckConfig, KeyConfig, Page, Variable
from d200_deck.config.keystore import KeyStore
from d200_deck.device.protocol import Msg, parse_info
from d200_deck.render.dynamic import DynamicKeys
from d200_deck.service.host import HostService, ServiceSettings
from d200_deck.service.variables import ERROR_VALUE, UNSUPPORTED_VALUE, VariablePoller, run_command

from .conftest import INFO_PAYLOAD, FakeDeck

PY = sys.executable
INFO = parse_info(INFO_PAYLOAD)  # key 0: 196x196, key 1: wide info window


def test_run_command_output_exit_code_timeout_missing() -> None:
    assert run_command(Variable("v", command=shlex.join([PY, "-c", "print('  42 %  ')"]))) == "42 %"
    assert run_command(Variable("v", command=shlex.join([PY, "-c", "print('x' * 100)"]))) == "x" * 64
    assert run_command(Variable("v", command=shlex.join([PY, "-c", "import sys; sys.exit(3)"]))) == ERROR_VALUE
    assert (
        run_command(Variable("v", command=shlex.join([PY, "-c", "import time; time.sleep(5)"])), timeout_s=0.2)
        == ERROR_VALUE
    )
    assert run_command(Variable("v", command="/no/such/program")) == ERROR_VALUE


def test_poller_runs_at_the_interval_and_reports_changes() -> None:
    now = [0.0]
    outputs = iter(["1", "1", "2"])
    poller = VariablePoller(lambda values: None, runner=lambda v: next(outputs), clock=lambda: now[0])
    poller.want([Variable("cpu", command="x", interval=5), Variable("lamp", "zigbee"), Variable("mq", "mqtt")])
    assert poller.poll_once() is True  # first values
    assert poller.values() == {"cpu": "1", "lamp": UNSUPPORTED_VALUE}  # mqtt: pushed by the broker, not polled
    now[0] = 3
    assert poller.poll_once() is False  # not due yet
    now[0] = 5
    assert poller.poll_once() is False  # due, same value
    now[0] = 10
    assert poller.poll_once() is True
    assert poller.values()["cpu"] == "2"
    poller.want([Variable("lamp", "zigbee")])  # cpu no longer used: its value goes
    assert poller.values() == {"lamp": UNSUPPORTED_VALUE}


def test_dynamic_keys_clock_and_placeholders(tmp_path: Path) -> None:
    dyn = DynamicKeys(KeyStore(tmp_path))
    cfg = DeckConfig(
        pages=(Page("root", keys=(KeyConfig(0, label="CPU {cpu}"),)),),
        variables=(Variable("cpu", command="x"), Variable("ram", command="y")),
    )
    assert [v.name for v in dyn.variables(cfg, "root")] == ["cpu"]  # only what the page uses
    now = datetime(2026, 10, 17, 14, 41)
    pixels = dyn.pixels(cfg, INFO, "root", {"cpu": "42"}, now)
    assert set(pixels) == {0, 1}  # placeholder key and the empty info window (clock)
    assert pixels != dyn.pixels(cfg, INFO, "root", {"cpu": "43"}, now)
    with_text = DeckConfig(pages=(Page("root", keys=(KeyConfig(1, label="Static"),)),))
    assert dyn.pixels(with_text, INFO, "root", {}, now) == {}  # static text: rendered by the browser


def test_service_sends_dynamic_keys_only_when_they_change(tmp_path: Path, fake_deck: Callable[..., FakeDeck]) -> None:
    counter = tmp_path / "n"
    counter.write_text("0")
    script = f"p='{counter}'; n=int(open(p).read()); print(n // 2); open(p,'w').write(str(n + 1))"
    cfg = tmp_path / "config.json"
    data = {
        "version": 2,
        "pages": [{"id": "root", "keys": [{"index": 0, "label": "N={n}"}, {"index": 1, "label": "x"}]}],
        "variables": [{"name": "n", "type": "command", "command": shlex.join([PY, "-c", script]), "interval": 1}],
    }
    cfg.write_text(json.dumps(data))
    deck = fake_deck()
    service = HostService(ServiceSettings("127.0.0.1", deck.port, cfg, tmp_path, poll_s=0.02))
    events = service.hub.subscribe()
    worker = threading.Thread(target=service.run)
    worker.start()
    try:

        def images() -> list[bytes]:
            return [p for t, p in deck.received if t == Msg.SET_KEY_IMAGE and p[0] == 0]

        # The command prints 0, 0, 1, 1, ... (one run per second): the key shows "...",
        # then "0", then "1"; repeated values must not be resent.
        deck.wait_for(lambda r: len(images()) >= 3, timeout=8)
        assert len(set(images())) == len(images())
        seen = []
        while not events.empty():
            item = events.get_nowait()
            if item is not None:  # None ends a stream
                seen.append(item)
        values = [data for kind, data in seen if kind == "values"]
        assert values
        assert {"n", "time", "date"} <= set(values[-1])
    finally:
        service.stop.set()
        worker.join(timeout=3)

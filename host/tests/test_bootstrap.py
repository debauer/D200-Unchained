from __future__ import annotations

import json
import sys
from collections.abc import Callable
from pathlib import Path

import pytest

from d200_deck.errors import AdbError, BootstrapError, DeviceNotFoundError
from d200_deck.service.bootstrap import DEVICE_BINARY, Adb, BootstrapSettings, ensure_deckd

# Mimics the D200's adb: device-side commands never report failures via exit codes.
FAKE_ADB = r"""
import json, sys
state_file, log_file = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
state = json.load(open(state_file))
open(log_file, "a").write(json.dumps(args) + "\n")
def save():
    json.dump(state, open(state_file, "w"))
if args == ["get-state"]:
    if not state["present"]:
        sys.stderr.write("error: no devices/emulators found\n")
        sys.exit(1)
    print("device")
elif args[0] == "shell":
    cmd = args[1]
    if cmd.startswith("killall -STOP WatcherProcess UlanziDeckKey"):
        alive = state["vendor_survives"] > 0
        state["vendor_survives"] -= 1
        save()
        print("vendor:" + ("612 640" if alive else ""))
    elif "netstat" in cmd and state["listening"]:
        print("tcp        0      0 127.0.0.1:7200          0.0.0.0:*               LISTEN")
    elif cmd.startswith("ls ") and state["binary"]:
        print("/userdata/d200/deckd")
    elif "setsid" in cmd and state["binary"] and state["starts"]:
        state["listening"] = True
        save()
    elif cmd.startswith("echo pid"):
        print("pid:\ndeckd: /dev/fb0: Permission denied")
elif args[0] == "push":
    state["binary"] = True
    save()
elif args[0] == "forward":
    pass
else:
    sys.exit(2)
"""


Calls = Callable[[], list[list[str]]]
FakeAdb = Callable[..., tuple[BootstrapSettings, Calls]]


@pytest.fixture
def adb(tmp_path: Path) -> FakeAdb:
    def make(
        present: bool = True,
        listening: bool = False,
        binary: bool = True,
        starts: bool = True,
        vendor_survives: int = 0,
    ) -> tuple[BootstrapSettings, Calls]:
        state, log = tmp_path / "state.json", tmp_path / "adb.log"
        flags = {"present": present, "listening": listening, "binary": binary, "starts": starts}
        state.write_text(json.dumps({**flags, "vendor_survives": vendor_survives}))
        script = tmp_path / "fake_adb.py"
        script.write_text(FAKE_ADB)
        wrapper = tmp_path / "adb"
        wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" -I "{script}" "{state}" "{log}" "$@"\n')
        wrapper.chmod(0o755)

        def calls() -> list[list[str]]:
            return [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []

        return BootstrapSettings(adb=str(wrapper), local_port=17200), calls

    return make


def no_sleep(_: float) -> None:
    return None


def shell_cmds(calls: list[list[str]]) -> list[str]:
    return [c[1] for c in calls if c[0] == "shell"]


def test_running_deckd_still_stops_the_vendor_app(adb: FakeAdb) -> None:
    settings, calls = adb(listening=True)
    ensure_deckd(settings, no_sleep)
    assert calls()[-1] == ["forward", "tcp:17200", "tcp:7200"]
    assert shell_cmds(calls())[0].startswith("killall -STOP WatcherProcess UlanziDeckKey")
    assert not any("setsid" in c for c in shell_cmds(calls()))


def test_vendor_app_is_frozen_before_it_is_killed(adb: FakeAdb) -> None:
    settings, calls = adb(listening=True, vendor_survives=1)
    ensure_deckd(settings, no_sleep)
    stops = [c for c in shell_cmds(calls()) if c.startswith("killall -STOP")]
    assert len(stops) == 2  # survived once, gone after the retry
    assert stops[0].index("-STOP") < stops[0].index("-KILL")
    assert "pidof WatcherProcess UlanziDeckKey" in stops[0]


def test_vendor_app_that_never_dies(adb: FakeAdb) -> None:
    settings, _ = adb(listening=True, vendor_survives=99)
    with pytest.raises(BootstrapError, match="cannot stop the vendor app"):
        ensure_deckd(settings, no_sleep)


def test_starts_deckd_and_stops_vendor_app(adb: FakeAdb) -> None:
    settings, calls = adb()
    ensure_deckd(settings, no_sleep)
    cmds = shell_cmds(calls())
    kill = next(i for i, c in enumerate(cmds) if c.startswith("killall -STOP WatcherProcess UlanziDeckKey"))
    start = next(i for i, c in enumerate(cmds) if c.startswith(f"setsid {DEVICE_BINARY} "))
    assert kill < start
    assert cmds[start].endswith("& sleep 1")  # keeps the old adbd from killing the job
    assert calls()[-1][0] == "forward"


def test_pushes_a_local_binary(adb: FakeAdb, tmp_path: Path) -> None:
    settings, calls = adb(binary=False)
    binary = tmp_path / "deckd"
    binary.write_bytes(b"\x7fELF")
    ensure_deckd(BootstrapSettings(settings.adb, settings.local_port, binary), no_sleep)
    assert ["push", str(binary), DEVICE_BINARY] in calls()
    assert f"chmod 755 {DEVICE_BINARY}" in shell_cmds(calls())


def test_missing_binary_without_local_copy(adb: FakeAdb) -> None:
    settings, _ = adb(binary=False)
    with pytest.raises(BootstrapError, match="missing on the device"):
        ensure_deckd(settings, no_sleep)


def test_no_device(adb: FakeAdb) -> None:
    settings, calls = adb(present=False)
    with pytest.raises(DeviceNotFoundError):
        ensure_deckd(settings, no_sleep)
    assert calls() == [["get-state"]]


def test_deckd_that_never_listens_reports_its_log(
    adb: FakeAdb,
) -> None:
    settings, _ = adb(starts=False)
    with pytest.raises(BootstrapError, match="Permission denied"):
        ensure_deckd(settings, no_sleep)


def test_missing_adb_executable(tmp_path: Path) -> None:
    with pytest.raises(DeviceNotFoundError):
        ensure_deckd(BootstrapSettings(adb=str(tmp_path / "no-adb")), no_sleep)
    with pytest.raises(AdbError, match="no-adb"):
        Adb(str(tmp_path / "no-adb")).run("version")

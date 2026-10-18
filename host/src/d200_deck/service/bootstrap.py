"""Bring deckd up on the D200 via adb: stop the vendor app, install and start deckd, forward the port.

The device's adbd (android-tools 4.2.2) does not forward exit codes of shell
commands and drops background jobs when the shell returns, see docs/hardware.md.
Device-side results are therefore judged by command output only."""

from __future__ import annotations

import logging
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from d200_deck.constants import DEFAULT_PORT
from d200_deck.errors import AdbError, BootstrapError, DeviceNotFoundError

log = logging.getLogger(__name__)

DEVICE_PORT = DEFAULT_PORT
DEVICE_BINARY = "/userdata/d200/deckd"
DEVICE_LOG = "/tmp/deckd.log"
# The vendor app and its watcher restart each other (app.pid / pgrep), so killing one
# after the other can race. Freeze both first, then kill both; pidof (not ps, whose
# output contains this very command line) tells whether any survived.
VENDOR_PROCESSES = ("WatcherProcess", "UlanziDeckKey")
_VENDOR = " ".join(VENDOR_PROCESSES)
STOP_VENDOR = (
    f"killall -STOP {_VENDOR} 2>/dev/null; killall -KILL {_VENDOR} 2>/dev/null; sleep 1; echo vendor:$(pidof {_VENDOR})"
)
STOP_ATTEMPTS = 3
START_CHECKS = 5
ADB_TIMEOUT_S = 30.0


@dataclass(frozen=True)
class BootstrapSettings:
    adb: str = "adb"
    local_port: int = DEFAULT_PORT
    local_binary: Path | None = None  # pushed when deckd is not running yet


class Adb:
    def __init__(self, exe: str) -> None:
        self.exe = exe

    def run(self, *args: str) -> str:
        """Run adb on the host; a non-zero exit code is a host-side failure."""
        try:
            result = subprocess.run(
                [self.exe, *args], capture_output=True, text=True, timeout=ADB_TIMEOUT_S, check=False
            )
        except (OSError, subprocess.TimeoutExpired) as e:
            raise AdbError(f"{self.exe} {' '.join(args)}: {e}") from e
        if result.returncode != 0:
            raise AdbError(f"{self.exe} {' '.join(args)}: {result.stderr.strip() or f'exit {result.returncode}'}")
        return result.stdout

    def shell(self, command: str) -> str:
        """Run a device shell command; only its output is meaningful (no exit codes)."""
        return self.run("shell", command)


def device_present(adb: Adb) -> bool:
    try:
        return adb.run("get-state").strip() == "device"
    except AdbError:
        return False


def deckd_listening(adb: Adb) -> bool:
    return f":{DEVICE_PORT} " in adb.shell("netstat -tln 2>/dev/null")


def stop_vendor_app(adb: Adb) -> None:
    """Stop the stock UI, which would otherwise keep drawing over deckd."""
    for _ in range(STOP_ATTEMPTS):
        output = adb.shell(STOP_VENDOR)
        survivors = output.rsplit("vendor:", 1)[-1].strip()
        if "vendor:" in output and not survivors:
            return
        log.warning("vendor app still running (pids %s), stopping it again", survivors or "?")
    raise BootstrapError(f"cannot stop the vendor app ({_VENDOR}): {output.strip()}")


def ensure_deckd(settings: BootstrapSettings, sleep: Callable[[float], None] = time.sleep) -> None:
    """Make sure the vendor app is gone, deckd runs on the device and the port is forwarded."""
    adb = Adb(settings.adb)
    if not device_present(adb):
        raise DeviceNotFoundError("no D200 connected via adb")
    stop_vendor_app(adb)
    if not deckd_listening(adb):
        _start(adb, settings, sleep)
    adb.run("forward", f"tcp:{settings.local_port}", f"tcp:{DEVICE_PORT}")


def _start(adb: Adb, settings: BootstrapSettings, sleep: Callable[[float], None]) -> None:
    if settings.local_binary is not None:
        log.info("installing %s as %s", settings.local_binary, DEVICE_BINARY)
        adb.shell(f"killall deckd 2>/dev/null; mkdir -p {Path(DEVICE_BINARY).parent}")
        adb.run("push", str(settings.local_binary), DEVICE_BINARY)
        adb.shell(f"chmod 755 {DEVICE_BINARY}")
    if DEVICE_BINARY not in adb.shell(f"ls {DEVICE_BINARY} 2>/dev/null"):
        raise BootstrapError(f"{DEVICE_BINARY} is missing on the device: run install.sh or pass --deckd-binary")
    log.info("starting deckd")
    adb.shell(f"setsid {DEVICE_BINARY} </dev/null >{DEVICE_LOG} 2>&1 & sleep 1")
    for _ in range(START_CHECKS):
        if deckd_listening(adb):
            log.info("deckd is listening on the device")
            return
        sleep(1.0)
    details = adb.shell(f"echo pid: $(pidof deckd); cat {DEVICE_LOG}").strip()
    raise BootstrapError(f"deckd did not start listening on port {DEVICE_PORT}: {details}")

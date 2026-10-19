"""Poll the values of placeholder variables in the background."""

from __future__ import annotations

import logging
import subprocess
import threading
import time
from collections.abc import Callable, Iterable

from d200_deck.config import VARIABLE_COMMAND, VARIABLE_MQTT, Variable

log = logging.getLogger(__name__)

TIMEOUT_S = 2.0
MAX_LENGTH = 64
ERROR_VALUE = "!"  # shown on the key when a command fails
UNSUPPORTED_VALUE = "?"  # variable type this host cannot read


def run_command(variable: Variable, timeout_s: float = TIMEOUT_S) -> str:
    """Value of a command variable: stdout of `sh -c command`, stripped and cut; ERROR_VALUE on failure."""
    try:
        result = subprocess.run(
            ["/bin/sh", "-c", variable.command],
            capture_output=True,
            text=True,
            timeout=timeout_s,
            check=False,
            stdin=subprocess.DEVNULL,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        log.warning("variable %s: %s", variable.name, e)
        return ERROR_VALUE
    if result.returncode != 0:
        log.warning("variable %s: exit code %d: %s", variable.name, result.returncode, result.stderr.strip()[:200])
        return ERROR_VALUE
    return result.stdout.strip()[:MAX_LENGTH]


class VariablePoller:
    """Runs the wanted variables at their interval; `on_change` gets all values after a change."""

    def __init__(
        self,
        on_change: Callable[[dict[str, str]], None],
        runner: Callable[[Variable], str] = run_command,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._on_change = on_change
        self._run = runner
        self._clock = clock
        self._lock = threading.Lock()
        self._wanted: dict[str, Variable] = {}
        self._due: dict[str, float] = {}
        self._values: dict[str, str] = {}

    def want(self, variables: Iterable[Variable]) -> None:
        """Poll exactly these from now on; new or changed ones right away."""
        wanted = {v.name: v for v in variables}
        with self._lock:
            for name, v in wanted.items():
                if self._wanted.get(name) != v:
                    self._due[name] = 0.0
            self._wanted = wanted
            # Drop values nobody polls any more (renamed, deleted, other page).
            self._values = {k: v for k, v in self._values.items() if k in wanted}

    def values(self) -> dict[str, str]:
        with self._lock:
            return dict(self._values)

    def poll_once(self) -> bool:
        """Run what is due; True if a value changed."""
        now = self._clock()
        with self._lock:
            # mqtt variables are pushed by the broker, see service/mqtt.py
            due = [v for name, v in self._wanted.items() if v.type != VARIABLE_MQTT and self._due.get(name, 0.0) <= now]
            for v in due:
                self._due[v.name] = now + v.interval
        changed = False
        for v in due:
            value = self._run(v) if v.type == VARIABLE_COMMAND else UNSUPPORTED_VALUE
            with self._lock:
                if self._values.get(v.name) != value:
                    self._values[v.name] = value
                    changed = True
        return changed

    def run(self, stop: threading.Event, tick_s: float = 0.25) -> None:
        while not stop.wait(tick_s):
            if self.poll_once():
                self._on_change(self.values())

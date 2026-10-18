"""Run key actions without blocking the event loop."""

from __future__ import annotations

import logging
import subprocess
import threading

from d200_deck.config import ACTION_COMMAND, Action

log = logging.getLogger(__name__)


def run_action(action: Action) -> threading.Thread | None:
    """Start the action; a watcher thread logs its exit code. Returns the watcher
    (for tests) or None if the action could not be started."""
    if action.type != ACTION_COMMAND:
        log.error("unsupported action type %r", action.type)
        return None
    try:
        proc = subprocess.Popen(list(action.argv), stdin=subprocess.DEVNULL)
    except OSError as e:
        log.error("cannot start %s: %s", action.argv[0], e)
        return None
    log.info("started %s (pid %d)", " ".join(action.argv), proc.pid)

    def watch() -> None:
        code = proc.wait()
        if code == 0:
            log.info("%s finished", action.argv[0])
        else:
            log.warning("%s exited with code %d", action.argv[0], code)

    watcher = threading.Thread(target=watch, name=f"action-{proc.pid}", daemon=True)
    watcher.start()
    return watcher

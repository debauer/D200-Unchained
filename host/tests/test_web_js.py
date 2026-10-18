"""Run the JavaScript unit tests of the web UI with node (skipped without node)."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

NODE = shutil.which("node")
JS_TESTS = Path(__file__).parent / "web"


@pytest.mark.skipif(NODE is None, reason="node not installed")
def test_web_ui_js() -> None:
    assert NODE is not None
    result = subprocess.run(
        [NODE, "--test", *sorted(str(p) for p in JS_TESTS.glob("*.test.mjs"))],
        capture_output=True,
        text=True,
        check=False,
        timeout=60,
    )
    assert result.returncode == 0, result.stdout + result.stderr

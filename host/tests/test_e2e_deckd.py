"""End-to-end test against a host build of deckd with fake device files."""

from __future__ import annotations

import os
import socket
import struct
import subprocess
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import pytest

from d200_deck.device.client import DeckClient
from d200_deck.device.protocol import Color, KeyEvent
from d200_deck.errors import ConnectionClosedError, DeviceError

DECKD = Path(__file__).resolve().parents[2] / "device/target/x86_64-unknown-linux-gnu/debug/deckd"
FB_W, FB_H, STRIDE = 720, 1280, 2880
MAX_CLIENTS = 8  # device/src/server.rs

pytestmark = pytest.mark.skipif(not DECKD.exists(), reason=f"host deckd not built: {DECKD}")


@dataclass
class Rig:
    port: int
    fb: Path
    backlight: Path
    input_fd: int

    def fb_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Landscape (x, y) -> RGB from the fake framebuffer."""
        fb_x, fb_y = y, FB_H - 1 - x
        with self.fb.open("rb") as f:
            f.seek(fb_y * STRIDE + fb_x * 4)
            b, g, r, _ = f.read(4)
        return r, g, b

    def press(self, code: int, pressed: bool) -> None:
        os.write(self.input_fd, struct.pack("<IIHHi", 0, 0, 1, code, int(pressed)))


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


@pytest.fixture
def rig(tmp_path: Path) -> Iterator[Rig]:
    sysfs, bl = tmp_path / "sysfs", tmp_path / "bl"
    sysfs.mkdir()
    bl.mkdir()
    (sysfs / "virtual_size").write_text(f"{FB_W},{FB_H}\n")
    (sysfs / "bits_per_pixel").write_text("32\n")
    (sysfs / "stride").write_text(f"{STRIDE}\n")
    (bl / "max_brightness").write_text("255\n")
    (bl / "brightness").write_text("10\n")
    fb = tmp_path / "fb"
    fb.write_bytes(bytes(STRIDE * FB_H))
    fifo = tmp_path / "input"
    os.mkfifo(fifo)
    port = free_port()
    env = dict(os.environ, DECKD_FB=str(fb), DECKD_FB_SYSFS=str(sysfs), DECKD_INPUT=str(fifo), DECKD_BACKLIGHT=str(bl))
    log = (tmp_path / "deckd.log").open("wb")  # inspect on failure: pytest keeps tmp_path
    proc = subprocess.Popen([str(DECKD), f"127.0.0.1:{port}"], env=env, stderr=log)
    input_fd = os.open(fifo, os.O_WRONLY)  # blocks until deckd opened the fifo
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.1).close()
            break
        except OSError:
            time.sleep(0.05)
    try:
        yield Rig(port, fb, bl / "brightness", input_fd)
    finally:
        os.close(input_fd)
        proc.terminate()
        proc.wait(timeout=5)
        log.close()


def test_draw_brightness_and_events(rig: Rig) -> None:
    with DeckClient.connect(port=rig.port) as client:
        info = client.hello()
        assert (info.width, info.height, len(info.keys)) == (1280, 720, 14)
        hw = client.sysinfo()
        assert int(hw["cpu_cores"]) >= 1
        assert int(hw["mem_total_kb"]) > 0
        assert int(hw["uptime_s"]) > 0
        assert hw["arch"] and hw["kernel"]
        assert "root_total_bytes" in hw
        k = info.key(9)
        client.fill_key(9, Color(255, 0, 0))
        assert rig.fb_pixel(k.x + k.width // 2, k.y + k.height // 2) == (255, 0, 0)
        client.set_brightness(128)
        assert rig.backlight.read_text().strip() == "128"
        with pytest.raises(DeviceError, match="unknown key 99"):
            client.fill_key(99, Color(0, 0, 0))
        rig.press(30, True)
        assert client.next_event(timeout=2) == KeyEvent(13, True)


def test_clients_share_the_device(rig: Rig) -> None:
    with DeckClient.connect(port=rig.port) as first, DeckClient.connect(port=rig.port) as second:
        first.hello()
        second.hello()
        # Commands from either client work; replies go only to the sender.
        first.fill_key(0, Color(0, 255, 0))
        second.ping()
        # Key events reach every client that sent HELLO.
        rig.press(7, False)
        assert first.next_event(timeout=2) == KeyEvent(9, False)
        assert second.next_event(timeout=2) == KeyEvent(9, False)


def test_client_limit(rig: Rig) -> None:
    time.sleep(0.2)  # let deckd drop the fixture's readiness probe connection
    clients = [DeckClient.connect(port=rig.port) for _ in range(MAX_CLIENTS)]
    try:
        for c in clients:
            c.ping()
        with DeckClient.connect(port=rig.port) as extra, pytest.raises((DeviceError, ConnectionClosedError, OSError)):
            extra.ping()
    finally:
        for c in clients:
            c.close()

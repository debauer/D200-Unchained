"""Find the deckd binary for the D200 and generate main/deckd_blob.S that embeds it.

deckd (ARMv7 musl) comes from ../install.sh or `cargo build --release --target
armv7-unknown-linux-musleabihf` in ../device; DECKD_BINARY overrides the search."""

import os
import sys
from pathlib import Path

Import("env")  # noqa: F821  (provided by PlatformIO)

project = Path(env["PROJECT_DIR"])  # noqa: F821
data_home = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
candidates = [
    os.environ.get("DECKD_BINARY"),
    project.parent / "device/target/armv7-unknown-linux-musleabihf/release/deckd",
    data_home / "deck/deckd",
]
found = next((Path(c) for c in candidates if c and Path(c).is_file()), None)
if found is None:
    sys.stderr.write("deckd binary not found: run ../install.sh or set DECKD_BINARY\n")
    env.Exit(1)  # noqa: F821
blob = project / "main/deckd_blob.S"
source = f""".section .rodata.deckd,"a",@progbits
.balign 4
.global deckd_start
deckd_start:
.incbin "{found.resolve()}"
.global deckd_end
deckd_end:
.byte 0
"""
if not blob.exists() or blob.read_text() != source:
    blob.write_text(source)
print(f"embedding deckd from {found}")

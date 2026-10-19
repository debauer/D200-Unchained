"""Serial monitor that also writes the log to a file (logs/latest.log + logs/<time>.log).

    uv run python scripts/monitor.py [--port /dev/ttyACM0] [--seconds 60]

Without --port it picks the first Espressif/USB-UART port. Stops on Ctrl-C, after --seconds, or when the
port vanishes."""

import argparse
import re
import sys
import time
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Espressif native USB, CP210x, CH340/CH9102, FTDI
KNOWN_VIDS = {0x303A, 0x10C4, 0x1A86, 0x0403}
LOG_DIR = Path(__file__).resolve().parent.parent / "logs"


def find_port() -> str:
    ports = [p for p in list_ports.comports() if p.vid in KNOWN_VIDS]
    if not ports:
        sys.exit("no serial port found: connect the COM port of the board or pass --port")
    return sorted(ports, key=lambda p: p.device)[0].device


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port (default: detect)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, help="stop after this many seconds")
    args = parser.parse_args()

    port = args.port or find_port()
    LOG_DIR.mkdir(exist_ok=True)
    stamped = LOG_DIR / f"{datetime.now():%Y%m%d-%H%M%S}.log"
    latest = LOG_DIR / "latest.log"
    # opened with DTR/RTS low so that opening the port does not reset the board or hold it in the bootloader
    conn = serial.Serial()
    conn.port, conn.baudrate, conn.timeout = port, args.baud, 0.5
    conn.dtr = conn.rts = False
    conn.open()
    print(f"--- {port} @ {args.baud}, log: {latest} (Ctrl-C to stop)", flush=True)
    end = time.monotonic() + args.seconds if args.seconds else None
    buffer = b""
    with stamped.open("w") as log_a, latest.open("w") as log_b:
        try:
            while end is None or time.monotonic() < end:
                try:
                    buffer += conn.read(4096)
                except serial.SerialException as e:
                    print(f"--- port lost: {e}", flush=True)
                    break
                *lines, buffer = buffer.split(b"\n")
                for raw in lines:
                    line = ANSI.sub("", raw.decode("utf-8", "replace")).rstrip("\r")
                    print(line, flush=True)
                    for f in (log_a, log_b):
                        f.write(line + "\n")
                        f.flush()
        except KeyboardInterrupt:
            pass
    conn.close()


if __name__ == "__main__":
    main()

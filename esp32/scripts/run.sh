#!/usr/bin/env bash
# Build, upload and monitor; everything is also written to esp32/logs/ (latest.log = monitor output,
# upload.log = build and upload output).
#
#   scripts/run.sh                 upload, then monitor until Ctrl-C
#   scripts/run.sh --seconds 60    monitor for 60 s only
#   scripts/run.sh --no-upload     only monitor (e.g. after replugging the D200)
#   scripts/run.sh --fs            also upload the web UI (uploadfs)
#   scripts/run.sh --port /dev/ttyACM0   pass the serial port to upload and monitor
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs

upload=1 fs=0 monitor_args=() pio_args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-upload) upload=0 ;;
        --fs) fs=1 ;;
        --seconds) monitor_args+=(--seconds "$2"); shift ;;
        --port) monitor_args+=(--port "$2"); pio_args+=(--upload-port "$2"); shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$upload" = 1 ]; then
    { uv run pio run -t upload "${pio_args[@]}"
      if [ "$fs" = 1 ]; then uv run pio run -t uploadfs "${pio_args[@]}"; fi
    } 2>&1 | tee logs/upload.log
fi
exec uv run python scripts/monitor.py "${monitor_args[@]}"

#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/check-download-mode.sh [--port <serial-port>] [--console-port <serial-port>] [--baud <baud>]

Probe ESP32 ROM download mode with a minimal esptool chip_id command.

Options:
  --port <serial-port>  Serial port to probe. Defaults to EVB_FLASH_PORT,
                        then EVB_SERIAL_PORT, then /dev/ttyS4.
  --console-port <serial-port>
                        Optional live UART console port. Defaults to
                        EVB_SERIAL_PORT when it differs from --port.
  --baud <baud>         Probe baud rate. Defaults to EVB_FLASH_BAUD or
                        115200.
  -h, --help            Show this help text.
EOF
}

die() {
    echo "check-download-mode.sh: $*" >&2
    exit 1
}

require_idf() {
    if [[ -z "${IDF_PATH:-}" ]]; then
        die "IDF_PATH must be set before probing download mode"
    fi

    # shellcheck source=/dev/null
    source "${IDF_PATH}/export.sh" >/dev/null 2>&1
}

capture_console_preview() {
    local serial_port="$1"
    local serial_baud="$2"

    python - "$serial_port" "$serial_baud" <<'PY'
import os
import select
import sys
import termios
import time

port = sys.argv[1]
baud = int(sys.argv[2])

baud_map = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
    230400: termios.B230400,
    460800: termios.B460800,
}

speed = baud_map.get(baud)
if speed is None:
    raise SystemExit(1)

fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)

try:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attrs[3] = 0
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)

    deadline = time.monotonic() + 1.0
    chunks: list[bytes] = []
    total_bytes = 0

    while time.monotonic() < deadline and total_bytes < 256:
        timeout = max(0.0, deadline - time.monotonic())
        readable, _, _ = select.select([fd], [], [], timeout)
        if not readable:
            break

        data = os.read(fd, 256 - total_bytes)
        if not data:
            break

        chunks.append(data)
        total_bytes += len(data)

    payload = b"".join(chunks)
    if not payload:
        raise SystemExit(1)

    preview = payload.decode("utf-8", errors="replace")
    preview = preview.replace("\\", "\\\\").replace("\r", "\\r").replace("\n", "\\n")
    sys.stdout.write(preview[:200])
finally:
    os.close(fd)
PY
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
port="${EVB_FLASH_PORT:-${EVB_SERIAL_PORT:-/dev/ttyS4}}"
console_port="${EVB_SERIAL_PORT:-}"
baud="${EVB_FLASH_BAUD:-115200}"

while (($# > 0)); do
    case "$1" in
    --port)
        port="${2:?missing value for --port}"
        shift 2
        ;;
    --console-port)
        console_port="${2:?missing value for --console-port}"
        shift 2
        ;;
    --baud)
        baud="${2:?missing value for --baud}"
        shift 2
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        die "unknown argument: $1"
        ;;
    esac
done

if [[ "${console_port}" == "${port}" ]]; then
    console_port=""
fi

require_idf

probe_output=""
if ! probe_output="$(
    python -m esptool \
        --chip esp32 \
        --port "${port}" \
        --baud "${baud}" \
        --before default_reset \
        --after hard_reset \
        --no-stub \
        chip_id 2>&1
)"; then
    printf '%s\n' "${probe_output}" >&2

    if [[ -n "${console_port}" ]]; then
        console_preview=""
        if console_preview="$(capture_console_preview "${console_port}" "${baud}" 2>/dev/null)" &&
            [[ -n "${console_preview}" ]]; then
            cat <<EOF >&2

Detected console activity on ${console_port} while probing ${port}.
Preview: ${console_preview}

This suggests ${port} is only the reset/control path while ${console_port}
carries UART data. The runner still is not holding GPIO0 in ROM
download mode during reset.
EOF
        fi
    fi

    cat <<EOF >&2

check-download-mode.sh: Failed to enter ESP32 ROM download mode on ${port}.
This runner is known to expose ambiguous control and console paths.
See ${repo_root}/docs/hardware-reference.md for the current /dev/ttyS4 and
/dev/ttyS5 mapping, expected failure signatures, and the Olimex R46/R14
hardware rework note.
EOF
    exit 1
fi

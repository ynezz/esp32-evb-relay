#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/check-download-mode.sh [--port <serial-port>] [--baud <baud>]

Probe ESP32 ROM download mode with a minimal esptool chip_id command.

Options:
  --port <serial-port>  Serial port to probe. Defaults to EVB_FLASH_PORT,
                        then EVB_SERIAL_PORT, then /dev/ttyS4.
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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
port="${EVB_FLASH_PORT:-${EVB_SERIAL_PORT:-/dev/ttyS4}}"
baud="${EVB_FLASH_BAUD:-115200}"

while (($# > 0)); do
    case "$1" in
    --port)
        port="${2:?missing value for --port}"
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
    cat <<EOF >&2

check-download-mode.sh: Failed to enter ESP32 ROM download mode on ${port}.
This runner is known to expose ambiguous control and console paths.
See ${repo_root}/docs/hardware-reference.md for the current /dev/ttyS4 and
/dev/ttyS5 mapping, expected failure signatures, and the Olimex R46/R14
hardware rework note.
EOF
    exit 1
fi

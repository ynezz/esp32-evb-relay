#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/flash.sh [--port <serial-port>] [--baud <baud>] [idf.py args...]

Flash the production firmware to an ESP32-EVB using the project's
firmware/ tree.

Options:
  --port <serial-port>  Serial port to flash. Defaults to EVB_FLASH_PORT,
                        then EVB_SERIAL_PORT, then /dev/esp32-evb.
  --baud <baud>         Flash baud rate. Defaults to EVB_FLASH_BAUD or
                        115200.
  -h, --help            Show this help text.

Any remaining arguments are passed through to idf.py after the flash
subcommand.
EOF
}

require_idf() {
    if [[ -z "${IDF_PATH:-}" ]]; then
        echo "IDF_PATH must be set before running scripts/flash.sh" >&2
        exit 1
    fi

    # shellcheck source=/dev/null
    source "${IDF_PATH}/export.sh" >/dev/null 2>&1
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
firmware_dir="${repo_root}/firmware"
port="${EVB_FLASH_PORT:-${EVB_SERIAL_PORT:-/dev/esp32-evb}}"
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
    --)
        shift
        break
        ;;
    *)
        break
        ;;
    esac
done

require_idf

download_mode_check="${repo_root}/scripts/check-download-mode.sh"
"${download_mode_check}" --port "${port}" --baud "${baud}"

cd "${firmware_dir}"
exec idf.py -p "${port}" -b "${baud}" flash "$@"

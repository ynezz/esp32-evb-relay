#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/provision.sh [options]

Set, rotate, or clear the production firmware API token by rewriting
only the NVS partition over the ESP32 serial bootloader path.

Options:
  --port <serial-port>  Serial port to use. Defaults to EVB_FLASH_PORT,
                        then EVB_SERIAL_PORT, then /dev/esp32-evb.
  --baud <baud>         Serial baud rate. Defaults to EVB_FLASH_BAUD or
                        115200.
  --token <value>       Provision this API token.
  --generate            Generate a random 32-hex-character API token.
                        This is the default when no token mode is given.
  --clear               Remove the stored API token.
  -h, --help            Show this help text.

The script preserves the currently stored device_cfg keys instead of
writing a fresh NVS partition from defaults.
EOF
}

die() {
    echo "provision.sh: $*" >&2
    exit 1
}

require_idf() {
    if [[ -z "${IDF_PATH:-}" ]]; then
        die "IDF_PATH must be set before running scripts/provision.sh"
    fi

    # shellcheck source=/dev/null
    source "${IDF_PATH}/export.sh" >/dev/null 2>&1
}

generate_token() {
    python3 - <<'PY'
import secrets
print(secrets.token_hex(16))
PY
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
firmware_dir="${repo_root}/firmware"
partition_table="${firmware_dir}/partitions.csv"
port="${EVB_FLASH_PORT:-${EVB_SERIAL_PORT:-/dev/esp32-evb}}"
baud="${EVB_FLASH_BAUD:-115200}"
token=""
mode="generate"

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
    --token)
        token="${2:?missing value for --token}"
        mode="set"
        shift 2
        ;;
    --generate)
        mode="generate"
        shift
        ;;
    --clear)
        mode="clear"
        shift
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

if [[ "${mode}" == "generate" ]]; then
    token="$(generate_token)"
fi

if [[ "${mode}" != "clear" ]]; then
    if [[ -z "${token}" ]]; then
        die "token must not be empty"
    fi
    if ((${#token} > 255)); then
        die "token length must be 255 characters or fewer"
    fi
    if [[ "${token}" =~ [[:space:]] ]]; then
        die "token must not contain whitespace"
    fi
fi

require_idf

download_mode_check="${repo_root}/scripts/check-download-mode.sh"
"${download_mode_check}" --port "${port}" --baud "${baud}"

parttool_py="${IDF_PATH}/components/partition_table/parttool.py"
nvs_gen_py="${IDF_PATH}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
nvs_tool_py="${IDF_PATH}/components/nvs_flash/nvs_partition_tool/nvs_tool.py"
parttool_esptool_args=(--esptool-args no-stub)

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

current_bin="${tmpdir}/nvs-current.bin"
dump_txt="${tmpdir}/nvs-current.txt"
csv_file="${tmpdir}/nvs-updated.csv"
updated_bin="${tmpdir}/nvs-updated.bin"
nvs_size="$(python3 "${parttool_py}" -f "${partition_table}" \
    get_partition_info --partition-name nvs --info size)"

python3 "${parttool_py}" \
    -f "${partition_table}" \
    "${parttool_esptool_args[@]}" \
    -p "${port}" \
    -b "${baud}" \
    read_partition \
    --partition-name nvs \
    --output "${current_bin}"

python3 "${nvs_tool_py}" -d minimal "${current_bin}" > "${dump_txt}"

python3 - "${dump_txt}" "${csv_file}" "${mode}" "${token}" <<'PY'
import csv
import pathlib
import sys

dump_path = pathlib.Path(sys.argv[1])
csv_path = pathlib.Path(sys.argv[2])
mode = sys.argv[3]
token = sys.argv[4]

key_types = {
    "api_token": ("data", "string"),
    "poll_ms": ("data", "u32"),
    "hostname": ("data", "string"),
    "modio_policy": ("data", "u8"),
    "wifi_ssid": ("data", "string"),
    "wifi_pass": ("data", "string"),
    "net_policy": ("data", "u8"),
}

values = {}
seen_other_namespaces = set()

with dump_path.open("rb") as handle:
    for raw_line in handle.read().splitlines():
        line = raw_line.decode("utf-8", errors="strict").replace("\x00", "").strip()
        if not line or line.startswith("Page "):
            continue

        if ":" not in line or " = " not in line:
            raise SystemExit(f"unsupported nvs_tool output: {line!r}")

        namespaced_key, value = line.split(" = ", 1)
        namespace, key = namespaced_key.split(":", 1)
        if namespace != "device_cfg":
            seen_other_namespaces.add(namespace)
            continue

        if key not in key_types:
            raise SystemExit(
                f"unsupported device_cfg key {key!r}; refusing to rewrite NVS"
            )
        values[key] = value

if seen_other_namespaces:
    namespaces = ", ".join(sorted(seen_other_namespaces))
    raise SystemExit(
        f"unsupported NVS namespace(s) present: {namespaces}; refusing to rewrite NVS"
    )

with csv_path.open("w", newline="", encoding="utf-8") as handle:
    writer = csv.writer(handle, lineterminator="\n")
    writer.writerow(["key", "type", "encoding", "value"])
    writer.writerow(["device_cfg", "namespace", "", ""])

    if mode != "clear":
        writer.writerow(["api_token", "data", "string", token])

    for key in ("poll_ms", "hostname", "modio_policy"):
        if key in values:
            row_type, row_encoding = key_types[key]
            writer.writerow([key, row_type, row_encoding, values[key]])
PY

python3 "${nvs_gen_py}" generate "${csv_file}" "${updated_bin}" "${nvs_size}" >/dev/null

python3 "${parttool_py}" \
    -f "${partition_table}" \
    "${parttool_esptool_args[@]}" \
    -p "${port}" \
    -b "${baud}" \
    write_partition \
    --partition-name nvs \
    --input "${updated_bin}"

if [[ "${mode}" == "clear" ]]; then
    echo "Cleared API token on ${port}"
else
    echo "Provisioned API token on ${port}: ${token}"
fi

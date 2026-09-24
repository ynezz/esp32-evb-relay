#!/usr/bin/env bash
# Shared ESP-IDF environment activation helper for scripts/*.sh.
#
# Source this file and call idf_env_activate from a caller that already
# has `set -euo pipefail`. It mirrors the Justfile's `idf_activate`
# recipe variable: if `idf.py` is already on PATH this is a no-op,
# otherwise it sources export.sh from IDF_PATH (or the ~/esp/esp-idf
# default) so scripts behave the same whether they're run directly or
# through `just`.
#
# On success, IDF_PATH is exported so any subprocesses the caller spawns
# (for example scripts/check-download-mode.sh) inherit a working
# ESP-IDF environment too.

idf_env_activate() {
    if command -v idf.py >/dev/null 2>&1; then
        return 0
    fi

    # ESP-IDF's own export.sh declares (and unsets) a same-named shell
    # variable called `idf_path`. Since we source it into this function's
    # scope, using that name here would collide, so a distinct name is
    # used and IDF_PATH is exported before sourcing rather than after.
    local resolved_idf_path="${IDF_PATH:-${HOME}/esp/esp-idf}"
    local export_script="${resolved_idf_path}/export.sh"

    if [[ ! -f "${export_script}" ]]; then
        echo "idf-env: idf.py is not on PATH and no ESP-IDF export.sh was found at" >&2
        echo "idf-env:   ${export_script}" >&2
        echo "idf-env: export IDF_PATH to point at your ESP-IDF checkout, or install" >&2
        echo "idf-env: ESP-IDF at \${HOME}/esp/esp-idf, then retry." >&2
        return 1
    fi

    export IDF_PATH="${resolved_idf_path}"

    # shellcheck source=/dev/null
    if ! source "${export_script}" >/dev/null 2>&1; then
        echo "idf-env: failed to source ${export_script}" >&2
        return 1
    fi

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "idf-env: sourced ${export_script} but idf.py is still not on PATH" >&2
        return 1
    fi
}

set shell := ["bash", "-euo", "pipefail", "-c"]

serial_port := env("EVB_SERIAL_PORT", "/dev/esp32-evb")
serial_baud := env("EVB_SERIAL_BAUD", "115200")
flash_port := env("EVB_FLASH_PORT", serial_port)
test_app_sdkconfig_defaults := env(
    "EVB_TEST_APP_SDKCONFIG_DEFAULTS",
    "sdkconfig.defaults",
)
idf_path := env("IDF_PATH", home_directory() / "esp/esp-idf")
pytest_args := env("EVB_PYTEST_ARGS", "")
test_device_watchdog_seconds := env("EVB_TEST_DEVICE_WATCHDOG_SECONDS", "180")
venv_dir := ".venv"
venv_python := ".venv/bin/python"
venv_astyle_py := ".venv/bin/astyle_py"

# Source ESP-IDF export.sh if idf.py is not already on PATH.
# Recipes that call idf.py should prefix commands with {{idf_activate}}.
idf_activate := "command -v idf.py >/dev/null 2>&1 || . " + idf_path / "export.sh" + " >/dev/null;"

build:
    {{idf_activate}} cd firmware && idf.py build

_ensure-python-tools:
    test -x {{venv_python}} || (echo "ERROR: Python tools not installed. Run: just setup" >&2 && exit 1)
    test -x {{venv_astyle_py}} || (echo "ERROR: astyle_py missing from {{venv_dir}}. Run: just setup" >&2 && exit 1)

test: _ensure-python-tools
    {{idf_activate}} cmake -S firmware/test -B firmware/test/build \
        -DCMAKE_BUILD_TYPE=Debug \
        -DENABLE_SANITIZERS=ON
    {{idf_activate}} cmake --build firmware/test/build
    {{idf_activate}} cd firmware/test/build && ctest --output-on-failure
    {{venv_python}} -m pytest -p no:cacheprovider \
        firmware/test_integration/test_serial_bootloader_config.py

cli-test:
    cd cli && go test -race ./...

cli-lint:
    cd cli && go run github.com/golangci/golangci-lint/v2/cmd/golangci-lint@v2.10.0 run --timeout=5m

cli-vet:
    cd cli && go vet ./...

cli-fmt-check:
    files="$(gofmt -l cli)"; \
    test -z "$files" || (gofmt -d $files && exit 1)

test-device: _ensure-python-tools
    {{idf_activate}} ./scripts/check-download-mode.sh --port {{flash_port}} --baud 115200
    [ ! firmware/test_app/sdkconfig -ot firmware/test_app/{{test_app_sdkconfig_defaults}} ] || \
        rm -f firmware/test_app/sdkconfig
    {{idf_activate}} cd firmware/test_app && \
        idf.py -DSDKCONFIG_DEFAULTS="{{test_app_sdkconfig_defaults}}" build
    {{idf_activate}} cd firmware/test_app && \
        ../../{{venv_python}} ../../scripts/run_with_watchdog.py \
        --timeout-seconds {{test_device_watchdog_seconds}} \
        -- \
        ../../{{venv_python}} -m pytest --target esp32 -p no:cacheprovider \
            pytest_evb_relay.py \
            --esptool-baud 115200 \
            --flash-port {{flash_port}} \
            --port {{serial_port}} \
            {{pytest_args}}

test-integration: _ensure-python-tools
    {{idf_activate}} ./scripts/check-download-mode.sh --port {{flash_port}} --baud 115200
    {{idf_activate}} cd firmware && \
        idf.py build
    {{idf_activate}} cd firmware && \
        ../{{venv_python}} -m pytest --target esp32 -p no:cacheprovider \
        test_integration \
        --port {{flash_port}} \
        --monitor-port {{serial_port}} \
        --monitor-baud {{serial_baud}}

format: _ensure-python-tools
    {{venv_astyle_py}} --astyle-version=3.4.7 --style=otbs \
        --attach-namespaces --attach-classes --indent=spaces=4 \
        --convert-tabs --align-reference=name \
        --keep-one-line-statements --pad-header --pad-oper \
        --unpad-paren --max-continuation-indent=120 \
        $(rg --files firmware -g '*.c' -g '*.h' \
            -g '!firmware/build/**' \
            -g '!firmware/managed_components/**' \
            -g '!firmware/test/build/**' \
            -g '!firmware/test_app/build/**' \
            -g '!firmware/test_app/managed_components/**')

format-check: _ensure-python-tools
    {{venv_astyle_py}} --dry-run --astyle-version=3.4.7 --style=otbs \
        --attach-namespaces --attach-classes --indent=spaces=4 \
        --convert-tabs --align-reference=name \
        --keep-one-line-statements --pad-header --pad-oper \
        --unpad-paren --max-continuation-indent=120 \
        $(rg --files firmware -g '*.c' -g '*.h' \
            -g '!firmware/build/**' \
            -g '!firmware/managed_components/**' \
            -g '!firmware/test/build/**' \
            -g '!firmware/test_app/build/**' \
            -g '!firmware/test_app/managed_components/**')

ci: format-check build test cli-fmt-check cli-lint cli-vet cli-test

ci-full: ci test-device test-integration

setup:
    python3 -m venv {{venv_dir}}
    {{venv_python}} -m pip install --upgrade pip
    {{venv_python}} -m pip install \
        astyle_py==1.0.5 \
        pytest-embedded \
        pytest-embedded-serial-esp \
        pytest-embedded-idf \
        requests
    cp tools/pre-commit-hook.sh .git/hooks/pre-commit
    chmod +x .git/hooks/pre-commit

clean:
    rm -rf firmware/build firmware/test/build \
        firmware/test_app/build firmware/test_app/sdkconfig \
        firmware/test_app/sdkconfig.old \
        firmware/test_app/managed_components \
        firmware/.pytest_cache firmware/test_app/.pytest_cache

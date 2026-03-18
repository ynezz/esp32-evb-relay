serial_port := env("EVB_SERIAL_PORT", "/dev/ttyS4")
test_app_sdkconfig_defaults := env(
    "EVB_TEST_APP_SDKCONFIG_DEFAULTS",
    "sdkconfig.defaults",
)

build:
    cd firmware && idf.py build

test:
    cmake -S firmware/test -B firmware/test/build \
        -DCMAKE_BUILD_TYPE=Debug \
        -DENABLE_SANITIZERS=ON
    cmake --build firmware/test/build
    cd firmware/test/build && ctest --output-on-failure
    python3 -m pytest -p no:cacheprovider \
        firmware/test_integration/test_serial_bootloader_config.py

cli-test:
    cd cli && go test ./...

cli-vet:
    cd cli && go vet ./...

test-device:
    cd firmware/test_app && \
        idf.py -DSDKCONFIG_DEFAULTS="{{test_app_sdkconfig_defaults}}" build
    cd firmware/test_app && \
        pytest --target esp32 -p no:cacheprovider \
        pytest_evb_relay.py \
        --esptool-baud 115200 \
        --port {{serial_port}}

test-integration:
    cd firmware && \
        idf.py build
    cd firmware && \
        pytest --target esp32 -p no:cacheprovider \
        test_integration \
        --port {{serial_port}}

format:
    astyle_py --astyle-version=3.4.7 --style=otbs \
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

format-check:
    astyle_py --dry-run --astyle-version=3.4.7 --style=otbs \
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

ci: format-check build test cli-vet cli-test

ci-full: ci test-device test-integration

setup:
    python3 -m pip install astyle_py==1.0.5 pytest-embedded \
        pytest-embedded-serial-esp pytest-embedded-idf
    cp tools/pre-commit-hook.sh .git/hooks/pre-commit
    chmod +x .git/hooks/pre-commit

clean:
    rm -rf firmware/build firmware/test/build \
        firmware/test_app/build firmware/test_app/sdkconfig \
        firmware/test_app/sdkconfig.old \
        firmware/test_app/managed_components \
        firmware/.pytest_cache firmware/test_app/.pytest_cache

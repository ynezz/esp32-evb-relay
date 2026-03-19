# QA Plan — esp32-evb-relay Firmware

> Comprehensive quality assurance strategy for the ESP32-EVB relay controller
> firmware. Covers testing architecture, per-component test plans, formatting
> enforcement, CI recipes, and infrastructure details.

## Current State

- 5 custom ESP-IDF components (`board`, `relay`, `mod_io`,
  `device_config`, `rest_api`); Tier 1 host tests cover
  `device_config`, `relay`, and `mod_io`, while Tier 2 and Tier 3 test
  projects exist for hardware-backed verification
- Formatting enforcement exists via `astyle_py`, `just format-check`,
  and the repo pre-commit hook
- Build and QA automation exists via `Justfile` recipes including
  `just ci`, `just test-device`, and `just test-integration`
- Hardware-backed QA runs on a self-hosted runner; repo recipes default to
  `/dev/esp32-evb` and can optionally split flash/control
  (`EVB_FLASH_PORT`) from live UART monitoring (`EVB_SERIAL_PORT`)
- Under QEMU/virsh guests, use the stable udev aliases documented in
  `docs/hardware-reference.md` instead of hardcoding `/dev/ttyS*`

### Critical QA Prerequisites

- Hardware-backed test lanes require a dedicated self-hosted runner. The
  public GitHub-hosted CI jobs can only cover host-side build and test work.
- The MOD-IO relay-state model is now settled: the hardware reference documents
  command `0x40` relay-state readback and records a verified-working relay
  sweep on 2026-03-16. QA should assert that authoritative readback behavior,
  not preserve the older write-only unknown/synchronized model.

---

## 1. Three-Tier Test Architecture

Based on ESP-IDF testing best practices and our hardware constraints.

### Tier 1 — Host Unit Tests (Linux, no hardware)

For components whose logic can be tested without real hardware. Fast
feedback loop, runs on any dev machine or CI runner.

Primary approach: standalone CMake with focused stubs. Hand-written
stubs track GPIO levels, I2C transactions, events, and NVS state so
tests can assert behavior without hardware. This keeps all host tests in
one harness instead of splitting coverage across a second, more
experimental Linux-target path.

**Works for:** `device_config`, `relay`, `mod_io`

**Common properties:**
- Uses plain Unity assertions with an explicit `test_main.c` runner
- Exercises public APIs and observable side effects; private static
  helpers are covered indirectly
- Runs via `just test` — fast feedback, no hardware needed
- AddressSanitizer (`-fsanitize=address`) + UBSan
  (`-fsanitize=undefined`) enabled
- Target: < 2 seconds total execution

ESP-IDF's Linux target can still be evaluated later for broader
whole-component smoke coverage, but it should not be the base Tier 1
strategy for this plan.

### Tier 2 — On-Device Unity Tests (ESP32-EVB hardware)

Flash test firmware to the device, run Unity tests via serial.

- Tests real GPIO toggle, real I2C to MOD-IO, real NVS, real HTTP server
- Works for **all 5 components**
- Driven by `pytest-embedded` (`pytest --target esp32`)
- Available on CI via a self-hosted runner with a working flash path
  (`EVB_FLASH_PORT`) and, when needed, a separate UART monitor path
  (`EVB_SERIAL_PORT`)
- Runs via `just test-device`

### Tier 3 — Integration / System Tests

Full firmware + pytest automation that flashes and provisions over the
serial bootloader path, then validates the live system over HTTP.

- End-to-end scenarios: REST API calls → relay state → MOD-IO sync
- Serial access is used for flash/provision setup; runtime assertions are
  HTTP-based against the resolved DUT host
- Establishes authentication deterministically for test runs; do not make
  CI depend on scraping a one-time random token from boot logs or on a
  test-only auth bypass in production firmware
- HTTP client drives REST endpoints from the host
- If the DUT host never resolves after flash/provision, fail the lane
  instead of skipping it
- Runs via `just test-integration`

---

## 2. Per-Component Test Plan

### device_config — Tier 1 (stubs) + Tier 2

**Host tests (Tier 1):**
- `poll_interval_ms` validation: below min (49) → `ESP_ERR_INVALID_ARG`,
  at min (50) → `ESP_OK`, at max (10000) → `ESP_OK`, above max (10001)
  → `ESP_ERR_INVALID_ARG`, default value is 100
- Hostname validation: empty string → reject, single char `"a"` → accept,
  63-char string → accept, 64-char string → reject, leading hyphen →
  reject, trailing hyphen → reject, embedded hyphen → accept, special
  chars (`"host.name"`, `"host_name"`) → reject, alphanumeric → accept
- `modio_boot_policy` parsing: `"leave_unchanged"` → enum 0,
  `"all_off"` → enum 1, `"bogus"` → `ESP_ERR_INVALID_ARG`,
  NULL → `ESP_ERR_INVALID_ARG`
- `modio_boot_policy_to_string`: round-trip with parse
- API token validation: NULL/empty → accept (clears token), 255-char
  string → accept, 256-char string → reject
- Snapshot consistency: after setting values, snapshot reflects them
- Key descriptor lookup: valid keys return non-NULL with correct
  metadata, out-of-range key returns NULL

**On-device tests (Tier 2):**
- NVS persistence: set value, re-init, read back → matches
- Get/set round-trip for all config keys
- Default values correct on fresh NVS

### relay — Tier 1 (stubs) + Tier 2

**Host tests (Tier 1):**
- Invalid relay IDs are rejected through the public API: 0 and 255 reject
  on `relay_set`, `relay_get`, and `relay_toggle`; 1 and 2 remain valid
- Init idempotency: second `relay_init()` returns `ESP_OK` without
  reconfiguring GPIOs
- Init sets all relays OFF: GPIO stub levels for pin 32 and 33 are both 0
  after init
- `relay_set`: set relay 1 ON → GPIO 32 level is 1, set relay 1 OFF →
  GPIO 32 level is 0
- `relay_get`: after set ON, get returns `true`; after set OFF, get
  returns `false`
- `relay_toggle`: from OFF → ON → OFF cycle, GPIO levels match
- NULL pointer rejection: `relay_get(1, NULL)` → `ESP_ERR_INVALID_ARG`
- Uninitialized state rejection: before `relay_init()`, `relay_set()`
  → `ESP_ERR_INVALID_STATE`
- GPIO stubs verify correct pin and level for each operation

**On-device tests (Tier 2):**
- Real GPIO toggle verified by reading back GPIO level
- `relay_init()` sets both relays OFF
- Event loop integration: register handler, toggle relay, verify
  `EVB_RELAY_EVENT_RELAY_CHANGED` event received with correct
  group/id/state

### mod_io — Tier 1 (stubs) + Tier 2

MOD-IO relay-state readback via command `0x40` is authoritative for the
deployed board. The component-level tests below should assert that model
consistently.

**Host tests (Tier 1):**
- I2C stubs capture transaction bytes and verify protocol:
  - Relay write: command `0x10` + 1 byte mask
  - Relay readback: command `0x40`, returns 1 byte
  - Digital input read: command `0x20`, returns 1 byte
  - Analog input read: commands `0x30`–`0x33`, returns 2 bytes each
- State machine transitions: probe → present/readable, absent → probe fails
  → stays absent
- `mod_io_set_relays`: `0x0F` is accepted, `0x10` is rejected, and a
  successful write is observable via readback or the component's refreshed
  cache
- `mod_io_set_relay`: 0 → invalid, 1–4 → valid when MOD-IO is present,
  5 → invalid
- `mod_io_read_analog_input`: 0 → invalid, 1–4 → valid, 5 → invalid
- Public analog read APIs return correctly decoded samples for known
  byte pairs (for example `{0x80, 0x00}` → 1, `{0x01, 0x00}` → 128,
  `{0xFF, 0x03}` → 1023)
- `mod_io_set_relay`: individual relay set modifies correct bit in mask
- Reconciliation on transaction failure: stub returns error → probe
  called → if probe fails, mark absent
- Mutex correctness: operations on uninitialized state →
  `ESP_ERR_INVALID_STATE`

**On-device tests (Tier 2):**
- I2C probe finds device at `0x58`
- Relay set + readback: write mask `0x05`, read back `0x05`
- Digital input read returns valid mask (bits 0–3 only)
- Analog input read: 4 channels return values in 0–1023 range
- All-off after test: write `0x00`; verify readback `0x00`

### board — Tier 2 only

Thin init wrapper with minimal logic to unit test.

**On-device tests (Tier 2):**
- `board_init()` returns `ESP_OK`
- `board_i2c_bus_handle()` returns non-NULL after init
- Idempotent init: second `board_init()` returns `ESP_OK` without error

### rest_api — Tier 2 + Tier 3

**On-device tests (Tier 2):**
- HTTP server starts on configured port
- `rest_api_parse_id_from_uri`: various URI patterns return correct IDs
- `rest_api_modio_sync_to_string`: all enum values → correct strings
- Fail-closed auth regression: `rest_api_start()` refuses to start when no
  auth handler is configured, instead of exposing anonymous access
- Auth handler: mock handler returning `UNAUTHORIZED` → 401 response
- Auth handler: mock handler returning `FORBIDDEN` → 403 response

**Integration tests (Tier 3):**
- pytest fixture resolves device IP and establishes a known valid token
  before authenticated HTTP checks using the same serial provisioning
  path the product already supports
- `GET /api/v1/status` returns valid JSON with expected schema
- Relay state changes via REST API propagate to actual relay state
- Auth token enforcement: requests without token → 401, with valid
  token → 200
- Invalid token path returns the documented auth error (`401` or `403`,
  whichever contract the firmware adopts) and does not expose
  device-context headers
- Authenticated responses include device-context headers;
  pre-auth `401`/`403` responses do not

---

## 3. Test Infrastructure

### Directory Structure

```
firmware/
  test/                              # Host tests (Tier 1)
    CMakeLists.txt                   # Top-level host test CMake project
    test_main.c                      # Unity entry point (runs all suites)
    stubs/
      sdkconfig.h                    # Minimal CONFIG_* defines for host build
      esp_idf_stubs.h               # Stub declarations
      esp_idf_stubs.c               # esp_log, esp_err_to_name, esp_timer
      freertos_stubs.h              # Stub declarations
      freertos_stubs.c              # Semaphore ops (always succeed)
      gpio_stubs.h                  # Test accessors for GPIO state
      gpio_stubs.c                  # gpio_config/set_level + level tracking
      i2c_stubs.h                   # Test accessors for I2C transactions
      i2c_stubs.c                   # I2C master ops + transaction capture
      esp_event_stubs.h             # Stub declarations
      esp_event_stubs.c             # esp_event_post noop + event base defn
      nvs_stubs.h                   # Test accessors for NVS state
      nvs_stubs.c                   # NVS get/set with in-memory backing
    relay/
      test_relay.c                  # Relay host unit tests
    mod_io/
      test_mod_io.c                 # MOD-IO host unit tests
    device_config/
      test_device_config.c          # Device config host unit tests
  test_app/                          # On-device tests (Tier 2)
    CMakeLists.txt                   # IDF project targeting ESP32
    sdkconfig.defaults               # Base device test config
    sdkconfig.ci                     # Optional CI-specific overrides,
                                     # applied via SDKCONFIG_DEFAULTS
    main/
      CMakeLists.txt                 # Component registration
      test_main.c                    # Unity runner (unity_run_menu)
      test_relay_device.c            # On-device relay tests
      test_mod_io_device.c           # On-device MOD-IO tests
      test_board_device.c            # On-device board init tests
      test_device_config_device.c    # On-device config tests
      test_rest_api_device.c         # On-device REST API tests
    pytest_evb_relay.py              # pytest-embedded test driver
  test_integration/                  # Integration tests (Tier 3)
    pytest_integration.py            # System-level pytest scenarios
    conftest.py                      # Shared fixtures (serial provisioning,
                                     # DUT discovery, HTTP client)
```

### Key Patterns from ESP-IDF

- `test_main.c` owns host-side test registration and calls `RUN_TEST(...)`
  explicitly for the standalone Unity harness
- The host harness should reuse Unity from the active ESP-IDF installation
  (via `$IDF_PATH`) instead of vendoring a second copy just for tests
- `dut.run_all_single_board_cases()` in pytest-embedded for Unity
  runner interaction
- `test_app/CMakeLists.txt` should wire `EXTRA_COMPONENT_DIRS` to the real
  `../components` tree so on-device tests exercise production code, not
  copies
- `sdkconfig.ci` (and `sdkconfig.ci.*` later if variants become necessary)
  for CI-specific build configurations
- `#ifdef UNIT_TEST` guard for test-only reset functions in production
  code (allows resetting static state between test cases)
- Dedicated `firmware/test/` host harness keeps stubs and host-only
  glue out of production component builds

### Test Reset Functions

Production source files (`relay.c`, `mod_io.c`, `device_config.c`) need
`#ifdef UNIT_TEST` guarded reset functions to allow test isolation:

```c
#ifdef UNIT_TEST
void relay_reset_for_testing(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_lock = NULL;
}
#endif
```

These reset static state between test cases without requiring process
restart. The `UNIT_TEST` define is set only by the host test
`CMakeLists.txt`.

### Test Hygiene

- Device and integration tests must not depend on execution order or on
  leftover state from prior runs
- Tests that change relay outputs must restore safe defaults before exit
  (`onboard` OFF, `mod_io` all-off or other explicitly documented safe
  state)
- Tests that mutate config or NVS must restore the prior state, or wipe and
  reinitialize the test namespace in fixture teardown
- Shared pytest fixtures should own cross-test cleanup so failures in one
  case do not poison later cases

### Stub Design Principles

**GPIO stubs (`gpio_stubs.c`):**
- Track configured pin modes and current levels in static arrays
- `gpio_config()` records pin_bit_mask and mode
- `gpio_set_level()` records level per pin
- Test accessors: `gpio_stub_get_level(gpio_num)`,
  `gpio_stub_reset()`

**I2C stubs (`i2c_stubs.c`):**
- Capture transmitted bytes into a transaction log
- Configurable return values for probe/transmit/receive
- Pre-loadable receive buffers for read operations
- Test accessors: `i2c_stub_get_last_transaction()`,
  `i2c_stub_set_probe_result()`, `i2c_stub_set_read_data()`,
  `i2c_stub_reset()`

**NVS stubs (`nvs_stubs.c`):**
- In-memory key-value store (hash map or fixed array)
- Supports `nvs_get_str`, `nvs_set_str`, `nvs_get_u8`, `nvs_set_u8`,
  `nvs_get_u32`, `nvs_set_u32`, `nvs_erase_key`, `nvs_commit`,
  `nvs_open`, `nvs_close`
- `nvs_flash_init()` / `nvs_flash_erase()` clear the backing store
- Test accessor: `nvs_stub_reset()`

**FreeRTOS stubs (`freertos_stubs.c`):**
- `xSemaphoreCreateMutexStatic()` returns a non-NULL sentinel
- `xSemaphoreTake()` always returns `pdTRUE`
- `xSemaphoreGive()` is a no-op
- Single-threaded test environment — no actual synchronization needed

**ESP-IDF stubs (`esp_idf_stubs.c`):**
- `esp_log_write()` → `printf` to stderr
- `esp_err_to_name()` → static string lookup for common error codes
- `esp_timer_get_time()` → monotonic clock via `clock_gettime()`
- `esp_event_post()` → no-op returning `ESP_OK` (or capture for
  verification)

---

## 4. Formatting Enforcement

### Tool

`astyle_py==1.0.5` driving AStyle 3.4.7 — matching the current ESP-IDF
pre-commit and `tools/format.sh` configuration.

### Style

OTBS (One True Brace Style), 4-space indent. Follow ESP-IDF's AStyle
rules for mechanical formatting and treat 120 columns as a review
guideline unless a separate line-length check is added.

### Flags

Matching the current ESP-IDF `tools/format.sh` flag set:

```
--astyle-version=3.4.7
--style=otbs
--attach-namespaces
--attach-classes
--indent=spaces=4
--convert-tabs
--align-reference=name
--keep-one-line-statements
--pad-header
--pad-oper
--unpad-paren
--max-continuation-indent=120
```

`--max-continuation-indent=120` limits continuation indentation depth; it
does **not** impose a hard 120-column line-length cap by itself.

### Enforcement Layers

1. **`just format`** — auto-fix all tracked firmware `.c/.h` files,
   including test sources, while excluding generated directories
2. **`just format-check`** — dry-run for CI over that same file set;
   exits non-zero on diff
3. **Pre-commit hook** — shell script at `tools/pre-commit-hook.sh`
   checks staged `firmware/**/*.{c,h}` files
4. **`just ci`** recipe includes `format-check` as first gate

---

## 5. Justfile Recipes

| Recipe              | What                                        | Tier        |
|---------------------|---------------------------------------------|-------------|
| `just build`        | `idf.py build` (ESP32 target)               | Build gate  |
| `just test`         | cmake + ctest host tests + serial bootloader config pytest | Tier 1      |
| `just cli-fmt-check` | `gofmt -l cli` gate                        | CLI gate    |
| `just cli-lint`     | `golangci-lint` v2 over `cli/`              | CLI gate    |
| `just cli-vet`      | `go vet ./...` in `cli/`                    | CLI gate    |
| `just cli-test`     | `go test -race` over CLI packages except `cli/test_e2e` | CLI gate    |
| `just test-e2e`     | `go test -v ./test_e2e/...` in `cli/`       | CLI e2e gate |
| `just test-device`  | download-mode preflight + build `test_app` + pytest-embedded flash/run | Tier 2      |
| `just test-integration` | download-mode preflight + firmware build + pytest-embedded HTTP checks with monitor port/baud | Tier 3      |
| `just format`       | astyle_py auto-fix                          | Format      |
| `just format-check` | astyle_py dry-run                           | Format gate |
| `just ci`           | format-check + build + test + CLI gates + CLI e2e | Full gate   |
| `just ci-full`      | ci + test-device + test-integration         | Full + HW (self-hosted) |
| `just setup`        | create local `.venv`, install QA Python deps, and install hook | One-time    |
| `just clean`        | rm build artifacts                          | Cleanup     |

### Recipe Details

The repo-root [`Justfile`](../Justfile) is the canonical recipe source.
This plan intentionally summarizes the current behaviors that matter for
QA instead of duplicating the whole file verbatim.

Hardware recipes should keep the build and pytest invocation in the same
recipe. `pytest-embedded` will flash and monitor the built app, but the app
still needs to be built first from the matching ESP-IDF project directory.

Justfile recipes that call `idf.py` source `export.sh` automatically when
`idf.py` is not already on `PATH`. Direct `idf.py` commands outside the
Justfile still require an active ESP-IDF environment
(`. $IDF_PATH/export.sh` or equivalent).

For CI hardware lanes, set
`EVB_TEST_APP_SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.ci'` so the
test-app build layers CI-specific overrides on top of the base defaults.

When hardware exposes separate flash/control and live-UART paths, set
`EVB_FLASH_PORT` to the ROM flasher path and `EVB_SERIAL_PORT` to the
console path. If `EVB_FLASH_PORT` is unset, the recipes and helper
scripts fall back to `EVB_SERIAL_PORT` so single-port local setups keep
working unchanged.

Current recipe assumptions and helpers:

- `serial_port` defaults to `/dev/esp32-evb`
- `serial_baud` defaults to `115200`
- `flash_port` falls back to `serial_port` when `EVB_FLASH_PORT` is unset
- `test_app_sdkconfig_defaults` defaults to `sdkconfig.defaults`
- `venv_dir`, `venv_python`, and `venv_astyle_py` point at the repo-local
  `.venv`
- `_ensure-python-tools` validates the local virtualenv before recipes
  that depend on Python tooling
- `idf_activate` sources `export.sh` only when `idf.py` is not already on
  `PATH`

Current QA-critical behaviors:

- `just test` runs both the host CMake/ctest harness and
  `firmware/test_integration/test_serial_bootloader_config.py`
- `just cli-test` covers CLI unit and package tests without
  double-running `cli/test_e2e`
- `just test-e2e` runs the stub-backed CLI subprocess suite in
  `cli/test_e2e`
- `just test-device` and `just test-integration` both run
  `./scripts/check-download-mode.sh --port {{flash_port}} --baud 115200`
  before flashing
- `just test-device` wraps the `pytest-embedded` invocation in a
  repo-owned wall-clock watchdog, configurable via
  `EVB_TEST_DEVICE_WATCHDOG_SECONDS`
- `just test-integration` passes both `--monitor-port {{serial_port}}`
  and `--monitor-baud {{serial_baud}}` to pytest so split-port runners
  and captured UART logs stay aligned
- `just ci` currently expands to
  `format-check + build + test + cli-fmt-check + cli-lint + cli-vet + cli-test + test-e2e`

For the exact up-to-date command bodies, read the repo-root
[`Justfile`](../Justfile).

---

## 6. Pre-commit Hook

Shell script at `tools/pre-commit-hook.sh` that checks the staged
snapshot of `firmware/**/*.{c,h}` files against astyle_py. Same flags as
`just format-check`, but only operates on staged files for speed and so
partial staging is handled correctly.

```bash
#!/usr/bin/env bash
# Pre-commit hook: verify C/H formatting with astyle_py
set -euo pipefail

mapfile -t STAGED_FILES < <(
    git diff --cached --name-only --diff-filter=ACM \
        | grep -E '^firmware/.*\.[ch]$' \
        | grep -Ev '^firmware/(build|managed_components|test/build|test_app/build|test_app/managed_components)/' || true
)

if [ "${#STAGED_FILES[@]}" -eq 0 ]; then
    exit 0
fi

ASTYLE_PY=".venv/bin/astyle_py"

if [ ! -x "$ASTYLE_PY" ]; then
    if ! ASTYLE_PY=$(command -v astyle_py); then
        echo "ERROR: astyle_py not found. Run: just setup"
        exit 1
    fi
fi

ASTYLE_FLAGS="--astyle-version=3.4.7 --style=otbs \
    --attach-namespaces --attach-classes \
    --indent=spaces=4 --convert-tabs --align-reference=name \
    --keep-one-line-statements --pad-header --pad-oper \
    --unpad-paren --max-continuation-indent=120"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

TMP_FILES=()
for path in "${STAGED_FILES[@]}"; do
    mkdir -p "$TMPDIR/$(dirname "$path")"
    git show ":$path" > "$TMPDIR/$path"
    TMP_FILES+=("$TMPDIR/$path")
done

# shellcheck disable=SC2086
if ! "$ASTYLE_PY" --dry-run $ASTYLE_FLAGS "${TMP_FILES[@]}"; then
    echo ""
    echo "Formatting errors detected. Run 'just format' to fix."
    exit 1
fi
```

---

## 7. AGENTS.md Updates

The following changes should be made to `AGENTS.md` (the repo also keeps
`CLAUDE.md` as a symlink to it):

### Replace "Compiler Checks (CRITICAL)" Section

Replace with:

> ### Quality Gates (CRITICAL)
>
> **After any firmware code changes, you MUST run `just ci` before
> committing:**
>
> ```bash
> just ci    # format-check + build + host tests
> ```
>
> If you changed component behavior, also run `just test-device` if
> hardware is available.
>
> If you see errors, **carefully understand and resolve each issue**.
> Read sufficient context to fix them the RIGHT way.

### Add Test Requirements

> ### Test Requirements
>
> - New or changed firmware public APIs MUST have corresponding Tier 1
>   host tests when the code can run in the host harness; otherwise add
>   the narrowest possible Tier 2 regression coverage
> - Bug fixes MUST include a regression test
> - Run `just format` before committing any firmware C/H files

### Update Session Protocol

Add `just ci` step before `git add`:

```bash
just ci                 # Quality gate (MUST pass)
git status              # Check what changed
git add <files>         # Stage code changes
```

### Update Landing the Plane

Replace quality gate step with:

> 2. **Run quality gates** (if code changed) — `just ci` (or
>    `just ci-full` when the self-hosted hardware lane is available)

### Add Commit Prefixes

Add to existing commit prefix list:

- `test:` — test infrastructure, test cases
- `ci:` — CI configuration, Justfile recipes, pre-commit hooks

---

## 8. Dependencies

```bash
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install astyle_py==1.0.5 \
    pytest-embedded \
    pytest-embedded-serial-esp \
    pytest-embedded-idf \
    requests
```

Install these into a project-local virtualenv rather than a managed system
Python. That avoids PEP 668 failures on distro-managed interpreters and keeps
the `just` recipes and pre-commit hook pointed at a deterministic toolchain.

These are needed on both developer machines and CI runners.

### .gitignore Additions

```
firmware/test/build/
firmware/test_app/build/
firmware/test_app/sdkconfig
firmware/test_app/sdkconfig.old
firmware/test_app/managed_components/
firmware/.pytest_cache/
firmware/test_app/.pytest_cache/
```

---

## 9. Implementation Sequence

Each step is a discrete, committable unit of work:

1. Install dependencies (`astyle_py`, `pytest-embedded`)
2. Create host test infrastructure: stubs + `CMakeLists.txt` +
   `test_main.c`
3. Add `#ifdef UNIT_TEST` reset functions to `relay.c`, `mod_io.c`,
   `device_config.c`
4. Write host tests for `device_config` (validation + NVS-backed behavior)
5. Write host tests for `relay` (state machine, GPIO stubs)
6. Write host tests for `mod_io` (I2C stubs, state machine, read/write
   behavior)
7. Create on-device `test_app/` project structure
   and wire it to `../components`
8. Write on-device tests for all 5 components
9. Create pytest driver files (`pytest_evb_relay.py`, `conftest.py`)
   including serial provisioning helpers for integration auth setup
10. Create integration test structure (`test_integration/`)
11. Create `Justfile` with all recipes
12. Create pre-commit hook (`tools/pre-commit-hook.sh`)
13. Update `AGENTS.md` with quality gate requirements
14. Update `.gitignore` with test build directories, test-app IDF
    artifacts, and pytest caches
15. Verify: `just ci` passes, `just test-device` passes,
    `just test-integration` passes

---

## 10. Verification

```bash
. "$IDF_PATH/export.sh"  # Or activate the equivalent ESP-IDF environment
just setup              # Create .venv, install deps, and install hook
just ci                 # format-check + build + host tests
just test-device        # On-device Unity tests (requires hardware)
just test-integration   # System-level pytest (requires hardware)
```

### Success Criteria

- `just ci` passes with zero failures on any Linux machine with ESP-IDF,
  the active IDF Python environment, and the documented QA dependencies
- `just test-device` passes on a self-hosted runner with ESP32-EVB and a
  working `EVB_FLASH_PORT`; set `EVB_SERIAL_PORT` separately when the
  runner exposes a different live UART path
- `just test-integration` passes on that same self-hosted runner with
  working Ethernet access to the DUT and the same flash-port semantics
- `just format-check` exits 0 on all existing firmware source files
- `just format` and `just format-check` cover production and test C/H
  sources with the same file-selection rules
- Pre-commit hook blocks commits with formatting violations
- All 3 testable components have host-level test coverage for their
  public API boundary conditions

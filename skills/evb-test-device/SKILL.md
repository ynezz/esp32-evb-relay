---
name: evb-test-device
description: Run the esp32-evb-relay test tiers as a maintainer. Use when asked to run the quality gate (just ci), host unit tests, on-device Unity tests (just test-device), hardware integration tests (just test-integration), to debug a failing or hanging hardware test run, or to put production firmware and a token back on the bench board after testing.
---

# Test esp32-evb-relay on host and hardware

Run from the repo root after `just setup` (creates `.venv`, installs
pytest-embedded and astyle, installs the pre-commit hook).

## Tiers

| Tier | Command | Needs | Touches the board |
| --- | --- | --- | --- |
| gate | `just ci` | ESP-IDF, Go, `.venv` | no |
| 1 host | `just test` | ESP-IDF (for cmake), `.venv`, ASan/UBSan (`libasan libubsan` on Fedora) | no |
| 2 device | `just test-device` | board on serial | **yes, destructive** |
| 3 integration | `just test-integration` | board on serial and Ethernet | **yes, destructive** |
| all | `just ci-full` | all of the above | yes |

- `just ci` = `format-check`, `build`, `test`, CLI format/lint/vet/tests,
  CLI e2e and the repo checks (`test-repo`). It must pass before every
  commit that touches firmware, CLI, skills or the `Justfile`.
- Tier 1 builds `firmware/test/` natively with hand-written stubs for
  GPIO, I2C, NVS and FreeRTOS, under ASan and UBSan. It covers
  `device_config`, `relay` and `mod_io`, and finishes in seconds.
- Tier 2 flashes `firmware/test_app/` (Unity tests for `board`, `relay`,
  `mod_io`, `device_config`, `rest_api`) over whatever firmware the board
  runs. `test_relay_device.c` switches onboard relay 1.
- Tier 3 builds and flashes the production firmware, provisions a fresh
  API token over serial (replacing the old one), finds the IP from the
  boot log and drives the REST API over HTTP.
- Both hardware tiers run `scripts/check-download-mode.sh` first and fail
  fast when the ROM loader is unreachable.

## Before any hardware tier

1. **Own the board.** Only one agent may use the board at a time. If the
   port is busy (`fuser <port>`) or another session says it is testing,
   wait or ask; never kill someone else's process on the port.
2. **Relays.** On ynezz's bench the relays drive no loads, so agents may
   toggle them freely. On any other bench, ask the user before running
   Tier 2 or 3 or switching relays: they may power real equipment.
3. **Port.** Find it with `skills/_shared/serial-port.md`. Recipes read
   `EVB_SERIAL_PORT` (console, default `/dev/esp32-evb`) and
   `EVB_FLASH_PORT` (esptool, defaults to the console port; differs only
   on split flash/console hardware).
4. Tell the user the board's current firmware and token will be replaced.

## Knobs

| Env var | Default | Use |
| --- | --- | --- |
| `EVB_SERIAL_PORT` | `/dev/esp32-evb` | console / UART |
| `EVB_FLASH_PORT` | `$EVB_SERIAL_PORT` | esptool port |
| `EVB_SERIAL_BAUD` | `115200` | console baud |
| `EVB_TEST_APP_SDKCONFIG_DEFAULTS` | `sdkconfig.defaults` | `sdkconfig.defaults;sdkconfig.ci` layers CI overrides |
| `EVB_TEST_DEVICE_WATCHDOG_SECONDS` | `480` | whole-run watchdog for `just test-device` |
| `EVB_PYTEST_ARGS` | empty | extra pytest args, e.g. `-v -s` or `-k <test>` |

Example: `EVB_SERIAL_PORT=/dev/serial/by-id/<id> EVB_PYTEST_ARGS="-v" just test-device`.

<!-- sync: evb-qv50.17/.19/.20 -->
The Tier 2 runner is being reworked; if a recipe or knob above behaves
differently, `just --list` and the `Justfile` are authoritative.

## When a hardware run fails

- Download-mode check fails: port wrong or busy, see
  `skills/evb-flash/references/recovery.md`.
- Run killed by the watchdog: the serial runner wedged. Rerun once with
  `EVB_PYTEST_ARGS="-v"` and read the streamed device log; a second
  identical hang is a bug to report with that log.
- Tier 3 cannot reach the IP: the host and board are not on the same
  network, or the shell blocks traffic; see
  `skills/_shared/device-address.md`.
- Tests must leave onboard relays OFF and MOD-IO all-off, and must not
  depend on order or leftover state. A test that breaks this is a bug.

## Afterwards: restore the bench

Tier 2 leaves the test app on the board; Tier 3 leaves a token nobody
kept. Put the board back in service:

1. `scripts/flash.sh --port <port>` (production firmware from this
   checkout), or write a release `-full.bin` as in the evb-flash skill.
2. `scripts/provision.sh --port <port> --generate` and hand the token to
   whoever uses the board, via `EVB_RELAY_API_TOKEN` or the CLI config
   file, not via chat logs.
3. Check `evb-relay -H <ip> status` and `evb-relay -H <ip> relay list`
   (everything off).
4. Release the board: say so to the user or the other agents.

## Test rules for changes

- New or changed firmware public API: add Tier 1 host tests when the
  code runs in the host harness, otherwise the narrowest Tier 2 test.
- Every bug fix gets a regression test.
- `just format` before committing firmware C/H files; `just format-check`
  (in `just ci`) enforces AStyle 3.4.7 via `astyle_py`.

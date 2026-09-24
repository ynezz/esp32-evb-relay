# esp32-evb-relay

`esp32-evb-relay` is a networked relay controller for the Olimex
ESP32-EVB and MOD-IO expansion board. This repository contains the
production ESP-IDF firmware in `firmware/` and the `evb-relay` Go CLI in
`cli/` for both human operators and automation.

The current system exposes an authenticated REST API over Ethernet or
WiFi STA, advertises itself with mDNS, streams live device events over
SSE, and supports OTA firmware updates.

## What It Does

- Controls 2 onboard ESP32-EVB relays and 4 MOD-IO relays
- Reads 4 MOD-IO digital inputs and 4 MOD-IO analog inputs
- Exposes an authenticated `/api/v1` HTTP interface over Ethernet or
  WiFi STA with explicit transport policy
- Publishes live `digital_input`, `analog_input`, `relay_changed`, and
  `button` events
- Supports runtime configuration updates and API-token provisioning
- Provides a CLI for discovery, status, relay control, config changes,
  input monitoring, and OTA
- Provides a machine-facing robot interface for agents and automation

## Hardware At A Glance

- **Board:** Olimex ESP32-EVB
- **MCU:** ESP32-D0WD with 4 MB flash
- **Onboard relays:** GPIO32 and GPIO33
- **UEXT I2C:** SDA on GPIO13, SCL on GPIO16
- **Expansion board:** Olimex MOD-IO at I2C address `0x58`
- **Preferred serial alias:** `/dev/esp32-evb`

For full pin mappings, flashing notes, MOD-IO protocol details, and the
current runner caveats, see
[`docs/hardware-reference.md`](docs/hardware-reference.md).

## Repo Layout

```text
firmware/   ESP-IDF application and firmware components
cli/        Go CLI for human and robot operators
scripts/    Flashing and provisioning helpers
docs/       Hardware notes, release test plan, and supporting project docs
tools/      Pre-commit hook and udev helper files
```

## Developer Quickstart

Prerequisites:

- ESP-IDF v5.4 with `IDF_PATH` set
- Go 1.25.x, or Go 1.24.4 with automatic toolchain downloads enabled
- Python 3 with `venv`
- `just`

From the repo root:

```bash
just setup

export IDF_PATH=/path/to/esp-idf
source "$IDF_PATH/export.sh"

just build
just test
just ci

cd cli && go build -o ../bin/evb-relay .
```

Notes:

- `just setup` creates the local `.venv`, installs the Python tooling
  used by formatting and pytest-based checks, and installs the repo
  pre-commit hook.
- `just ci` is the main quality gate. It runs firmware format checks,
  firmware build, host tests, CLI format checks, `golangci-lint`,
  `go vet`, and CLI tests.
- For firmware C/H edits, run `just format` before committing.

## Device Quickstart

Connect the ESP32-EVB and MOD-IO, then use the repo-standard serial
alias if you have one configured. The examples below assume
`/dev/esp32-evb`.

```bash
export IDF_PATH=/path/to/esp-idf
source "$IDF_PATH/export.sh"

./scripts/flash.sh --port /dev/esp32-evb
./scripts/provision.sh --port /dev/esp32-evb --generate

cd cli && go build -o ../bin/evb-relay .

../bin/evb-relay discover
../bin/evb-relay -H esp32-evb-relay.local -k "<token>" status
../bin/evb-relay -H esp32-evb-relay.local -k "<token>" relay list
```

`scripts/provision.sh` prints the provisioned API token. Store it and
reuse it through `--api-token`, `EVB_RELAY_API_TOKEN`, or the CLI config
file.

If you are running hardware-backed checks instead of a manual flash
loop, use `just test-device`. For port aliasing and download-mode
diagnostics, see [`docs/hardware-reference.md`](docs/hardware-reference.md).

## CLI Quickstart

The CLI can discover devices over mDNS, query status, change relays,
read inputs, and perform OTA updates.

```bash
./bin/evb-relay discover

./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" status

./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" relay list
./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" relay on onboard:1
./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" relay set onboard:2=off modio:3=on

./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" input digital
./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" input analog 2
./bin/evb-relay --robot --format json -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" input watch

./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" config show
./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" config set hostname=lab-relay poll_interval_ms=250
./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" config wifi ssid=lab-net passphrase=supersecret network_policy=prefer_ethernet

./bin/evb-relay -H esp32-evb-relay.local -k "$EVB_RELAY_API_TOKEN" ota flash path/to/firmware.bin
```

Automation-facing IDs are 1-based:

- Onboard relays: `onboard:1..2`
- MOD-IO relays: `modio:1..4`
- MOD-IO inputs: `1..4`

## Configuration

### CLI Runtime Configuration

Resolution order is:

1. CLI flags
2. Environment variables
3. CLI config file

Supported environment variables:

- `EVB_RELAY_HOST`
- `EVB_RELAY_API_TOKEN`
- `EVB_RELAY_TIMEOUT`
- `EVB_RELAY_ROBOT`

The CLI config file lives under the platform-native user config
directory, for example `~/.config/evb-relay/config.toml` on Linux.

Example:

```toml
host = "esp32-evb-relay.local"
api_token = "replace-me"
timeout = "10s"
format = "table"
robot = false
```

Use `host` in the CLI config file. `hostname` is the device runtime
setting exposed by `evb-relay config show` / `evb-relay config set`, not
the local CLI target address key.

### Device Runtime Configuration

The device exposes the following config keys through the REST API and
the CLI:

- `api_token`
- `hostname`
- `poll_interval_ms`
- `modio_boot_policy`
- `wifi.network_policy`
- `wifi.ssid_set`
- `wifi.passphrase_set`

Set WiFi credentials and policy with `config wifi` / `PUT /api/v1/config/wifi`.
WiFi credentials are write-only at the API layer.

`wifi.network_policy` accepts:

- `ethernet_only`
- `wifi_only`
- `prefer_ethernet`

`modio_boot_policy` accepts:

- `leave_unchanged`
- `all_off`

The API token is write-only at the device API layer. `config show`
returns `api_token_set` instead of echoing the token value. WiFi
credentials follow the same rule: `config show` returns only
`wifi.ssid_set` and `wifi.passphrase_set`.

## Agents / Robots

The CLI has a dedicated machine-facing interface for agents and
automation in addition to the human `table`, `json`, and `plain`
formats.

### Capability Discovery

Use `--robot-capabilities` to inspect the supported commands, their
arguments, output fields, error codes, exit codes, and environment
variables:

```bash
./bin/evb-relay --robot-capabilities
```

This is the stable starting point for automation. It is better than
hardcoding command assumptions in an agent prompt.

### One-Shot Robot Output

Use `--robot` on normal commands such as `status`, `relay list`,
`config show`, `config set`, `input digital`, `input analog`, `discover`,
and `ota flash`.

```bash
./bin/evb-relay --robot --format json \
  -H esp32-evb-relay.local \
  -k "$EVB_RELAY_API_TOKEN" \
  status
```

Robot mode returns a structured envelope with fields such as:

- `v`
- `command`
- `timestamp`
- `elapsed_ms`
- `exit_code`
- `host`
- `device_context`
- `data`
- `error`
- `warnings`
- `next`

Default robot output is **TOON**, a compact line-oriented structured
format. Use `--format json` when you want explicit JSON envelopes.

Example JSON shape:

```json
{
  "v": 1,
  "command": "status",
  "timestamp": "2026-03-18T12:34:56Z",
  "elapsed_ms": 14,
  "exit_code": 0,
  "host": "esp32-evb-relay.local",
  "device_context": {
    "firmware_version": "0.4.0",
    "modio_present": true,
    "modio_sync": "synchronized"
  },
  "data": {
    "status": {
      "uptime_seconds": 42,
      "firmware_version": "0.4.0"
    }
  }
}
```

### Streaming Robot Output

For live event consumption, use:

```bash
./bin/evb-relay --robot --format json \
  -H esp32-evb-relay.local \
  -k "$EVB_RELAY_API_TOKEN" \
  input watch
```

This command emits newline-delimited JSON:

- A single stream header record
- One record per event
- A final `stream_end` record when the client stops

Event names currently include:

- `digital_input`
- `analog_input`
- `relay_changed`
- `button`
- `stream_end`

Example NDJSON sequence:

```json
{"v":1,"stream":"events","host":"esp32-evb-relay.local","started_at":"2026-03-18T12:34:56Z","device_context":{"firmware_version":"0.4.0","modio_present":true,"modio_sync":"synchronized"}}
{"event":"relay_changed","data":{"group":"onboard","id":1,"state":true},"received_at":"2026-03-18T12:34:57Z"}
{"event":"stream_end","reason":"client_disconnect","received_at":"2026-03-18T12:35:10Z"}
```

The CLI automatically reconnects on transient EOF and network errors
while `input watch` is running.

### Device Context And Auth

Authenticated responses include device context headers that the CLI
projects into `device_context`:

- `X-FW-Version`
- `X-ModIO-Present`
- `X-ModIO-Sync`

That lets agents recover core firmware and hardware state without making
an extra status request first.

Authentication uses `Authorization: Bearer <token>`.

### Exit Codes

Robot and human invocations share the same exit code classes:

- `0` success
- `1` general error
- `2` network error
- `3` auth error
- `4` not found
- `5` bad argument
- `6` state error
- `7` hardware unavailable

Common machine-meaningful error codes include:

- `NETWORK_ERROR`
- `AUTH_REQUIRED`
- `AUTH_FORBIDDEN`
- `RELAY_NOT_FOUND`
- `INPUT_NOT_FOUND`
- `MODIO_NOT_PRESENT`
- `MODIO_SAMPLE_UNAVAILABLE`

`modio_sync` currently has two states:

- `synchronized`
- `absent`

## Testing And QA

Primary commands:

```bash
just ci
just test-device
just test-integration
```

`just ci` is the required baseline quality gate after firmware changes.
Use the hardware-backed commands when the target board is available.

Three test tiers: `just test` (Tier 1, host-only unit tests), `just
test-device` (Tier 2, on-device Unity tests — destructive, reflashes the
test app), and `just test-integration` (Tier 3, HTTP integration against
flashed production firmware). Before cutting a release, also run the
manual checklist in
[`docs/release-test-plan.md`](docs/release-test-plan.md).

### Device Test Environment Variables

The `just test-device` and `just test-integration` recipes are
controlled by environment variables:

| Variable | Default | Description |
|---|---|---|
| `EVB_SERIAL_PORT` | `/dev/esp32-evb` | Serial port for monitor and Unity output |
| `EVB_FLASH_PORT` | `$EVB_SERIAL_PORT` | Port used by esptool for flashing (set separately for split-port setups) |
| `EVB_SERIAL_BAUD` | `115200` | Monitor baud rate |
| `EVB_TEST_APP_SDKCONFIG_DEFAULTS` | `sdkconfig.defaults` | sdkconfig defaults file for the test app build |
| `EVB_TEST_DEVICE_WATCHDOG_SECONDS` | `480` | Whole-run wall-clock watchdog for `just test-device` |
| `EVB_PYTEST_ARGS` | _(empty)_ | Extra flags passed verbatim to pytest |

To see the device serial log and Unity test results live during a run:

```bash
EVB_PYTEST_ARGS="-v -s" just test-device
```

`-v` enables verbose test case names; `-s` disables pytest output
capture so the device boot log and Unity results stream directly to
your terminal.

`just test-device` also applies a repo-owned whole-run watchdog so
serial wedges fail promptly. Override it with
`EVB_TEST_DEVICE_WATCHDOG_SECONDS` when a slower hardware lane needs
more wall-clock headroom.

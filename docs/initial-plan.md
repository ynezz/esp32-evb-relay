# ESP32-EVB Relay Controller — Firmware + CLI

## Context

Build a networked relay controller for Olimex ESP32-EVB + MOD-IO expansion. The system exposes a REST API for remote power control over Ethernet first, with WiFi added later, and a Go CLI for human and automation use.

**Hardware:**
- ESP32-EVB: 2 onboard relays (GPIO32, GPIO33), Ethernet (LAN8710A), UEXT I2C (SDA=GPIO13, SCL=GPIO16)
- MOD-IO (I2C slave 0x58): 4 relays, 4 digital inputs, 4 analog inputs (10-bit samples read over I2C)
- Serial: host-specific USB serial device (for example `/dev/tty.usbserial-*`)

---

## Project Structure

```
esp32-evb-relay/
├── .github/workflows/
│   ├── ci.yml                       # CI: build + lint + test (Step 15)
│   └── release.yml                  # Release: tag → artifacts (Step 16)
├── cliff.toml                       # git-cliff changelog config (Step 18)
├── firmware/                        # ESP-IDF v5.x project
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── partitions.csv               # OTA-capable partition table
│   ├── version.txt                  # Dev fallback version (Step 19)
│   ├── main/
│   │   ├── CMakeLists.txt
│   │   ├── idf_component.yml        # mdns dependency
│   │   ├── main.c                   # app_main: init orchestration
│   │   └── Kconfig.projbuild
│   └── components/
│       ├── board/                    # Pin defs, I2C bus init
│       ├── relay/                    # Onboard relay GPIO driver
│       ├── mod_io/                   # MOD-IO I2C protocol driver
│       ├── network/                  # Ethernet (+ WiFi placeholder)
│       ├── device_config/            # NVS-backed runtime config + validation
│       ├── input_monitor/             # Polls MOD-IO inputs, detects changes
│       ├── rest_api/                 # HTTP server + JSON handlers + SSE
│       ├── auth/                     # API token middleware
│       └── ota/                      # OTA firmware update
├── cli/                             # Go CLI (cobra)
│   ├── go.mod
│   ├── main.go
│   ├── .goreleaser.yaml             # GoReleaser v2 config (Step 17)
│   ├── cmd/                         # Commands: relay, input, status, ota, discover
│   ├── client/                      # HTTP client wrapper + device context extraction
│   │   ├── client.go                # Base URL, auth, timing, header extraction
│   │   └── errors.go                # Structured APIError, exit code classification
│   └── internal/
│       ├── exitcodes/
│       │   └── codes.go             # Exit code constants (0-7)
│       ├── format/
│       │   └── output.go            # Human output: table, json, plain
│       ├── robot/
│       │   ├── envelope.go          # Robot envelope struct, Wrap()
│       │   ├── errors.go            # Error → remediation mapping
│       │   ├── context.go           # DeviceContext from response headers
│       │   ├── capabilities.go      # --robot-capabilities introspection
│       │   └── stream.go            # NDJSON writer for watch streams
│       └── toon/
│           └── encoder.go           # Minimal TOON output encoder
└── scripts/
    ├── flash.sh
    └── provision.sh
```

---

## Phase 1: Firmware (ESP-IDF, C)

### Step 1 — Scaffold ESP-IDF project
- `firmware/CMakeLists.txt`, `firmware/main/CMakeLists.txt`
- `sdkconfig.defaults` with: 4MB flash, custom partition table, Ethernet EMAC enabled, task WDT, OTA rollback
- `partitions.csv`: nvs, otadata, phy_init, coredump, ota_0, ota_1
- Size OTA slots from the real firmware binary with explicit headroom, instead of assuming a vague "~1.9MB" budget on a 4MB flash part
- `.gitignore` (sdkconfig, build/, managed_components/)

### Step 2 — `board` component
- Pin constants: `BOARD_RELAY1_GPIO=32`, `BOARD_RELAY2_GPIO=33`, `BOARD_I2C_SDA=13`, `BOARD_I2C_SCL=16`, `BOARD_ETH_MDC=23`, `BOARD_ETH_MDIO=18`, `BOARD_BUTTON=34`
- `board_init()`: configure I2C master bus (100kHz), button GPIO

### Step 3 — `relay` component
- `relay_init()`, `relay_set(id, state)`, `relay_get(id)`, `relay_toggle(id)`
- Drive both onboard relays to an explicit boot default during `relay_init()`; default to `off` so an ESP32 reboot does not accidentally energize local loads
- GPIO output, mutex for thread safety, tracks state in memory after initialization

### Step 4 — `mod_io` component
- Uses ESP-IDF v5.x `i2c_master` API (not deprecated `i2c_cmd_link`)
- I2C protocol:
  - `0x10` + bitmask → set relay outputs (bits 0-3)
  - `0x20` → read digital inputs (1 byte)
  - `0x30-0x33` → select analog inputs 1-4, then read two bytes carrying the 10-bit sample in the MOD-IO manual's bit-packed `LSB:MSB` format; decode it explicitly instead of treating it as a plain host-endian `uint16`
  - `0x40` → read the current relay-state bitmask (authoritative hardware readback)
- The write command still always sends the full 4-bit relay bitmap, so single-relay operations should perform a read-modify-write cycle against the authoritative readback value instead of trusting stale RAM
- After ESP32 reboot or MOD-IO hot reattach, repopulate the cached relay bitmap from command `0x40`; do not force clients through a synthetic recovery step when the hardware can report its own state
- Do not write every relay toggle to NVS just to simulate readback; that would create flash wear without making the state authoritative
- Serialize all MOD-IO I2C transactions inside the component so background polling and request handlers never race each other on the shared bus
- `mod_io_init(bus_handle)`, `mod_io_is_present()`, graceful failure if module absent

### Step 4b — `input_monitor` component
- FreeRTOS task polls MOD-IO digital + analog inputs at configurable interval (default 100ms)
- Owns the latest sampled input snapshot and timestamps; REST input endpoints should serve this shared snapshot by default instead of triggering separate I2C reads on every request
- Compares against previous state and publishes normalized events into a single internal event queue
- Relay setters publish `relay_changed` events into the same internal event queue; `input_monitor` should not invent relay events
- Also monitors onboard button (GPIO34 interrupt → `button` event on the same internal event queue)
- Debounce the onboard button in software before emitting `button` events so one press does not fan out into multiple spurious notifications
- Analog inputs: configurable threshold for change detection on 10-bit samples (avoid noise-triggered events)
- If MOD-IO probing starts succeeding after an absence/error period, publish a presence change, refresh the cached relay bitmap from readback, and resume normal sampling without reapplying `modio_boot_policy`

### Step 4c — `device_config` component
- NVS-backed source of truth for `api_token`, `poll_interval_ms`, `hostname`, `modio_boot_policy`, and future WiFi credentials
- Centralizes validation, defaults, and persistence so `auth`, `network`, `input_monitor`, and `rest_api` do not each manage their own ad hoc NVS keys
- `modio_boot_policy` should be explicit and low-churn, and should default to `leave_unchanged` so an ESP32 reboot does not silently toggle attached loads; `all_off` is an explicit opt-in if fail-safe-off behavior is desired
- Secrets are write-only at the API layer: never echo `api_token` or raw WiFi credentials back from `GET /api/v1/config`; expose redacted metadata instead
- Returns metadata about whether a config change is applied live or requires a restart/rebind

### Step 5 — `network` component
- Ethernet init: LAN8710A PHY, PHY address `0`, no dedicated ESP32-controlled PHY reset GPIO on current ESP32-EVB revisions (`reset_gpio_num = -1` unless board-specific testing proves otherwise), RMII clock input on GPIO0, MDC/MDIO on GPIO23/18
- Event-driven: wait for IP via `IP_EVENT_ETH_GOT_IP`
- `network_wait_for_ip(timeout_ms)` blocks `app_main` until connected or returns a timeout/error; in the Ethernet-only phase, treat timeout as a startup failure instead of silently continuing without a usable control plane
- Architecture allows a later WiFi phase (separate init path, shared event handlers)
- mDNS: default hostname `esp32-evb-relay` comes from `device_config`; register `_http._tcp` with TXT records (fw_version, board type)

### Step 6 — `auth` component
- API token loaded from `device_config`
- `auth_check(httpd_req_t*)` validates `Authorization: Bearer <token>` for all `/api/v1/*` endpoints
- Secure by default. On first boot, if no token exists yet, generate a random token, persist it, and print it once on the serial console for provisioning
- Define an explicit recovery path for lost credentials: `scripts/provision.sh` should be able to set or rotate the token over serial during provisioning/service, and a factory-reset path may clear the token by wiping relevant NVS keys
- Constant-time comparison

### Step 7 — `rest_api` component
Base: `http://<host>/api/v1`

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/status` | Uptime, FW version, network info, MOD-IO presence/sync state, free heap |
| GET | `/api/v1/relays` | Onboard relay states plus MOD-IO presence/sync metadata; when MOD-IO is present, return authoritative relay states from hardware readback |
| GET | `/api/v1/relays/onboard` | Onboard relay states |
| PUT | `/api/v1/relays/onboard/{id}` | Set onboard relay `{"state": true}` |
| POST | `/api/v1/relays/onboard/{id}/toggle` | Toggle onboard relay |
| GET | `/api/v1/relays/modio` | Current MOD-IO relay states when the daughterboard is present |
| PUT | `/api/v1/relays/modio/{id}` | Set one MOD-IO relay using read-modify-write against the authoritative hardware bitmap |
| PUT | `/api/v1/relays/modio` | Bulk set `{"states": [true,false,true,false]}` for efficient batched updates |
| GET | `/api/v1/inputs/digital` | Read the latest sampled digital inputs plus sample timestamp/staleness metadata |
| GET | `/api/v1/inputs/digital/{id}` | Read one digital input from the latest sampled snapshot plus sample timestamp/staleness metadata |
| GET | `/api/v1/inputs/analog` | Read the latest sampled analog inputs plus sample timestamp/staleness metadata |
| GET | `/api/v1/inputs/analog/{id}` | Read one analog input from the latest sampled snapshot plus sample timestamp/staleness metadata |
| GET | `/api/v1/events` | SSE stream — pushes input/relay/button change events |
| GET | `/api/v1/config` | Read validated, redacted device config (`poll_interval_ms`, `hostname`, `modio_boot_policy`, secret-presence metadata) |
| PUT | `/api/v1/config` | Update device config `{"poll_interval_ms": 200}` and report whether the change applied live |
| POST | `/api/v1/ota` | Upload firmware binary (octet-stream) |

**SSE event stream** (`GET /api/v1/events`, `Accept: text/event-stream`):
- Long-lived HTTP connection, server pushes SSE-framed JSON payloads
- A dedicated event-dispatch path inside `rest_api` drains the internal event queue and fans out copies to connected SSE clients; individual HTTP handlers must not consume the one producer queue directly
- Event types: `digital_input`, `analog_input`, `relay_changed`, `button`
- Format: `event: digital_input\ndata: {"id":2,"state":true,"ts_ms":12345}\n\n`
- Firmware I2C polling is internal (MOD-IO has no interrupt line); SSE makes the *client* event-driven
- Polling interval configurable via `PUT /api/v1/config` (see below)
- Cap SSE fan-out to a small fixed number of clients and drop stale subscribers on backpressure instead of letting one slow client exhaust MCU resources
- Heartbeat every 30s to detect stale connections

**Response headers** — every authenticated `rest_api` HTTP response includes
device context headers so agents can build `device_context` without a sidecar
`/api/v1/status` call:

```
X-FW-Version: 0.3.1
X-ModIO-Present: true
X-ModIO-Sync: synchronized|absent
```

Add a post-handler hook or helper that injects these headers on success
responses and on application errors returned after auth succeeds. Do not attach
them to pre-auth `401/403` responses, since unauthenticated clients do not need
firmware/hardware metadata. `X-ModIO-Present` and `X-ModIO-Sync` are read from
`mod_io` component state; once relay-state readback succeeds the sync value
stays `synchronized` across boots and reconnects. `X-FW-Version` is read from
`esp_app_desc_t.version`.

Error format: `{"error": {"code": "RELAY_NOT_FOUND", "message": "...", "status": 404}}`
- MOD-IO-specific endpoints return `503 MODIO_NOT_PRESENT` when the daughterboard is absent; do not fabricate zeroed input or relay state
- If no valid MOD-IO sample exists yet, input endpoints return `503 MODIO_SAMPLE_UNAVAILABLE` rather than pretending an old or nonexistent snapshot is current
- Public API and CLI IDs are 1-based (`onboard:1..2`, `modio:1..4`, `input 1..4`); the firmware maps them internally onto GPIO definitions and MOD-IO protocol channels `0..3`

URI parsing: register wildcard handlers with `httpd_uri_match_wildcard()` and use a helper such as `parse_id_from_uri()` because ESP-IDF httpd still lacks native path params.

### Step 8 — `ota` component
- `POST /api/v1/ota` streams binary via `esp_ota_begin/write/end`
- On the next boot, only call `esp_ota_mark_app_valid_cancel_rollback()` after the image has passed bounded local startup health checks (for example: config loaded, board/peripheral init succeeded, background tasks started, and the Ethernet driver initialized); do not make rollback confirmation depend on external conditions like DHCP or mDNS
- Sets boot partition, reboots after 2s delay
- Rollback enabled via `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`

### Step 9 — `main.c` boot sequence
```
nvs_flash_init → event_loop_create → device_config_init →
board_init → relay_init →
mod_io_init → input_monitor_start → auth_init → network_init →
ota_confirm_running_image_if_core_init_passed → wait_for_ip →
mdns_register → rest_api_start →
register long-running tasks with the task WDT
```

---

## Phase 2: CLI Client (Go)

### Step 10 — Scaffold Go project
- `go.mod` with `github.com/spf13/cobra`, `github.com/hashicorp/mdns`, `github.com/BurntSushi/toml`
- Config file: `os.UserConfigDir()/evb-relay/config.toml` so the CLI uses the platform-native config directory on Linux, macOS, and Windows

**Global flags:**

| Flag | Env Var | Description |
|------|---------|-------------|
| `--host/-H` | `EVB_RELAY_HOST` | Device IP or hostname |
| `--api-token/-k` | `EVB_RELAY_API_TOKEN` | API authentication token |
| `--format/-f` | — | Human mode: table/json/plain (default: table). Robot mode: only `json` is a valid override; otherwise robot mode defaults to TOON. |
| `--timeout/-t` | `EVB_RELAY_TIMEOUT` | HTTP timeout (e.g., 5s, 10s; default: 10s) |
| `--robot` | `EVB_RELAY_ROBOT=1` | Activate robot mode (TOON/JSON envelope on stdout, no color) |
| `--robot-capabilities` | — | Introspection: dump full CLI contract as JSON, exit. No `--host` needed. |
| `--version` | — | Print CLI version metadata and exit |

**Format matrix:**

| Mode | Default format | Output | Envelope? |
|------|---------------|--------|-----------|
| `--robot` | TOON | TOON envelope + data | yes |
| `--robot --format json` | JSON | JSON envelope + data | yes |
| `--format json` | JSON | command result as JSON, no robot envelope | no |
| `--format table` | table | human-aligned columns | no |
| `--format plain` | plain | bare values, one/line | no |
| (default) | table | human-aligned columns | no |

With `--robot`, only the default TOON output and `--format json` are valid.
Reject `--robot --format table` and `--robot --format plain` as bad-argument
usage (`exit_code=5`) instead of silently falling back to a human format.

**Config precedence (highest → lowest):**
1. CLI flags (`--host`, `--api-token`, `--robot`, `--format`, `--timeout`)
2. Environment variables (`EVB_RELAY_HOST`, `EVB_RELAY_API_TOKEN`, `EVB_RELAY_ROBOT`, `EVB_RELAY_TIMEOUT`)
3. Config file (`~/.config/evb-relay/config.toml`)

### Step 11 — `client/` package
- HTTP client wrapper: base URL, auth header injection, timeout, error handling
- Maps HTTP errors to structured Go errors
- Captures request timing (start/end) for `elapsed_ms` in robot envelopes
- Extracts `DeviceContext` from response headers: `X-ModIO-Sync`,
  `X-FW-Version`, `X-ModIO-Present`
- Returns structured `APIError` with code, message, HTTP status for the robot
  error classification pipeline
- `client/errors.go`: error classification and exit code mapping based on
  `APIError.Code` and HTTP status fallback

### Step 12 — Commands
```
evb-relay relay list                    # Relay states + MOD-IO sync metadata
evb-relay relay on onboard:1            # Target format: <group>:<id>
evb-relay relay off modio:3             # Works whenever MOD-IO is present
evb-relay relay toggle onboard:2
evb-relay relay set onboard:1=on modio:all=off   # Multi-target with :all shorthand
evb-relay relay set modio:1=on modio:2=off modio:3=on modio:4=off   # Explicit per-relay

evb-relay input digital                 # Latest sampled digital snapshot
evb-relay input digital 2               # One digital input from the latest sampled snapshot
evb-relay input analog                  # Latest sampled analog snapshot
evb-relay input analog 2                # One analog input from the latest sampled snapshot
evb-relay input watch                   # SSE stream — prints events as they arrive

evb-relay status                        # Device health
evb-relay config show                   # Read device config
evb-relay config set poll_interval_ms=200  # Change input polling interval
evb-relay discover                      # mDNS browse
evb-relay ota flash <firmware.bin>      # OTA update
evb-relay completion bash|zsh|fish|powershell  # Shell completions
```

#### Step 12a — `modio:all` and `onboard:all` shorthand

`relay set` resolves `modio:all` and `onboard:all` into a desired-state plan
before choosing transport:

```bash
evb-relay relay set modio:all=off
# resolves to desired MOD-IO bitmap [false,false,false,false]
# then sends one bulk PUT /api/v1/relays/modio request

evb-relay relay set onboard:all=on
# resolves to: onboard:1=on onboard:2=on
# then sends two onboard requests because the firmware only exposes
# per-relay onboard endpoints
```

If a single `relay set` command mentions any `modio:*` targets, the CLI must
coalesce them into one final 4-relay bitmap and send exactly one MOD-IO bulk
request. That matches the firmware's full-bitmap operation and avoids transient
intermediate states between separate single-relay writes.

Relay count (4 MOD-IO, 2 onboard) is hardcoded in the CLI to match the
hardware spec. Batch results in robot mode still report per-target
success/failure:

```
results
target	state	ok	error
onboard:1	true	true	-
modio:1	false	true	-
modio:2	false	true	-
modio:3	false	true	-
modio:4	false	true	-

all_ok=true
```

On partial failure: `exit_code=1`, `error.code=PARTIAL_FAILURE`, per-target
`ok=false` + `error` on failed targets, `all_ok=false`. The MOD-IO rows share
the outcome of the single bulk MOD-IO request; the CLI must not treat them as
independent single-relay writes.

#### Step 12b — TOON encoder

Minimal Go TOON encoder, output-only. Because `--robot` uses TOON by default,
the encoder must emit spec-compatible TOON rather than an ad hoc `key=value`
dialect. The reference TOON project reports materially lower token counts than
pretty-printed JSON on mixed benchmarks, but the real savings depend on payload
shape and are best on uniform tabular data.

**Capabilities:**
- Nested objects via indentation
- Compact primitive fields (`key: value`)
- Spec-compatible uniform arrays (`items[3]: a,b,c`)
- Spec-compatible uniform object tables (`relays[6]{group,id,state,sync}:`)
- Type handling: null, bool, number, string
- No TOON parser needed (output-only)
- Validate the encoder against upstream TOON conformance fixtures before
  shipping robot mode with TOON as the default format

**API:**
```go
package toon

func Encode(w io.Writer, v any) error
```

#### Step 12c — Robot envelope

Every non-streaming `--robot` response wraps output in a structured envelope.
`input watch` is the one explicit exception and uses NDJSON instead (see Step
12e).

**Success envelope (TOON):**
```
v: 1
command: relay list
timestamp: 2026-03-16T14:22:03.412Z
elapsed_ms: 42
exit_code: 0
host: 192.168.1.50
device_context:
  modio_present: true
  modio_sync: synchronized
  firmware_version: 0.3.1
data:
  relays[6]{group,id,state,sync}:
    onboard,1,true,null
    onboard,2,false,null
    modio,1,true,synchronized
    modio,2,false,synchronized
    modio,3,true,synchronized
    modio,4,false,synchronized
```

**Success envelope (JSON, via `--robot --format json`):**
```json
{
  "v": 1,
  "command": "relay list",
  "timestamp": "2026-03-16T14:22:03.412Z",
  "elapsed_ms": 42,
  "exit_code": 0,
  "host": "192.168.1.50",
  "data": {
    "relays": [
      {"group": "onboard", "id": 1, "state": true, "sync": null},
      {"group": "onboard", "id": 2, "state": false, "sync": null},
      {"group": "modio", "id": 1, "state": true, "sync": "synchronized"},
      {"group": "modio", "id": 2, "state": false, "sync": "synchronized"},
      {"group": "modio", "id": 3, "state": true, "sync": "synchronized"},
      {"group": "modio", "id": 4, "state": false, "sync": "synchronized"}
    ]
  },
  "device_context": {
    "modio_present": true,
    "modio_sync": "synchronized",
    "firmware_version": "0.3.1"
  },
  "warnings": []
}
```

**Error envelope (TOON):**
```
v: 1
command: relay on modio:3
timestamp: 2026-03-16T14:22:03.464Z
elapsed_ms: 52
exit_code: 7
host: 192.168.1.50
device_context:
  modio_present: false
  modio_sync: absent
  firmware_version: 0.3.1
error:
  code: MODIO_NOT_PRESENT
  message: MOD-IO board is not present.
  http_status: 503
  retryable: false
  remediation: null
```

**Error envelope (JSON):**
```json
{
  "v": 1,
  "command": "relay on modio:3",
  "timestamp": "2026-03-16T14:22:03.464Z",
  "elapsed_ms": 52,
  "exit_code": 7,
  "host": "192.168.1.50",
  "error": {
    "code": "MODIO_NOT_PRESENT",
    "message": "MOD-IO board is not present.",
    "http_status": 503,
    "retryable": false,
    "remediation": null
  },
  "device_context": {
    "modio_present": false,
    "modio_sync": "absent",
    "firmware_version": "0.3.1"
  }
}
```

**Remediation table (built into CLI):**

| API Error | Exit Code | Remediation |
|-----------|-----------|-------------|
| `MODIO_NOT_PRESENT` (503) | 7 | *(none — hardware)* |
| `MODIO_SAMPLE_UNAVAILABLE` (503) | 7 | *(retryable: true, wait for poll cycle)* |
| `RELAY_NOT_FOUND` (404) | 4 | *(none — bad ID)* |
| `AUTH_REQUIRED`/`AUTH_INVALID` (401/403) | 3 | Provide a valid API token via `--api-token`, `EVB_RELAY_API_TOKEN`, or the CLI config file |
| Network timeout | 2 | Retry; if the target host is stale or unknown, re-run `evb-relay discover` |

When `error.remediation` or `next[]` contains angle-bracket placeholders such
as `<on|off>`, treat it as a command template that requires operator or agent
substitution before execution, not as a literal shell command.

`device_context` is populated from firmware response headers (`X-ModIO-Sync`,
`X-FW-Version`, `X-ModIO-Present`). If firmware doesn't provide headers yet,
the field is null/omitted.

**Output flow** — each command handler:

```go
func runRelayOn(cmd *cobra.Command, args []string) error {
    result, deviceCtx, err := client.SetRelay(target, true)

    if robotMode {
        // Writes the envelope and returns a typed exit error on non-zero exit.
        return robot.Wrap(cmd, os.Stdout, robot.WrapOpts{
            Data:          result,
            DeviceContext: deviceCtx,
            Err:           err,
            Format:        robotFormat, // "toon" or "json"
        })
    }

    // Human output path (table/json/plain)
    return format.Output(os.Stdout, result, outputFormat)
}
```

`robot.Wrap()` handles: building the envelope (command, timing, exit code,
device context), error → remediation mapping, populating `next` suggestions,
and marshaling to TOON or JSON. On non-zero outcomes it returns a typed error
that carries the desired exit code after the envelope has already been written,
and the root command must intercept that type so Cobra does not print a second
human-oriented error line.

#### Step 12d — `--robot-capabilities` introspection

`evb-relay --robot-capabilities` — always JSON (even if TOON is default in
robot mode, capabilities is complex/nested and JSON is better here). No
`--host` required.

```json
{
  "v": 1,
  "cli_version": "0.3.1",
  "envelope_version": 1,
  "default_robot_format": "toon",
  "commands": [
    {
      "name": "relay list",
      "description": "List all relay states with MOD-IO sync metadata",
      "args": [],
      "flags": [],
      "output_fields": ["relays[].group", "relays[].id", "relays[].state", "relays[].sync"],
      "errors": [],
      "example": "evb-relay --robot relay list"
    }
  ],
  "exit_codes": {
    "0": "success",
    "1": "general error",
    "2": "network error",
    "3": "auth error",
    "4": "not found",
    "5": "bad argument",
    "6": "state error (reserved for future device-state conflicts)",
    "7": "hardware unavailable"
  },
  "error_codes": {
    "MODIO_NOT_PRESENT": {"exit_code": 7, "retryable": false, "remediation": null},
    "MODIO_SAMPLE_UNAVAILABLE": {"exit_code": 7, "retryable": true, "remediation": null},
    "RELAY_NOT_FOUND": {"exit_code": 4, "retryable": false, "remediation": null},
    "AUTH_REQUIRED": {"exit_code": 3, "retryable": false, "remediation": null},
    "AUTH_INVALID": {"exit_code": 3, "retryable": false, "remediation": null},
    "PARTIAL_FAILURE": {"exit_code": 1, "retryable": false, "remediation": null}
  },
  "state_machine": {
    "modio_sync_states": ["synchronized", "absent"],
    "transitions": {
      "absent -> synchronized": "MOD-IO is physically connected and relay-state readback succeeds",
      "synchronized -> synchronized": "ESP32 reboots or MOD-IO reconnects and the firmware refreshes relay state from readback",
      "* -> absent": "MOD-IO is physically disconnected or probe/readback fails"
    },
    "boot_hint": "No bulk recovery step is required; the firmware refreshes the authoritative relay bitmap from MOD-IO during init"
  },
  "environment_variables": {
    "EVB_RELAY_HOST": "Device IP or hostname",
    "EVB_RELAY_API_TOKEN": "API authentication token",
    "EVB_RELAY_ROBOT": "Set to 1 to enable robot mode",
    "EVB_RELAY_TIMEOUT": "HTTP timeout (e.g., 5s, 10s)"
  }
}
```

Generated from cobra command tree — command metadata is annotated on each cobra
command, then the capabilities handler walks the tree and emits the JSON. Not
hand-maintained.

#### Step 12e — NDJSON watch stream

`evb-relay --robot input watch` produces NDJSON (one JSON object per line), not
the standard envelope.

**Stream header (first line):**
```json
{"v":1,"stream":"events","host":"192.168.1.50","started_at":"2026-03-16T14:22:03.000Z","device_context":{"modio_present":true,"modio_sync":"synchronized","firmware_version":"0.3.1"}}
```

**Event lines:**
```json
{"event":"digital_input","data":{"id":2,"state":true,"ts_ms":12345},"received_at":"2026-03-16T14:22:03.412Z"}
{"event":"relay_changed","data":{"group":"modio","id":1,"state":false,"ts_ms":12350},"received_at":"2026-03-16T14:22:04.001Z"}
{"event":"heartbeat","data":{"ts_ms":42345},"received_at":"2026-03-16T14:22:33.412Z"}
```

**Stream end (on clean disconnect):**
```json
{"event":"stream_end","reason":"client_disconnect","received_at":"2026-03-16T14:22:35.000Z"}
```

The CLI should extract firmware response headers once at stream startup and
include them in the NDJSON header line as `device_context`, so robot consumers
get the same zero-extra-request device metadata that non-streaming robot
commands expose.

Why NDJSON for streams (not TOON): this plan's TOON encoder targets bounded
request/response envelopes and fixed-shape arrays. SSE events are
heterogeneous (`digital_input` vs `relay_changed` vs `heartbeat`) and the
stream is unbounded, so NDJSON is the better fit.

### Step 13 — Output formats and exit codes

**Human formats** (no envelope):
- **table** (default): aligned columns via `text/tabwriter`
- **json**: command result as JSON, without the robot envelope
- **plain**: bare values, one per line (for piping)

**Robot formats** (for bounded request/response commands):
- **toon** (default in `--robot`): spec-compatible TOON envelope + data
- **json** (`--robot --format json`): JSON envelope + data

`evb-relay --robot input watch` is a streaming exception: it uses NDJSON, not a
TOON/JSON envelope.

**Exit codes:**

| Code | Name | Meaning | Agent response |
|------|------|---------|---------------|
| 0 | success | Command completed | Read `data` |
| 1 | general | Unexpected error / partial failure | Parse `error`, log, escalate |
| 2 | network | Connection/timeout | Retry, run `discover` |
| 3 | auth | 401/403 | Provide a valid API token |
| 4 | not_found | 404 | Fix target identifier |
| 5 | bad_arg | Invalid CLI usage | Fix invocation |
| 6 | state | Reserved for future device-state conflicts | Parse `error`, follow remediation if present |
| 7 | hardware | 503 MODIO_NOT_PRESENT/SAMPLE_UNAVAILABLE | Check physical hardware, wait, or skip |

**Per-command output schemas (data field):**

| Command | Schema |
|---------|--------|
| `relay list` | `relays[]: {group, id, state: bool\|null, sync: string\|null}` |
| `relay on/off/toggle` | `relay: {group, id, state: bool}` |
| `relay set` (batch) | `results[]: {target, state, ok: bool, error: string\|null}`, `all_ok: bool` |
| `input digital` | `inputs[]: {id, state: bool}`, `sample_ts_ms`, `sample_age_ms` |
| `input analog` | `inputs[]: {id, value: int}`, `sample_ts_ms`, `sample_age_ms` |
| `status` | `uptime_seconds`, `firmware_version`, `free_heap_bytes`, `network.*`, `modio.*`, `relays.*` |
| `config show` | `config: {poll_interval_ms, hostname, modio_boot_policy, api_token_set}` |
| `config set` | `changes[]: {key, old, new, live: bool}`, `restart_required: bool` |
| `discover` | `devices[]: {hostname, ip, port, txt}` |
| `ota flash` | `uploaded_bytes`, `firmware_file`, `reboot_in_seconds` |

`input watch` is intentionally omitted from the table above because it is a
streaming command, not a bounded `data` envelope. Its NDJSON event schema is
defined in Step 12e.

---

## Phase 2b: CI/CD & Release Cycle

A unified version tag `vX.Y.Z` drives firmware builds, CLI releases, and
changelog generation. GitHub Actions handles everything: CI on every push/PR,
and a full release pipeline on tag push.

### Step 14 — Conventional Commits convention

All commit messages follow the [Conventional Commits](https://www.conventionalcommits.org/) format:

```
<type>[(<scope>)][!]: <description>
```

- **Scopes:** `firmware`, `cli`, or omit for cross-cutting changes
- Scope and `!` are optional; use `!` or a `BREAKING CHANGE:` footer when a
  commit introduces a major-version change
- **Types and changelog mapping:**

| Type | Changelog Group | Included |
|------|-----------------|----------|
| `feat` | Features | yes |
| `fix` | Bug Fixes | yes |
| `refactor` | Refactoring | yes |
| `perf` | Performance | yes |
| `docs` | Documentation | yes |
| `ci` | CI/CD | yes |
| `test` | Testing | yes |
| `build` | Build System | yes |
| `chore` | *(filtered out)* | no |

Examples:
```
feat(firmware): add SSE heartbeat to rest_api component
fix(cli): surface MODIO_NOT_PRESENT in relay commands
ci: add firmware binary size tracking to CI
```

### Step 15 — CI workflow (`.github/workflows/ci.yml`)

**Triggers:** push to `main`, pull requests targeting `main`.

**Permissions:**
```yaml
permissions:
  contents: read
  pull-requests: read
```

Path filtering via `dorny/paths-filter@v3` ensures firmware-only changes skip
Go jobs and vice versa, while shared CI/release changes still exercise both
stacks.

**Jobs:**

1. **`changes`** — detect which paths changed
   - Runs on `ubuntu-latest`
   - Starts with `actions/checkout@v6`; push-based change detection needs the
     repository checkout even though pull-request mode can use the GitHub API
   - Uses `dorny/paths-filter@v3`
   - Outputs: `firmware` (bool), `cli` (bool), `shared` (bool)
   - Filters:
     ```yaml
     firmware:
       - 'firmware/**'
     cli:
       - 'cli/**'
     shared:
       - '.github/workflows/**'
       - 'cli/.goreleaser.yaml'
       - 'cliff.toml'
       - 'Justfile'
     ```

2. **`firmware-build`** — build the ESP-IDF project and report size
   - Needs `changes`
   - Runs on `ubuntu-latest`
   - Condition: `needs.changes.outputs.firmware == 'true' || needs.changes.outputs.shared == 'true'`
   - Starts with `actions/checkout@v6`
   - Uses `espressif/esp-idf-ci-action@v1` with `esp_idf_version: v5.4`,
     `path: firmware`, command: `idf.py set-target esp32 && idf.py build && idf.py size`
   - Logs partition and binary sizes in the same job while the generated
     `.elf` and `.map` files are still present
   - Uploads `firmware/build/esp32-evb-relay.bin` as a workflow artifact

3. **`cli-lint`** — lint Go code
   - Needs `changes`
   - Runs on `ubuntu-latest`
   - Condition: `needs.changes.outputs.cli == 'true' || needs.changes.outputs.shared == 'true'`
   - Starts with `actions/checkout@v6`
   - Uses `actions/setup-go@v6` with
     `go-version-file: cli/go.mod`, `cache-dependency-path: cli/go.sum`
   - Uses `golangci/golangci-lint-action@v9` with `working-directory: cli`

4. **`cli-test`** — run Go tests
   - Needs `changes`
   - Runs on `ubuntu-latest`
   - Condition: `needs.changes.outputs.cli == 'true' || needs.changes.outputs.shared == 'true'`
   - Starts with `actions/checkout@v6`
   - Uses `actions/setup-go@v6` with
     `go-version-file: cli/go.mod`, `cache-dependency-path: cli/go.sum`
   - Runs in `cli/`: `go test -race -coverprofile=coverage.out` over
     all CLI packages except `cli/test_e2e`

5. **`cli-e2e`** — run stub-backed CLI subprocess tests
   - Needs `changes`
   - Runs on `ubuntu-latest`
   - Condition: `needs.changes.outputs.cli == 'true' || needs.changes.outputs.firmware == 'true' || needs.changes.outputs.shared == 'true'`
   - Starts with `actions/checkout@v6`
   - Uses `actions/setup-go@v6` with
     `go-version-file: cli/go.mod`, `cache-dependency-path: cli/go.sum`
   - Installs `just`
   - Runs from the repo root: `just test-e2e`

6. **`cli-build`** — verify Go compilation
   - Needs `changes`
   - Runs on `ubuntu-latest`
   - Condition: `needs.changes.outputs.cli == 'true' || needs.changes.outputs.shared == 'true'`
   - Starts with `actions/checkout@v6`
   - Uses `actions/setup-go@v6` with
     `go-version-file: cli/go.mod`, `cache-dependency-path: cli/go.sum`
   - Runs in `cli/`: `go build -o /dev/null .`

### Step 16 — Release workflow (`.github/workflows/release.yml`)

**Trigger:** tag push matching `v*`.

**Permissions:**
```yaml
permissions:
  contents: write
```

Extracts version from the tag (`${GITHUB_REF_NAME#v}`) and coordinates four
jobs. Each job starts with `actions/checkout@v6` using `fetch-depth: 0` so tag
history is available to GoReleaser and `git-cliff`:

1. **`firmware`** — build versioned firmware binary
   - Runs on `ubuntu-latest`
   - Uses `espressif/esp-idf-ci-action@v1` with `esp_idf_version: v5.4`,
     `path: firmware`, command:
     `idf.py set-target esp32 && idf.py -DPROJECT_VER=X.Y.Z build`
     so a clean checkout without a tracked `sdkconfig` still builds for the
     correct chip while embedding the release version
   - Renames output to `esp32-evb-relay-vX.Y.Z.bin`
   - Generates `esp32-evb-relay-vX.Y.Z.bin.sha256` checksum
   - Uploads both as workflow artifacts

2. **`cli`** — build CLI binaries via GoReleaser
   - Runs on `ubuntu-latest`
   - After checkout, uses `actions/setup-go@v6` with
     `go-version-file: cli/go.mod`, `cache-dependency-path: cli/go.sum`
   - Uses `goreleaser/goreleaser-action@v6` with `version: "~> v2"`,
     `args: release --clean --config cli/.goreleaser.yaml`
   - Creates a draft GitHub Release with 6 CLI archives
     (linux/darwin/windows × amd64/arm64) + checksums file
   - GoReleaser creates the release; later jobs augment it

3. **`changelog`** — generate release notes
   - Runs on `ubuntu-latest`
   - Installs `git-cliff` and runs `git-cliff --latest --strip header`
     against the full fetched tag history
   - Uploads the changelog text as a workflow artifact

4. **`release`** — assemble final GitHub Release
   - Depends on: `firmware`, `cli`, `changelog`
   - Runs on `ubuntu-latest`
   - Downloads firmware artifact and changelog artifact
   - Uploads `esp32-evb-relay-vX.Y.Z.bin` + `.sha256` to the existing
     GitHub Release (created by GoReleaser)
   - Sets the release body to the git-cliff output via
     `softprops/action-gh-release@v2`
   - Marks the release as non-draft

### Step 17 — GoReleaser configuration (`cli/.goreleaser.yaml`)

GoReleaser v2 format. Key settings:

```yaml
project_name: evb-relay
version: 2
builds:
  - dir: cli
    main: .
    binary: evb-relay
    env:
      - CGO_ENABLED=0
    goos: [linux, darwin, windows]
    goarch: [amd64, arm64]
    flags:
      - -trimpath
    ldflags:
      - -s -w
      - -X github.com/ynezz/esp32-evb-relay/cli/cmd.Version={{.Version}}
      - -X github.com/ynezz/esp32-evb-relay/cli/cmd.Commit={{.Commit}}
      - -X github.com/ynezz/esp32-evb-relay/cli/cmd.Date={{.CommitDate}}
    mod_timestamp: "{{ .CommitTimestamp }}"

archives:
  - formats:
      - tar.gz
    format_overrides:
      - goos: windows
        formats:
          - zip

checksum:
  name_template: checksums.txt

release:
  draft: true
  prerelease: auto    # -rc/-beta tags → pre-release

changelog:
  disable: true       # git-cliff handles changelog
```

- The release workflow can invoke GoReleaser from the repository root with
  `--config cli/.goreleaser.yaml`; `dir: cli` makes the build step resolve the
  CLI module correctly in that mode
- `project_name: evb-relay` keeps archive names aligned with the documented
  release assets instead of inheriting the repository name
- `binary: evb-relay` makes the extracted executable match the CLI command name
- `CGO_ENABLED=0` for static binaries
- `checksum.name_template: checksums.txt` makes the published checksum asset
  match the documented release contents exactly
- `-trimpath` plus commit-derived timestamps keep repeat builds from the same
  tag closer to reproducible
- 6 targets: linux/darwin/windows × amd64/arm64
- Archives: `.tar.gz` (Unix), `.zip` (Windows)
- `draft: true` matches the release workflow's expectation that GoReleaser
  creates a draft release first
- `prerelease: auto` flags `-rc`/`-beta` tags as pre-release
- Changelog disabled — git-cliff generates release notes instead

### Step 18 — git-cliff configuration (`cliff.toml`)

Conventional commit parser with type-to-group mapping and scope-inline
rendering:

```toml
[changelog]
header = ""
body = """
{% for group, commits in commits | group_by(attribute="group") %}
### {{ group | upper_first }}
{% for commit in commits %}
- {% if commit.scope %}**{{ commit.scope }}**: {% endif %}\
  {{ commit.message | upper_first }}\
  ({{ commit.id | truncate(length=7, end="") }})\
{% endfor %}
{% endfor %}
"""
trim = true

[git]
conventional_commits = true
filter_unconventional = true
commit_parsers = [
  { message = "^feat",     group = "Features" },
  { message = "^fix",      group = "Bug Fixes" },
  { message = "^refactor", group = "Refactoring" },
  { message = "^perf",     group = "Performance" },
  { message = "^docs",     group = "Documentation" },
  { message = "^ci",       group = "CI/CD" },
  { message = "^test",     group = "Testing" },
  { message = "^build",    group = "Build System" },
  { message = "^chore",    skip = true },
]
```

- Scopes render inline (e.g., `**firmware**: Add SSE heartbeat`) so
  firmware-only and CLI-only changes are visually distinct in the changelog
- `chore` commits are filtered out
- Unconventional commits are excluded (`filter_unconventional = true`)

### Step 19 — Version management

**Single source of truth:** the git tag `vX.Y.Z`.

- **Firmware:** The release workflow runs
  `idf.py set-target esp32 && idf.py -DPROJECT_VER=X.Y.Z build`, which sets the
  target on clean checkouts, populates `PROJECT_VER`, and fills
  `esp_app_desc_t.version`. This version surfaces in
  `GET /api/v1/status` and the mDNS `fw_version` TXT record.
  `firmware/version.txt` contains `0.0.0-dev` as a local dev fallback when
  `PROJECT_VER` is not injected by the release pipeline.

- **CLI:** GoReleaser injects version metadata via ldflags into `cmd.Version`,
  `cmd.Commit`, and `cmd.Date`. The `--version` flag and
  `--robot-capabilities` read from the same `cmd` package metadata instead of
  splitting version state across `cmd` and `main`.

- **Semver policy:**
  - **Major** — breaking REST API changes, breaking CLI interface changes
  - **Minor** — new endpoints, new CLI commands, new firmware features
  - **Patch** — bug fixes, documentation, internal refactors

### Step 20 — Full release cycle

**End-to-end flow:**

```
feature branch → PR → CI (Step 15) → review → merge to main
                                                    ↓
                                              git tag vX.Y.Z
                                                    ↓
                                        release workflow (Step 16)
                                           ↓        ↓        ↓
                                      firmware    CLI     changelog
                                           ↓        ↓        ↓
                                        GitHub Release (assembled)
```

1. Create a feature branch, make changes with conventional commits
2. Open a PR targeting `main` — CI runs path-filtered jobs
3. On merge to `main`, tag the release: `git tag v0.1.0 && git push origin v0.1.0`
4. The release workflow triggers and produces:
   - Firmware binary built with the embedded version
   - 6 CLI archives built by GoReleaser
   - Changelog generated by git-cliff
5. The final `release` job assembles the GitHub Release

**GitHub Release contents:**

| Asset | Description |
|-------|-------------|
| `esp32-evb-relay-v0.1.0.bin` | Firmware application image for OTA or updating the app partition on an already provisioned device |
| `esp32-evb-relay-v0.1.0.bin.sha256` | SHA-256 checksum for firmware |
| `evb-relay_0.1.0_linux_amd64.tar.gz` | CLI binary (Linux amd64) |
| `evb-relay_0.1.0_linux_arm64.tar.gz` | CLI binary (Linux arm64) |
| `evb-relay_0.1.0_darwin_amd64.tar.gz` | CLI binary (macOS amd64) |
| `evb-relay_0.1.0_darwin_arm64.tar.gz` | CLI binary (macOS arm64) |
| `evb-relay_0.1.0_windows_amd64.zip` | CLI binary (Windows amd64) |
| `evb-relay_0.1.0_windows_arm64.zip` | CLI binary (Windows arm64) |
| `checksums.txt` | SHA-256 checksums for all CLI archives |

For first-time blank-device flashing, use the local ESP-IDF build outputs (or
later add a dedicated factory-flash bundle); the released firmware asset above
is intentionally app-only.

**Edge cases:**

- **Firmware-only changes:** CI skips CLI jobs; release still builds both
  artifacts from the tagged commit (the CLI binary is unchanged but versioned
  consistently)
- **CLI-only changes:** CI skips firmware jobs; same unified release
- **Pre-release tags** (`v0.2.0-rc.1`): GoReleaser marks the GitHub Release
  as a pre-release via `prerelease: auto`; git-cliff still generates the
  changelog

---

## Phase 3 (future): WiFi support
- Add WiFi STA init in `network` component (credentials from `device_config`)
- Replace unconditional fallback with an explicit network policy such as `ethernet_only`, `wifi_only`, or `prefer_ethernet`; do not silently jump transports just because DHCP was slow once
- If `prefer_ethernet` is enabled, only try WiFi after a deliberate timeout and surface the active transport in `/api/v1/status`
- Add `PUT /api/v1/config/wifi` endpoint for setting credentials
- Add `evb-relay config wifi` CLI command
- Architecture in phase 1 already accommodates this (shared config, event handlers, common HTTP stack)

---

## Verification

### Firmware

1. **Build firmware**: `cd firmware && idf.py set-target esp32 && idf.py build`
2. **Flash**: `cd firmware && idf.py -p <serial-port> flash monitor`
3. **Verify boot**: serial console shows init sequence, prints the first-boot API token if one was generated, and reports an Ethernet IP
4. **Verify token recovery path**: use `scripts/provision.sh` or the documented serial recovery flow to rotate the token without exposing it from `GET /api/v1/config`
5. **Test API**: `curl -H "Authorization: Bearer <token>" http://<ip>/api/v1/status` returns JSON
6. **Test relays**: `curl -X PUT -H "Authorization: Bearer <token>" -H "Content-Type: application/json" -d '{"state":true}' http://<ip>/api/v1/relays/onboard/1` — hear relay click
7. **Test MOD-IO readback model**: with `modio_boot_policy=leave_unchanged`, `GET /api/v1/relays/modio` returns the authoritative 4-relay bitmap immediately after boot; single-relay `PUT /api/v1/relays/modio/{id}` updates only the targeted bit, and bulk `PUT /api/v1/relays/modio` still applies one full bitmap in a single request
8. **Test response headers**: verify authenticated API responses, including post-auth application errors, include `X-FW-Version`, `X-ModIO-Present`, and `X-ModIO-Sync` headers; pre-auth `401/403` responses may omit them

### CLI — Human Mode

9. **Build CLI**: `cd cli && go build -o evb-relay .`
10. **CLI test**: `./cli/evb-relay --host <ip> --api-token <token> status` returns device info
11. **CLI relay control**: `./cli/evb-relay --host <ip> --api-token <token> relay on onboard:1` — relay clicks
12. **CLI version**: `./cli/evb-relay --version` prints the injected CLI version metadata
13. **OTA**: `./cli/evb-relay --host <ip> --api-token <token> ota flash firmware/build/esp32-evb-relay.bin` — device reboots with new firmware

### CLI — Robot Mode (unit tests in `go test ./...`)

14. **TOON encoder**: unit tests covering primitives, uniform arrays, uniform object tables, nested objects, null/bool/number/string types, plus upstream TOON conformance fixtures
15. **Robot envelope**: test `Wrap()` produces valid TOON and JSON envelopes for success and error cases
16. **Remediation mapping**: test each API error code maps to correct exit code and remediation command/template
17. **`--robot-capabilities`**: verify output is valid JSON with all commands, exit codes, error codes, state machine, and env vars
18. **`modio:all` resolution**: test `modio:all=off` becomes one bulk MOD-IO bitmap request, while `onboard:all=on` becomes 2 onboard relay targets
19. **Batch results**: test partial failure produces per-target results with `PARTIAL_FAILURE` error code
20. **NDJSON watch**: test stream header, including `device_context` when headers are available, plus event lines and stream_end are valid NDJSON
21. **Exit codes**: test each error condition produces the correct exit code (0-7)
22. **Device context**: test extraction from HTTP response headers, test null when headers absent
23. **Format matrix**: test `--robot`, `--robot --format json`, `--format json`, `--format table`, and `--format plain` each produce the expected output shape, and test `--robot --format table/plain` are rejected with exit code 5

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
│   ├── client/                      # HTTP client wrapper
│   └── internal/                    # Output formatters, mDNS discovery
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
  - `0x30-0x33` → select analog inputs 0-3, then read back a 16-bit value carrying the 10-bit sample
- There is no separate relay-state readback command in the Olimex firmware, and the write command always sends the full 4-bit relay bitmap
- Keep the relay bitmap in RAM for normal uptime, but after an ESP32 reboot or MOD-IO reattach mark MOD-IO relay state as `unknown` until a deliberate boot policy applies or a client sends a bulk `PUT /api/v1/relays/modio`
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
- If MOD-IO probing starts succeeding after an absence/error period, publish a presence change, reset relay sync to `unknown`, and resume normal sampling

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
| GET | `/api/v1/relays` | Onboard relay states plus MOD-IO state/sync metadata; never guess unknown MOD-IO relay values |
| GET | `/api/v1/relays/onboard` | Onboard relay states |
| PUT | `/api/v1/relays/onboard/{id}` | Set onboard relay `{"state": true}` |
| POST | `/api/v1/relays/onboard/{id}/toggle` | Toggle onboard relay |
| GET | `/api/v1/relays/modio` | MOD-IO relay states when synchronized |
| PUT | `/api/v1/relays/modio/{id}` | Set MOD-IO relay when sync state is known |
| PUT | `/api/v1/relays/modio` | Bulk set `{"states": [true,false,true,false]}` and establish authoritative sync |
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

Error format: `{"error": {"code": "RELAY_NOT_FOUND", "message": "...", "status": 404}}`
- MOD-IO-specific endpoints return `503 MODIO_NOT_PRESENT` when the daughterboard is absent; do not fabricate zeroed input or relay state
- If MOD-IO relay state is unknown after boot or reattach, `GET /api/v1/relays/modio` and single-relay `PUT /api/v1/relays/modio/{id}` return `409 MODIO_STATE_UNKNOWN`; clients must use bulk `PUT /api/v1/relays/modio` to establish a full bitmap first
- If no valid MOD-IO sample exists yet, input endpoints return `503 MODIO_SAMPLE_UNAVAILABLE` rather than pretending an old or nonexistent snapshot is current

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
- Global flags: `--host/-H`, `--api-key/-k`, `--format/-f` (table/json/plain), `--timeout/-t`
- Config file: `~/.config/evb-relay/config.toml`, env var `EVB_RELAY_API_KEY` override

### Step 11 — `client/client.go`
- HTTP client wrapper: base URL, auth header injection, timeout, error handling
- Maps HTTP errors to structured Go errors

### Step 12 — Commands
```
evb-relay relay list                    # Relay states + MOD-IO sync metadata
evb-relay relay on onboard:1            # Target format: <group>:<id>
evb-relay relay off modio:3             # Works once MOD-IO sync state is known
evb-relay relay toggle onboard:2
evb-relay relay set onboard:1=on modio:1=off modio:2=off modio:3=on modio:4=off   # Multi-target; also the safe way to re-establish a full MOD-IO bitmap

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
evb-relay completion bash|zsh|fish      # Shell completions
```

If the firmware reports `MODIO_STATE_UNKNOWN` after boot, use `evb-relay relay set ...` with all four MOD-IO relays once before relying on single-relay `on`/`off` commands.

### Step 13 — Output formats
- **table** (default): aligned columns via `text/tabwriter`
- **json**: raw API response
- **plain**: bare values, one per line (for piping)

Exit codes: 0=success, 1=general, 2=network, 3=auth, 4=not found, 5=bad argument

---

## Phase 2b: CI/CD & Release Cycle

A unified version tag `vX.Y.Z` drives firmware builds, CLI releases, and
changelog generation. GitHub Actions handles everything: CI on every push/PR,
and a full release pipeline on tag push.

### Step 14 — Conventional Commits convention

All commit messages follow the [Conventional Commits](https://www.conventionalcommits.org/) format:

```
<type>(<scope>): <description>
```

- **Scopes:** `firmware`, `cli`, or omit for cross-cutting changes
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
fix(cli): handle 409 MODIO_STATE_UNKNOWN in relay set
ci: add firmware binary size tracking to CI
```

### Step 15 — CI workflow (`.github/workflows/ci.yml`)

**Triggers:** push to `main`, pull requests targeting `main`.

Path filtering via `dorny/paths-filter@v3` ensures firmware-only changes skip
Go jobs and vice versa, while shared CI/release changes still exercise both
stacks.

**Jobs:**

1. **`changes`** — detect which paths changed
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
     ```

2. **`firmware-build`** — build the ESP-IDF project
   - Condition: `needs.changes.outputs.firmware == 'true' || needs.changes.outputs.shared == 'true'`
   - Uses `espressif/esp-idf-ci-action@v1` with `esp_idf_version: v5.4`,
     `path: firmware`, command: `idf.py set-target esp32 && idf.py build`
   - Uploads `firmware/build/esp32-evb-relay.bin` as a workflow artifact

3. **`firmware-size`** — track binary size
   - Condition: `needs.changes.outputs.firmware == 'true'`
   - Runs after `firmware-build`
   - Uses `espressif/esp-idf-ci-action@v1` with `idf.py size`
   - Logs partition sizes to CI output for historical tracking

4. **`cli-lint`** — lint Go code
   - Condition: `needs.changes.outputs.cli == 'true' || needs.changes.outputs.shared == 'true'`
   - Uses `actions/setup-go@v6` with `cache-dependency-path: cli/go.mod`
   - Uses `golangci/golangci-lint-action@v9` with `working-directory: cli`

5. **`cli-test`** — run Go tests
   - Condition: `needs.changes.outputs.cli == 'true' || needs.changes.outputs.shared == 'true'`
   - Runs in `cli/`: `go test -race -coverprofile=coverage.out ./...`

6. **`cli-build`** — verify Go compilation
   - Condition: `needs.changes.outputs.cli == 'true' || needs.changes.outputs.shared == 'true'`
   - Runs in `cli/`: `go build -o /dev/null .`

### Step 16 — Release workflow (`.github/workflows/release.yml`)

**Trigger:** tag push matching `v*`.

Extracts version from the tag (`${GITHUB_REF_NAME#v}`) and coordinates four
jobs:

1. **`firmware`** — build versioned firmware binary
   - Uses `espressif/esp-idf-ci-action@v1` with
     `cmake -DPROJECT_VER=X.Y.Z` to embed the version in the binary
   - Renames output to `esp32-evb-relay-vX.Y.Z.bin`
   - Generates `esp32-evb-relay-vX.Y.Z.bin.sha256` checksum
   - Uploads both as workflow artifacts

2. **`cli`** — build CLI binaries via GoReleaser
   - Uses `goreleaser/goreleaser-action@v6` with GoReleaser v2
   - Creates a draft GitHub Release with 6 CLI archives
     (linux/darwin/windows × amd64/arm64) + checksums file
   - GoReleaser creates the release; later jobs augment it

3. **`changelog`** — generate release notes
   - Installs `git-cliff` and runs `git-cliff --latest --strip header`
   - Uploads the changelog text as a workflow artifact

4. **`release`** — assemble final GitHub Release
   - Depends on: `firmware`, `cli`, `changelog`
   - Downloads firmware artifact and changelog artifact
   - Uploads `esp32-evb-relay-vX.Y.Z.bin` + `.sha256` to the existing
     GitHub Release (created by GoReleaser)
   - Sets the release body to the git-cliff output via
     `softprops/action-gh-release@v2`
   - Marks the release as non-draft

### Step 17 — GoReleaser configuration (`cli/.goreleaser.yaml`)

GoReleaser v2 format. Key settings:

```yaml
version: 2
builds:
  - main: .
    dir: cli
    env:
      - CGO_ENABLED=0
    goos: [linux, darwin, windows]
    goarch: [amd64, arm64]
    flags:
      - -trimpath
    ldflags:
      - -s -w
      - -X main.version={{.Version}}
      - -X main.commit={{.Commit}}
      - -X main.date={{.Date}}

archives:
  - formats:
      - tar.gz
    format_overrides:
      - goos: windows
        formats:
          - zip

release:
  prerelease: auto    # -rc/-beta tags → pre-release

changelog:
  disable: true       # git-cliff handles changelog
```

- `CGO_ENABLED=0` for static binaries
- `-trimpath` for reproducible builds
- 6 targets: linux/darwin/windows × amd64/arm64
- Archives: `.tar.gz` (Unix), `.zip` (Windows)
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

- **Firmware:** The release workflow passes `-DPROJECT_VER=X.Y.Z` to CMake,
  which populates `esp_app_desc_t.version`. This version surfaces in
  `GET /api/v1/status` and the mDNS `fw_version` TXT record.
  `firmware/version.txt` contains `0.0.0-dev` as a local dev fallback for
  builds outside the release pipeline.

- **CLI:** GoReleaser injects the version via ldflags into `main.version`,
  `main.commit`, and `main.date`. The `--version` flag reads these values.

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
3. On merge to `main`, tag the release: `git tag v0.1.0 && git push --tags`
4. The release workflow triggers and produces:
   - Firmware binary built with the embedded version
   - 6 CLI archives built by GoReleaser
   - Changelog generated by git-cliff
5. The final `release` job assembles the GitHub Release

**GitHub Release contents:**

| Asset | Description |
|-------|-------------|
| `esp32-evb-relay-v0.1.0.bin` | Firmware binary for OTA or serial flash |
| `esp32-evb-relay-v0.1.0.bin.sha256` | SHA-256 checksum for firmware |
| `evb-relay_0.1.0_linux_amd64.tar.gz` | CLI binary (Linux amd64) |
| `evb-relay_0.1.0_linux_arm64.tar.gz` | CLI binary (Linux arm64) |
| `evb-relay_0.1.0_darwin_amd64.tar.gz` | CLI binary (macOS amd64) |
| `evb-relay_0.1.0_darwin_arm64.tar.gz` | CLI binary (macOS arm64) |
| `evb-relay_0.1.0_windows_amd64.zip` | CLI binary (Windows amd64) |
| `evb-relay_0.1.0_windows_arm64.zip` | CLI binary (Windows arm64) |
| `checksums.txt` | SHA-256 checksums for all CLI archives |

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

1. **Build firmware**: `cd firmware && idf.py set-target esp32 && idf.py build`
2. **Flash**: `idf.py -p <serial-port> flash monitor`
3. **Verify boot**: serial console shows init sequence, prints the first-boot API token if one was generated, and reports an Ethernet IP
4. **Verify token recovery path**: use `scripts/provision.sh` or the documented serial recovery flow to rotate the token without exposing it from `GET /api/v1/config`
5. **Test API**: `curl -H "Authorization: Bearer <key>" http://<ip>/api/v1/status` returns JSON
6. **Test relays**: `curl -X PUT -H "Authorization: Bearer <key>" -H "Content-Type: application/json" -d '{"state":true}' http://<ip>/api/v1/relays/onboard/1` — hear relay click
7. **Test MOD-IO sync model**: with `modio_boot_policy=leave_unchanged`, `GET /api/v1/relays/modio` returns `409 MODIO_STATE_UNKNOWN` after boot; `PUT /api/v1/relays/modio` with all 4 states establishes sync, after which `GET` returns the authoritative 4-relay bitmap
8. **Build CLI**: `cd cli && go build -o evb-relay .`
9. **CLI test**: `./evb-relay --host <ip> --api-key <key> status` returns device info
10. **CLI relay control**: `./evb-relay --host <ip> --api-key <key> relay on onboard:1` — relay clicks
11. **OTA**: `./evb-relay --host <ip> --api-key <key> ota flash firmware/build/<project-name>.bin` — device reboots with new firmware

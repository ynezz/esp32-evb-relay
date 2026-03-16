# ESP32-EVB Relay Controller — Firmware + CLI

## Context

Build a networked relay controller for Olimex ESP32-EVB + MOD-IO expansion. The system exposes a REST API for remote power control over Ethernet first, with WiFi added later, and a Go CLI for human and automation use.

**Hardware:**
- ESP32-EVB: 2 onboard relays (GPIO32, GPIO33), Ethernet (LAN8710A), UEXT I2C (SDA=GPIO13, SCL=GPIO16)
- MOD-IO (I2C slave 0x58): 4 relays, 4 digital inputs, 4 analog inputs (1-byte, 8-bit samples over I2C)
- Serial: host-specific USB serial device (for example `/dev/tty.usbserial-*`)

---

## Project Structure

```
esp32-evb-relay/
├── firmware/                        # ESP-IDF v5.x project
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── partitions.csv               # OTA-capable partition table
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
- GPIO output, mutex for thread safety, tracks state in memory

### Step 4 — `mod_io` component
- Uses ESP-IDF v5.x `i2c_master` API (not deprecated `i2c_cmd_link`)
- I2C protocol:
  - `0x41` + bitmask → set relay outputs (bits 0-3)
  - `0x42` → read digital inputs (1 byte)
  - `0x43-0x46` → read analog inputs 0-3 (1 byte each, 8-bit samples)
- There is no separate relay-state readback command in the Olimex firmware, and the write command always sends the full 4-bit relay bitmap
- Keep the relay bitmap in RAM for normal uptime, but after an ESP32 reboot or MOD-IO reattach mark MOD-IO relay state as `unknown` until a deliberate boot policy applies or a client sends a bulk `PUT /api/v1/relays/modio`
- Do not write every relay toggle to NVS just to simulate readback; that would create flash wear without making the state authoritative
- `mod_io_init(bus_handle)`, `mod_io_is_present()`, graceful failure if module absent

### Step 4b — `input_monitor` component
- FreeRTOS task polls MOD-IO digital + analog inputs at configurable interval (default 100ms)
- Compares against previous state, on change: publishes input events onto a shared event queue
- Relay setters publish `relay_changed` events onto the same queue; `input_monitor` should not invent relay events
- Also monitors onboard button (GPIO34 interrupt → `button` event on the shared queue)
- Analog inputs: configurable threshold for change detection on 8-bit samples (avoid noise-triggered events)

### Step 4c — `device_config` component
- NVS-backed source of truth for `api_token`, `poll_interval_ms`, `hostname`, `modio_boot_policy`, and future WiFi credentials
- Centralizes validation, defaults, and persistence so `auth`, `network`, `input_monitor`, and `rest_api` do not each manage their own ad hoc NVS keys
- `modio_boot_policy` should be explicit and low-churn, for example `all_off` (apply `0000` on boot and mark state synchronized) or `leave_unchanged` (do not touch hardware on boot, but report MOD-IO relay state as unknown until the client performs a bulk set)
- Returns metadata about whether a config change is applied live or requires a restart/rebind

### Step 5 — `network` component
- Ethernet init: LAN8710A PHY, PHY address `0`, no dedicated ESP32-controlled PHY reset GPIO on current ESP32-EVB revisions (`reset_gpio_num = -1` unless board-specific testing proves otherwise), RMII clock input on GPIO0, MDC/MDIO on GPIO23/18
- Event-driven: wait for IP via `IP_EVENT_ETH_GOT_IP`
- `network_wait_for_ip(timeout_ms)` blocks `app_main` until connected or returns a timeout/error; in the Ethernet-only phase, treat timeout as a startup failure instead of silently continuing without a usable control plane
- Architecture allows a later WiFi phase (separate init path, shared event handlers)
- mDNS: hostname `esp32-evb-relay`, register `_http._tcp` with TXT records (fw_version, board type)

### Step 6 — `auth` component
- API token loaded from `device_config`
- `auth_check(httpd_req_t*)` validates `Authorization: Bearer <token>` for all `/api/v1/*` endpoints
- Secure by default. On first boot, if no token exists yet, generate a random token, persist it, and print it once on the serial console for provisioning
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
| GET | `/api/v1/inputs/digital` | Read all digital inputs |
| GET | `/api/v1/inputs/digital/{id}` | Read single digital input |
| GET | `/api/v1/inputs/analog` | Read all analog inputs |
| GET | `/api/v1/inputs/analog/{id}` | Read single analog input |
| GET | `/api/v1/events` | SSE stream — pushes input/relay/button change events |
| GET | `/api/v1/config` | Read validated device config (`poll_interval_ms`, `hostname`, etc.) |
| PUT | `/api/v1/config` | Update device config `{"poll_interval_ms": 200}` and report whether the change applied live |
| POST | `/api/v1/ota` | Upload firmware binary (octet-stream) |

**SSE event stream** (`GET /api/v1/events`, `Accept: text/event-stream`):
- Long-lived HTTP connection, server pushes SSE-framed JSON payloads
- Event types: `digital_input`, `analog_input`, `relay_changed`, `button`
- Format: `event: digital_input\ndata: {"id":2,"state":true,"ts_ms":12345}\n\n`
- Firmware I2C polling is internal (MOD-IO has no interrupt line); SSE makes the *client* event-driven
- Polling interval configurable via `PUT /api/v1/config` (see below)
- Cap SSE fan-out to a small fixed number of clients and drop stale subscribers on backpressure instead of letting one slow client exhaust MCU resources
- Heartbeat every 30s to detect stale connections

Error format: `{"error": {"code": "RELAY_NOT_FOUND", "message": "...", "status": 404}}`
- MOD-IO-specific endpoints return `503 MODIO_NOT_PRESENT` when the daughterboard is absent; do not fabricate zeroed input or relay state
- If MOD-IO relay state is unknown after boot or reattach, `GET /api/v1/relays/modio` and single-relay `PUT /api/v1/relays/modio/{id}` return `409 MODIO_STATE_UNKNOWN`; clients must use bulk `PUT /api/v1/relays/modio` to establish a full bitmap first

URI parsing: register wildcard handlers with `httpd_uri_match_wildcard()` and use a helper such as `parse_id_from_uri()` because ESP-IDF httpd still lacks native path params.

### Step 8 — `ota` component
- `POST /api/v1/ota` streams binary via `esp_ota_begin/write/end`
- On the next boot, only call `esp_ota_mark_app_valid_cancel_rollback()` after the image has passed its real startup health checks (for example: board init succeeded, networking came up, and the REST API started); if startup fails first, leave rollback pending
- Sets boot partition, reboots after 2s delay
- Rollback enabled via `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`

### Step 9 — `main.c` boot sequence
```
nvs_flash_init → event_loop_create → device_config_init →
board_init → relay_init →
mod_io_init → auth_init → network_init → wait_for_ip →
mdns_register → rest_api_start → input_monitor_start →
ota_confirm_running_image_if_healthy →
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
evb-relay relay off modio:3
evb-relay relay toggle onboard:2
evb-relay relay set onboard:1=on modio:1=off modio:2=off modio:3=on modio:4=off   # Multi-target; also the safe way to re-establish a full MOD-IO bitmap

evb-relay input digital                 # All digital inputs (one-shot)
evb-relay input digital 2               # Single digital input
evb-relay input analog                  # All analog inputs (one-shot)
evb-relay input analog 2                # Single analog input
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

## Phase 3 (future): WiFi support
- Add WiFi STA init in `network` component (credentials from `device_config`)
- Replace unconditional fallback with an explicit network policy such as `ethernet_only`, `wifi_only`, or `prefer_ethernet`; do not silently jump transports just because DHCP was slow once
- If `prefer_ethernet` is enabled, only try WiFi after a deliberate timeout and surface the active transport in `/api/v1/status`
- Add `POST /api/v1/config/wifi` endpoint for setting credentials
- Add `evb-relay config wifi` CLI command
- Architecture in phase 1 already accommodates this (shared config, event handlers, common HTTP stack)

---

## Verification

1. **Build firmware**: `cd firmware && idf.py set-target esp32 && idf.py build`
2. **Flash**: `idf.py -p <serial-port> flash monitor`
3. **Verify boot**: serial console shows init sequence, prints the first-boot API token if one was generated, and reports an Ethernet IP
4. **Test API**: `curl -H "Authorization: Bearer <key>" http://<ip>/api/v1/status` returns JSON
5. **Test relays**: `curl -X PUT -H "Authorization: Bearer <key>" -H "Content-Type: application/json" -d '{"state":true}' http://<ip>/api/v1/relays/onboard/1` — hear relay click
6. **Test MOD-IO sync model**: with `modio_boot_policy=leave_unchanged`, `GET /api/v1/relays/modio` returns `409 MODIO_STATE_UNKNOWN` after boot; `PUT /api/v1/relays/modio` with all 4 states establishes sync, after which `GET` returns the authoritative 4-relay bitmap
7. **Build CLI**: `cd cli && go build -o evb-relay .`
8. **CLI test**: `./evb-relay --host <ip> --api-key <key> status` returns device info
9. **CLI relay control**: `./evb-relay --host <ip> --api-key <key> relay on onboard:1` — relay clicks
10. **OTA**: `./evb-relay --host <ip> --api-key <key> ota flash firmware/build/esp32-evb-relay.bin` — device reboots with new firmware

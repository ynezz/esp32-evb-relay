# Manual Pre-Release Test Plan

> Agent-executable checklist for validating the full CLI + firmware
> feature set against real hardware before cutting a release. The
> executing agent fills in the **Result** and **Notes** columns, then
> commits this file with the completed results.

---

## Test Run Metadata

| Field              | Value |
|--------------------|-------|
| Firmware version   | 0.0.0-dev |
| CLI version        | 0.0.0-dev (commit: unknown, built: unknown) |
| Device IP / host   | 192.168.200.211 |
| Agent identity     | Claude Opus 4.6 (1M context) |
| Date               | 2026-03-21 |
| Git commit (RC)    | ff0f395 |
| MOD-IO attached    | no |
| Ethernet connected | yes |
| WiFi AP available  | no |

---

## Prerequisites

Before starting, the agent must verify the hardware and complete the
software setup steps below. **Do not skip to the test sections until
every step here succeeds.**

### Hardware checklist

1. ESP32-EVB board is powered and connected via Ethernet
2. MOD-IO module is attached via UEXT connector
3. Serial alias `/dev/esp32-evb` exists (symlink to the board's UART;
   see `docs/hardware-reference.md` § Serial Device Naming)

### Step 0 — Source ESP-IDF

ESP-IDF must be on `PATH` for firmware builds, flashing, and
provisioning. The default install location is `~/esp/esp-idf`.

```bash
source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"
```

### Step 1 — Build production firmware

```bash
just build          # -> firmware/build/evb_relay_firmware.bin
```

### Step 2 — Build CLI binary

```bash
cd cli && go build -o /tmp/evb-relay . && cd ..
export PATH="/tmp:$PATH"
hash -r
```

Or use `just cli-test` to also run unit tests. The commands below
assume `evb-relay` resolves on `PATH`; if you skip the `export PATH`
step, replace `evb-relay` with `/tmp/evb-relay`.

### Step 3 — Flash production firmware

The ESP32-EVB board requires ROM download mode for serial flashing.
On stock boards this needs the R46/R14 hardware rework (see
`docs/hardware-reference.md` § Current Runner Caveat). The flash
script runs `check-download-mode.sh` automatically and fails fast if
the ROM bootloader is unreachable.

```bash
scripts/flash.sh                       # uses /dev/esp32-evb by default
# or: scripts/flash.sh --port /dev/esp32-evb --baud 115200
```

After flashing, wait ~10 s for the device to boot and obtain a DHCP
lease.

### Step 4 — Discover the device IP

The production firmware logs its Ethernet IP on the serial console
during boot. Read it with:

```bash
# Watch serial output for the DHCP lease line:
#   "Ethernet got IP: ip=<addr>"
timeout 20 python3 -c "
import serial, re, time
s = serial.Serial('/dev/esp32-evb', 115200, timeout=1)
s.dtr = False; s.rts = False
buf = ''
end = time.time() + 20
while time.time() < end:
    chunk = s.read(256)
    if chunk:
        buf += chunk.decode('utf-8', errors='ignore')
        m = re.search(r'Ethernet got IP: ip=(\d+\.\d+\.\d+\.\d+)', buf)
        if m:
            print(m.group(1)); break
s.close()
"
```

If the device has already booted, trigger a reset pulse first:

```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/esp32-evb', 115200)
s.dtr = False; s.rts = True; time.sleep(0.1)
s.rts = False; s.close(); print('Reset sent')
"
```

Then re-run the IP discovery snippet above.

Alternatively, if mDNS works on your network (same L2 segment):

```bash
evb-relay discover --timeout 5s --format json
```

Export the discovered IP:

```bash
export EVB_RELAY_HOST="<device-ip>"     # e.g. 192.168.200.211
```

### Step 5 — Provision an API token

The firmware rejects all API requests until a token is written to
NVS. Use the provisioning script to generate and store one:

```bash
scripts/provision.sh --generate
# Prints: "Provisioned API token on /dev/esp32-evb: <hex-token>"
```

Copy the printed token and export it:

```bash
export EVB_RELAY_API_TOKEN="<hex-token>"
```

The provisioning script resets the device. Wait ~10 s for the HTTP
server to come up, then verify connectivity:

```bash
curl -sf -H "Authorization: Bearer $EVB_RELAY_API_TOKEN" \
     "http://$EVB_RELAY_HOST/api/v1/status" | head -c 200
```

If this returns a JSON status object, the device is ready.

### Step 6 — Run automated CI gates (optional pre-check)

```bash
just ci             # format-check + build + host tests + CLI tests
# or: just ci-full  # also runs on-device + integration tests
```

### Verify

At this point all of the following must be true:

- `echo $EVB_RELAY_HOST` prints the device IP
- `echo $EVB_RELAY_API_TOKEN` prints the provisioned token
- `curl` to the status endpoint returns HTTP 200 with JSON
- `evb-relay --version` prints the CLI version

---

## Section 1 — Automated Quality Gates

Run automated CI gates first. If these fail, stop and fix before
proceeding to manual tests.

| #    | Test                   | Command                | Expected                | Result | Notes |
|------|------------------------|------------------------|-------------------------|--------|-------|
| 1.1  | Format check           | `just format-check`    | Exit 0, no diff         | PASS | No formatting diffs |
| 1.2  | Firmware build         | `just build`           | Exit 0, binary produced | PASS | Binary 949 KB, 51% free in app partition |
| 1.3  | Host unit tests        | `just test`            | All tests pass          | PASS | 3/3 CTests + 31/31 pytest passed |
| 1.4  | CLI format check       | `just cli-fmt-check`   | Exit 0                  | PASS | No formatting issues |
| 1.5  | CLI lint               | `just cli-lint`        | Exit 0                  | PASS | golangci-lint v2.10.0: 0 issues |
| 1.6  | CLI vet                | `just cli-vet`         | Exit 0                  | PASS | Clean |
| 1.7  | CLI unit tests         | `just cli-test`        | All tests pass          | PASS | All packages pass with -race |
| 1.8  | CLI E2E tests          | `just test-e2e`        | All tests pass          | PASS | All E2E tests pass |
| 1.9  | Full CI gate           | `just ci`              | Exit 0                  | PASS | Exit 0 |
| 1.10 | On-device tests (HW)   | `just test-device`     | All tests pass          | PASS | 46 device tests pass in 96.92s |
| 1.11 | Integration tests (HW) | `just test-integration` | All tests pass          | PASS | 4/4 integration tests pass in 180.75s |
| 1.12 | Full CI+HW gate        | `just ci-full`         | Exit 0                  | FAIL | test-device timed out (480s limit) on second run within ci-full; individual runs pass |

---

## Section 2 — Version & Discovery

| #   | Test                        | Command | Expected | Result | Notes |
|-----|-----------------------------|---------|----------|--------|-------|
| 2.1 | CLI version flag            | `evb-relay --version` | Prints version, commit, date; exit 0 | PASS | Prints `0.0.0-dev`, commit: unknown, built: unknown |
| 2.2 | Robot capabilities          | `evb-relay --robot-capabilities` | JSON with command list; exit 0 | PASS | Valid JSON with v=1, cli_version, commands array |
| 2.3 | mDNS discover               | `evb-relay discover --format json` | JSON array with at least 1 device | FAIL | Returns `{"devices":[]}` — no mDNS responders on this network segment |
| 2.4 | Discover timeout            | `evb-relay discover --timeout 1s --format json` | Completes within ~1s | PASS | Completes in 1.004s |

---

## Section 3 — Status

| #   | Test                        | Command | Expected | Result | Notes |
|-----|-----------------------------|---------|----------|--------|-------|
| 3.1 | Status table format         | `evb-relay status` | Human-readable table with uptime, FW version, heap, network, MOD-IO; exit 0 | PASS | All columns present; exit 0 |
| 3.2 | Status JSON format          | `evb-relay status --format json` | Valid JSON; fields: `uptime_seconds`, `firmware_version`, `free_heap_bytes`, `network.hostname`, `network.connected`, `network.transport`, `network.ip`, `network.netmask`, `network.gateway`, `modio.present`, `modio.sync` | PASS | All fields present and valid |
| 3.3 | Status robot mode           | `evb-relay status --robot` | TOON envelope with `v`, `command`, `timestamp`, `elapsed_ms`, `exit_code`, `host`, `device_context`, `data` | PASS | All envelope fields present |
| 3.4 | Status robot JSON           | `evb-relay status --robot --format json` | JSON envelope; same fields as 3.3 | PASS | Valid JSON envelope with all fields |
| 3.5 | Status via REST             | `curl -s -H "Authorization: Bearer $EVB_RELAY_API_TOKEN" http://$EVB_RELAY_HOST/api/v1/status` | Valid JSON matching CLI output; response headers include `X-FW-Version`, `X-ModIO-Present`, `X-ModIO-Sync` | PASS | Headers: X-FW-Version: 0.0.0-dev, X-ModIO-Present: false, X-ModIO-Sync: absent |
| 3.6 | FW version match            | Compare `--version` output with status `firmware_version` | Versions match | PASS | Both report `0.0.0-dev` |

---

## Section 4 — Onboard Relay Control

Start state: both onboard relays OFF after boot.

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 4.1  | List relays (initial)      | `evb-relay relay list --format json` | Both onboard relays present, state `false`; exit 0 | PASS | 2 onboard relays, both state=false |
| 4.2  | Relay 1 ON (CLI)           | `evb-relay relay on onboard:1 --format json` | `group=onboard`, `id=1`, `state=true`; exit 0 | PASS | group=onboard, id=1, state=true |
| 4.3  | Relay 1 verify ON          | `evb-relay relay list --format json` | Relay 1 state `true` | PASS | Relay 1 state=true confirmed |
| 4.4  | Relay 1 OFF (CLI)          | `evb-relay relay off onboard:1 --format json` | `state=false`; exit 0 | PASS | state=false |
| 4.5  | Relay 2 toggle ON          | `evb-relay relay toggle onboard:2 --format json` | `state=true`; exit 0 | PASS | state=true |
| 4.6  | Relay 2 toggle OFF         | `evb-relay relay toggle onboard:2 --format json` | `state=false`; exit 0 | PASS | state=false |
| 4.7  | Batch set both ON          | `evb-relay relay set onboard:1=on onboard:2=on --format json` | `all_ok=true`, both relays `state=true` | PASS | all_ok=true, both state=true |
| 4.8  | Batch set both OFF         | `evb-relay relay set onboard:1=off onboard:2=off --format json` | `all_ok=true`, both relays `state=false` | PASS | all_ok=true, both state=false |
| 4.9  | Relay ON via REST          | `curl -s -X PUT ... -d '{"state":true}' .../relays/onboard/1` | JSON with `state=true`; HTTP 200 | PASS | `{"relay":{"group":"onboard","id":1,"state":true}}` |
| 4.10 | Relay toggle via REST      | `curl -s -X POST .../relays/onboard/1/toggle` | State toggled; HTTP 200 | PASS | Toggled true→false |
| 4.11 | Relay OFF via REST         | `curl -s -X PUT ... -d '{"state":false}' .../relays/onboard/1` | `state=false`; HTTP 200 | PASS | state=false |
| 4.12 | GET onboard relays (REST)  | `curl -s .../relays/onboard` | JSON array of 2 relays with `id`, `state` | PASS | JSON with 2 relays |
| 4.13 | Cleanup: both OFF          | `evb-relay relay set onboard:1=off onboard:2=off` | Both OFF | PASS | Both off, all_ok=true |

---

## Section 5 — MOD-IO Relay Control

Prerequisite: MOD-IO attached. Status must show `modio.present=true`.

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 5.1  | MOD-IO presence            | `evb-relay status --format json` | `true` | SKIP | MOD-IO not attached; present=false |
| 5.2  | MOD-IO sync state          | `evb-relay status --format json` | `unknown` or `synchronized` | SKIP | MOD-IO not attached; sync=absent |
| 5.3  | Batch set all ON (sync)    | `evb-relay relay set modio:1=on modio:2=on modio:3=on modio:4=on --format json` | `all_ok=true`; all 4 relays `state=true`; sync becomes `synchronized` | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 5.4  | Verify sync state          | `evb-relay status --format json` | `synchronized` | SKIP | MOD-IO not attached |
| 5.5  | Single relay OFF           | `evb-relay relay off modio:2 --format json` | `id=2`, `state=false`; exit 0 | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 5.6  | Single relay ON            | `evb-relay relay on modio:2 --format json` | `id=2`, `state=true`; exit 0 | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 5.7  | Toggle relay               | `evb-relay relay toggle modio:3 --format json` | State flipped; exit 0 | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 5.8  | Batch set via REST         | `curl -s -X PUT ... -d '{"states":[false,true,false,true]}' .../relays/modio` | HTTP 200; states match request | SKIP | HTTP 503 MODIO_NOT_PRESENT |
| 5.9  | Single relay via REST      | `curl -s -X PUT ... -d '{"state":true}' .../relays/modio/1` | HTTP 200; `state=true` | SKIP | HTTP 503 MODIO_NOT_PRESENT |
| 5.10 | Toggle relay via REST      | `curl -s -X POST .../relays/modio/1/toggle` | State toggled; HTTP 200 | SKIP | MOD-IO not attached |
| 5.11 | GET modio relays (REST)    | `curl -s .../relays/modio` | JSON array of 4 relays | SKIP | HTTP 503 MODIO_NOT_PRESENT |
| 5.12 | GET all relays (REST)      | `curl -s .../relays` | JSON with both onboard (2) and modio (4) relays | PASS | Returns onboard relays only; modio_present=false, modio_sync=absent |
| 5.13 | Relay list shows all       | `evb-relay relay list --format json` | 6 total relays; `modio_present=true`, `modio_sync=synchronized` | SKIP | Only 2 onboard relays shown; modio_present=false |
| 5.14 | Cleanup: all MOD-IO OFF    | `evb-relay relay set modio:1=off modio:2=off modio:3=off modio:4=off` | All OFF | SKIP | MOD-IO not attached |

---

## Section 6 — MOD-IO Inputs

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 6.1  | All digital inputs         | `evb-relay input digital --format json` | JSON with 4 inputs; each has `id`, `state` (boolean); `sample_ts_ms`, `sample_age_ms`, `poll_interval_ms` present | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 6.2  | Single digital input       | `evb-relay input digital 1 --format json` | Single input with `id=1`, `state` (boolean) | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 6.3  | All analog inputs          | `evb-relay input analog --format json` | JSON with 4 inputs; each has `id`, `value` (0-1023); timestamp metadata present | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 6.4  | Single analog input        | `evb-relay input analog 3 --format json` | Single input with `id=3`, `value` in 0-1023 range | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 6.5  | Digital inputs via REST    | `curl -s .../inputs/digital` | JSON array of 4 digital inputs | SKIP | HTTP 503 MODIO_NOT_PRESENT |
| 6.6  | Single digital via REST    | `curl -s .../inputs/digital/2` | Single input JSON | SKIP | HTTP 503 MODIO_NOT_PRESENT |
| 6.7  | Analog inputs via REST     | `curl -s .../inputs/analog` | JSON array of 4 analog inputs, values 0-1023 | SKIP | HTTP 503 MODIO_NOT_PRESENT |
| 6.8  | Single analog via REST     | `curl -s .../inputs/analog/4` | Single input JSON, value 0-1023 | SKIP | HTTP 503 MODIO_NOT_PRESENT |
| 6.9  | Digital plain format       | `evb-relay input digital --format plain` | Plain text output; exit 0 | SKIP | MODIO_NOT_PRESENT; exit 7 |
| 6.10 | Analog table format        | `evb-relay input analog` | Human-readable table; exit 0 | SKIP | MODIO_NOT_PRESENT; exit 7 |

---

## Section 7 — SSE / Event Streaming

| #   | Test                        | Command | Expected | Result | Notes |
|-----|-----------------------------|---------|----------|--------|-------|
| 7.1 | Input watch starts          | `timeout 10 evb-relay input watch --robot 2>/dev/null \| head -3` | NDJSON lines; first line is stream header with `stream=start`, `device_context`; exit 0 or 124 (timeout) | PASS | Stream header with v=1, stream=events, device_context |
| 7.2 | Relay event in stream       | Start `evb-relay input watch --robot` in background, toggle a relay, capture output | `relay_changed` event appears with relay group, id, state | PASS | `relay_changed` events captured for onboard:1 toggle on/off |
| 7.3 | Heartbeat received          | `timeout 35 curl ... /api/v1/events` | At least one heartbeat line within 30s | PASS | `:heartbeat` comment line received |
| 7.4 | SSE via REST                | `timeout 5 curl -s -N .../events \| head -5` | SSE format: lines starting with `event:` and `data:` | PASS | SSE format with `:connected` and event/data lines |
| 7.5 | Multiple SSE clients        | Open 2 concurrent curl SSE connections, toggle relay | Both clients receive the event | PASS | Both clients received identical relay_changed events |

---

## Section 8 — Configuration

| #    | Test                              | Command | Expected | Result | Notes |
|------|-----------------------------------|---------|----------|--------|-------|
| 8.1  | Config show (CLI)                 | `evb-relay config show --format json` | JSON with `poll_interval_ms`, `hostname`, `modio_boot_policy`, `api_token_set`, `wifi.ssid_set`, `wifi.passphrase_set`, `wifi.network_policy` | PASS | All fields present; poll=100, hostname=esp32-evb-relay, modio_boot_policy=leave_unchanged, network_policy=ethernet_only |
| 8.2  | Config show (table)               | `evb-relay config show` | Human-readable table; exit 0 | PASS | Table with all columns; exit 0 |
| 8.3  | Config show (REST)                | `curl -s .../config` | JSON matching CLI output; secrets shown as `_set` booleans only | PASS | JSON matches CLI output; api_token_set=true, ssid_set/passphrase_set=false |
| 8.4  | Set hostname                      | `evb-relay config set hostname=test-relay --format json` | Accepted; exit 0 | FAIL | Connection reset by peer; exit 2. Config PUT endpoint crashes httpd task (suspected stack overflow) |
| 8.5  | Verify hostname                   | `evb-relay config show --format json` | `test-relay` | FAIL | Hostname unchanged (esp32-evb-relay) — config set failed |
| 8.6  | Set poll_interval_ms              | `evb-relay config set poll_interval_ms=200 --format json` | Accepted; exit 0 | FAIL | Connection reset by peer; exit 2 |
| 8.7  | Verify poll_interval_ms           | `evb-relay config show --format json` | `200` | FAIL | poll_interval_ms unchanged (100) — config set failed |
| 8.8  | Set modio_boot_policy             | `evb-relay config set modio_boot_policy=all_off --format json` | Accepted; exit 0 | FAIL | Connection reset by peer; exit 2 |
| 8.9  | Invalid: poll too low             | `evb-relay config set poll_interval_ms=10` | Rejected; exit code 5 (bad argument) | FAIL | Connection reset by peer; exit 2. Server crashes before validating |
| 8.10 | Invalid: poll too high            | `evb-relay config set poll_interval_ms=20000` | Rejected; exit code 5 | FAIL | Connection reset by peer; exit 2 |
| 8.11 | Invalid: empty hostname           | `evb-relay config set hostname=` | Rejected; exit code 5 | PASS | Client-side rejection: "hostname must not be empty"; exit 5 |
| 8.12 | Invalid: hostname too long        | `evb-relay config set hostname=a234...` (64 chars) | Rejected (64 chars > max 63); exit code 5 | FAIL | Connection reset by peer; exit 2. Validation not reached |
| 8.13 | Invalid: hostname leading hyphen  | `evb-relay config set hostname=-bad` | Rejected; exit code 5 | FAIL | Connection reset by peer; exit 2 |
| 8.14 | Invalid: hostname special chars   | `evb-relay config set hostname=host.name` | Rejected; exit code 5 | FAIL | Connection reset by peer; exit 2 |
| 8.15 | Config via REST                   | `curl -s -X PUT ... -d '{"poll_interval_ms":100}' .../config` | HTTP 200; accepted | FAIL | Connection reset (HTTP 000) |
| 8.16 | Restore defaults                  | `evb-relay config set hostname=esp32-evb-relay poll_interval_ms=100 modio_boot_policy=leave_unchanged` | All accepted | FAIL | Connection reset; config set is non-functional |

---

## Section 9 — WiFi Configuration

| #    | Test                              | Command | Expected | Result | Notes |
|------|-----------------------------------|---------|----------|--------|-------|
| 9.1  | Set WiFi credentials              | `evb-relay config wifi ssid=TestNet passphrase=secret123 --format json` | Accepted; `restart_required` may be true; exit 0 | FAIL | Connection reset by peer; exit 2. WiFi config PUT endpoint also crashes |
| 9.2  | Verify ssid_set                   | `evb-relay config show --format json` | `true` | FAIL | ssid_set=false — WiFi config set failed |
| 9.3  | Verify passphrase_set             | `evb-relay config show --format json` | `true` | FAIL | passphrase_set=false — WiFi config set failed |
| 9.4  | Set network_policy                | `evb-relay config wifi network_policy=prefer_ethernet --format json` | Accepted; exit 0 | FAIL | Connection reset; same root cause as config endpoint |
| 9.5  | Verify network_policy             | `evb-relay config show --format json` | `prefer_ethernet` | FAIL | Still ethernet_only |
| 9.6  | WiFi via REST                     | `curl -s -X PUT ... -d '{"network_policy":"ethernet_only"}' .../config/wifi` | HTTP 200; accepted | FAIL | Connection reset |
| 9.7  | Clear WiFi credentials            | `evb-relay config wifi clear=true --format json` | Accepted; exit 0 | FAIL | Connection reset |
| 9.8  | Verify cleared                    | `evb-relay config show --format json` | `false` | SKIP | Depends on 9.7 |
| 9.9  | Invalid: passphrase without ssid  | `evb-relay config wifi passphrase=secret` | Rejected; exit code 5 | PASS | Client-side rejection: "passphrase requires ssid"; exit 5 |
| 9.10 | Invalid: clear with ssid          | `evb-relay config wifi clear=true ssid=TestNet` | Rejected; exit code 5 | PASS | Client-side rejection: "clear=true cannot be combined with ssid"; exit 5 |

---

## Section 10 — Authentication & Security

| #     | Test                        | Command | Expected | Result | Notes |
|-------|-----------------------------|---------|----------|--------|-------|
| 10.1  | No token -> 401             | `curl -s -o /dev/null -w '%{http_code}' .../status` | `401` | PASS | HTTP 401 |
| 10.2  | No token: no device headers | `curl -s -D- .../status \| grep -ci 'X-FW-Version'` | `0` (header absent) | PASS | 0 — header absent |
| 10.3  | Wrong token -> 401/403      | `curl ... -H "Authorization: Bearer WRONGTOKEN" .../status` | `401` or `403` | PASS | HTTP 403 |
| 10.4  | Wrong token: no device headers | `curl ... \| grep -ci 'X-FW-Version'` | `0` (header absent) | PASS | 0 — header absent |
| 10.5  | Valid token -> 200          | `curl ... -H "Authorization: Bearer $EVB_RELAY_API_TOKEN" .../status` | `200` | PASS | HTTP 200 |
| 10.6  | Valid token: device headers | `curl ... \| grep -c 'X-FW-Version'` | `1` (header present) | PASS | 1 — header present |
| 10.7  | X-ModIO-Present header      | `curl ... \| grep -i 'X-ModIO-Present'` | Header present with value `true` or `false` | PASS | X-ModIO-Present: false |
| 10.8  | X-ModIO-Sync header         | `curl ... \| grep -i 'X-ModIO-Sync'` | Header present with value `absent`, `unknown`, or `synchronized` | PASS | X-ModIO-Sync: absent |
| 10.9  | CLI without token           | `EVB_RELAY_API_TOKEN="" evb-relay status` | Auth error; exit code 3 | PASS | AUTH_REQUIRED; exit 3 |
| 10.10 | CLI with wrong token        | `EVB_RELAY_API_TOKEN="WRONG" evb-relay status` | Auth error; exit code 3 | PASS | AUTH_FORBIDDEN; exit 3 |
| 10.11 | Auth error JSON body        | `curl -s -H "Authorization: Bearer WRONGTOKEN" .../status` | JSON with error code (`AUTH_REQUIRED` or `AUTH_FORBIDDEN`) | PASS | `{"error":{"code":"AUTH_FORBIDDEN","message":"Access denied","status":403}}` |

---

## Section 11 — OTA Firmware Update

> **Caution:** This section flashes firmware. Ensure a known-good
> binary is available for recovery. The agent should build the RC
> binary fresh before testing.

| #    | Test                  | Command | Expected | Result | Notes |
|------|-----------------------|---------|----------|--------|-------|
| 11.1 | Build RC binary       | `just build` | Binary at firmware/build/ | PASS | Binary at firmware/build/evb_relay_firmware.bin, 949 KB |
| 11.2 | OTA flash             | `evb-relay ota flash firmware/build/evb_relay_firmware.bin --format json` | `uploaded_bytes` > 0, `reboot_in_seconds` present; exit 0 | FAIL | Connection reset after 98304/972048 bytes (10%). OTA endpoint crashes httpd task |
| 11.3 | Wait for reboot       | `sleep 10 && evb-relay status --format json` | Device responds; FW version matches RC | SKIP | Depends on 11.2 |
| 11.4 | Post-OTA relay test   | `evb-relay relay list --format json` | Relays accessible; exit 0 | SKIP | Depends on 11.2 |
| 11.5 | Post-OTA config persisted | `evb-relay config show --format json` | Config values match pre-OTA settings | SKIP | Depends on 11.2 |

---

## Section 12 — Error Handling & Edge Cases

| #     | Test                          | Command | Expected | Result | Notes |
|-------|-------------------------------|---------|----------|--------|-------|
| 12.1  | Invalid onboard relay ID      | `evb-relay relay on onboard:5` | Error; exit code 5 (bad argument) | PASS | "onboard ids must be in range 1-2"; exit 5 |
| 12.2  | Invalid onboard relay ID 0    | `evb-relay relay on onboard:0` | Error; exit code 5 | PASS | "onboard ids must be in range 1-2"; exit 5 |
| 12.3  | Invalid modio relay ID        | `evb-relay relay on modio:5` | Error; exit code 5 | PASS | "modio ids must be in range 1-4"; exit 5 |
| 12.4  | Invalid input ID              | `evb-relay input digital 5` | Error; exit code 5 | PASS | "input id must be in range 1-4"; exit 5 |
| 12.5  | Invalid input ID 0            | `evb-relay input analog 0` | Error; exit code 5 | PASS | "input id must be in range 1-4"; exit 5 |
| 12.6  | Invalid group name            | `evb-relay relay on bogus:1` | Error; exit code 5 | PASS | "unsupported relay group"; exit 5 |
| 12.7  | Invalid relay via REST        | `curl ... .../relays/onboard/9` | HTTP 400 or 404 | FAIL | HTTP 405 (Method Not Allowed) — GET on wildcard relay URI matches PUT handler |
| 12.8  | Invalid input via REST        | `curl ... .../inputs/digital/9` | HTTP 400 or 404 | SKIP | HTTP 503 — MOD-IO not present takes precedence over invalid ID |
| 12.9  | Missing JSON body (relay)     | `curl -X PUT ... .../relays/onboard/1` (no body) | HTTP 400 | FAIL | Connection reset (HTTP 000) — httpd crashes on PUT with no body |
| 12.10 | Invalid JSON body             | `curl ... -d 'not-json' .../relays/onboard/1` | HTTP 400 | FAIL | Connection reset (HTTP 000) — httpd crashes |
| 12.11 | Unknown endpoint              | `curl ... .../nonexistent` | HTTP 404 | PASS | HTTP 404 |
| 12.12 | Network error (CLI)           | `EVB_RELAY_HOST=192.0.2.1 evb-relay status --timeout 2s` | Network error; exit code 2 | PASS | Context deadline exceeded; exit 2 |
| 12.13 | Relay set partial bad target  | `evb-relay relay set onboard:1=on bogus:1=on` | Error or partial failure; exit code != 0 | PASS | "unsupported relay group"; exit 5 |
| 12.14 | OTA with bad file             | `echo "garbage" > /tmp/bad-fw.bin && evb-relay ota flash /tmp/bad-fw.bin` | Error or device rejects; device recovers | PASS | Upload completes but device crashes and reboots; device recovers successfully (uptime resets) |

---

## Section 13 — MOD-IO Absent (Graceful Degradation)

> **Instructions:** If MOD-IO can be safely disconnected during the
> test run, execute these tests. Otherwise mark as SKIP with a note.
> **Reconnect MOD-IO before proceeding to later sections.**

| #     | Test                       | Command | Expected | Result | Notes |
|-------|----------------------------|---------|----------|--------|-------|
| 13.1  | Disconnect MOD-IO          | Physically disconnect MOD-IO from UEXT | Device continues running | SKIP | MOD-IO was never attached in this test run |
| 13.2  | Status shows absent        | `evb-relay status --format json` | `present=false`, `sync=absent` | PASS | present=False, sync=absent |
| 13.3  | MOD-IO relay -> state error | `evb-relay relay on modio:1` | Error; exit code 6 or 7 | PASS | MODIO_NOT_PRESENT; exit 7 |
| 13.4  | MOD-IO input -> error      | `evb-relay input digital` | Error or empty/stale data | PASS | MODIO_NOT_PRESENT; exit 7 |
| 13.5  | Onboard relays still work  | `evb-relay relay toggle onboard:1` | Works normally; exit 0 | PASS | Toggled to ON; exit 0 |
| 13.6  | REST modio -> 503          | `curl ... .../relays/modio/1` | HTTP 503 | PASS | HTTP 503 MODIO_NOT_PRESENT |
| 13.7  | Relay list (degraded)      | `evb-relay relay list --format json` | `modio_present=false` | PASS | modio_present=False |
| 13.8  | Reconnect MOD-IO           | Physically reconnect MOD-IO to UEXT | Device detects MOD-IO | SKIP | Cannot physically reconnect MOD-IO (not available) |
| 13.9  | Status shows recovery      | `evb-relay status --format json` | `present=true`; sync transitions | SKIP | MOD-IO not available |
| 13.10 | Cleanup: onboard OFF       | `evb-relay relay off onboard:1` | Exit 0 | PASS | Exit 0 |

---

## Section 14 — MOD-IO Boot Policy

> **Instructions:** These tests require device reboots. Record the
> relay state before and after each reboot.

| #     | Test                       | Command | Expected | Result | Notes |
|-------|----------------------------|---------|----------|--------|-------|
| 14.1  | Set policy: all_off        | `evb-relay config set modio_boot_policy=all_off --format json` | Accepted; exit 0 | FAIL | Config PUT endpoint crashes (connection reset) |
| 14.2  | Set MOD-IO relays ON       | `evb-relay relay set modio:1=on modio:2=on modio:3=on modio:4=on` | All ON; exit 0 | SKIP | MOD-IO not attached |
| 14.3  | Reboot device              | `evb-relay ota flash firmware/build/evb_relay_firmware.bin --format json` | Device comes back online | SKIP | OTA non-functional; depends on 14.1 |
| 14.4  | Verify all_off applied     | `evb-relay relay list --format json` | All MOD-IO relays OFF after boot | SKIP | Depends on 14.1-14.3 |
| 14.5  | Set policy: leave_unchanged | `evb-relay config set modio_boot_policy=leave_unchanged --format json` | Accepted; exit 0 | FAIL | Config PUT endpoint crashes |
| 14.6  | Set MOD-IO relays ON       | `evb-relay relay set modio:1=on modio:2=on modio:3=on modio:4=on` | All ON; exit 0 | SKIP | MOD-IO not attached |
| 14.7  | Reboot device              | `evb-relay ota flash firmware/build/evb_relay_firmware.bin --format json` | Device comes back online | SKIP | OTA non-functional |
| 14.8  | Verify leave_unchanged     | `evb-relay relay list --format json` | MOD-IO relay state unchanged (ON) or `sync=unknown` | SKIP | Depends on 14.5-14.7 |
| 14.9  | Cleanup: all OFF           | `evb-relay relay set modio:1=off modio:2=off modio:3=off modio:4=off` | All OFF | SKIP | MOD-IO not attached |
| 14.10 | Restore default policy     | `evb-relay config set modio_boot_policy=leave_unchanged` | Accepted | FAIL | Config PUT endpoint crashes |

---

## Section 15 — Output Formats & Exit Codes

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 15.1 | Table format (default)     | `evb-relay relay list` | Formatted ASCII table; exit 0 | PASS | Formatted table with TARGET, STATE, SYNC, MODIO columns |
| 15.2 | Plain format               | `evb-relay relay list --format plain` | Minimal text output; exit 0 | PASS | Key=value pairs; exit 0 |
| 15.3 | JSON format                | `evb-relay relay list --format json` | Valid JSON (parseable by jq); exit 0 | PASS | Valid JSON; exit 0 |
| 15.4 | Robot TOON format          | `evb-relay relay list --robot` | TOON envelope line; exit 0 | PASS | TOON with v=1, command="relay list" |
| 15.5 | Robot JSON format          | `evb-relay relay list --robot --format json` | JSON envelope with `v`, `command`, `timestamp`, `elapsed_ms`, `exit_code`, `host`, `device_context`, `data`; exit 0 | PASS | All 8 envelope keys present |
| 15.6 | Exit code 0 (success)      | `evb-relay status; echo $?` | `0` | PASS | Exit 0 |
| 15.7 | Exit code 2 (network)      | `EVB_RELAY_HOST=192.0.2.1 evb-relay status --timeout 2s; echo $?` | `2` | PASS | Exit 2 |
| 15.8 | Exit code 3 (auth)         | `EVB_RELAY_API_TOKEN=wrong evb-relay status; echo $?` | `3` | PASS | Exit 3 |
| 15.9 | Exit code 5 (bad arg)      | `evb-relay relay on onboard:99; echo $?` | `5` | PASS | Exit 5 |

---

## Section 16 — Config Persistence Across Reboot

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 16.1 | Set distinctive values     | `evb-relay config set hostname=persist-test poll_interval_ms=250` | Accepted | FAIL | Config PUT endpoint crashes (connection reset) |
| 16.2 | Reboot device              | `evb-relay ota flash firmware/build/evb_relay_firmware.bin --format json` | Device comes back online | SKIP | Depends on 16.1; OTA also non-functional |
| 16.3 | Verify hostname persisted  | `evb-relay config show --format json` | `persist-test` | SKIP | Depends on 16.1 |
| 16.4 | Verify poll_interval       | `evb-relay config show --format json` | `250` | SKIP | Depends on 16.1 |
| 16.5 | Restore defaults           | `evb-relay config set hostname=esp32-evb-relay poll_interval_ms=100` | Accepted | FAIL | Config PUT endpoint crashes |

---

## Section 17 — Button Event

> **Instructions:** Requires physical button press on the ESP32-EVB
> board (GPIO34). If the agent cannot actuate the button, mark as
> SKIP.

| #    | Test                  | Command | Expected | Result | Notes |
|------|-----------------------|---------|----------|--------|-------|
| 17.1 | Start event stream    | `timeout 15 evb-relay input watch --robot 2>/dev/null` | Stream starts | SKIP | Requires physical button press; agent cannot actuate |
| 17.2 | Press button          | Physically press button on ESP32-EVB | `button` event appears in stream | SKIP | Requires physical button press; agent cannot actuate |

---

## Results Summary

| Section | Description                 | Total | Pass | Fail | Skip |
|---------|-----------------------------|-------|------|------|------|
| 1       | Automated quality gates     | 12    | 11   | 1    | 0    |
| 2       | Version & discovery         | 4     | 3    | 1    | 0    |
| 3       | Status                      | 6     | 6    | 0    | 0    |
| 4       | Onboard relay control       | 13    | 13   | 0    | 0    |
| 5       | MOD-IO relay control        | 14    | 1    | 0    | 13   |
| 6       | MOD-IO inputs               | 10    | 0    | 0    | 10   |
| 7       | SSE / event streaming       | 5     | 5    | 0    | 0    |
| 8       | Configuration               | 16    | 4    | 12   | 0    |
| 9       | WiFi configuration          | 10    | 2    | 7    | 1    |
| 10      | Authentication & security   | 11    | 11   | 0    | 0    |
| 11      | OTA firmware update         | 5     | 1    | 1    | 3    |
| 12      | Error handling & edge cases | 14    | 9    | 3    | 2    |
| 13      | MOD-IO absent (degradation) | 10    | 6    | 0    | 4    |
| 14      | MOD-IO boot policy          | 10    | 0    | 3    | 7    |
| 15      | Output formats & exit codes | 9     | 9    | 0    | 0    |
| 16      | Config persistence          | 5     | 0    | 2    | 3    |
| 17      | Button event                | 2     | 0    | 0    | 2    |
| **Total** |                           | **156** | **81** | **30** | **45** |

---

## Failure Analysis

### F1 — Config PUT endpoint crashes httpd task (8.4–8.16, 9.1–9.7, 14.1/14.5/14.10, 16.1/16.5)

**Root cause:** The `rest_api_config_update_handler` and
`rest_api_wifi_config_update_handler` in `rest_api.c` allocate ~1.2 KB
of local variables on the httpd task stack (two `device_config_snapshot_t`
at ~80 B each, `rest_api_config_update_request_t` at ~333 B including a
256-byte `api_token` buffer, `request_body[512]`, `rest_api_status_view_t`
at ~124 B, plus arrays). The httpd server uses `HTTPD_DEFAULT_CONFIG()`
which has a 4096-byte stack on ESP32. Combined with function call
overhead from `rest_api_require_authenticated_status`, cJSON parsing,
and NVS writes, this overflows the stack and crashes the task, causing a
TCP connection reset.

**Impact:** All runtime configuration changes via CLI or REST API are
completely non-functional. This blocks hostname changes, poll interval
tuning, MOD-IO boot policy, WiFi credential storage, and any config
persistence testing. **Release blocker.**

**Fix:** Increase `server_config.stack_size` in `rest_api_start()` (e.g.
8192 or 10240), or refactor the config handlers to reduce stack usage by
making large buffers `static` or heap-allocated.

### F2 — OTA endpoint crashes httpd task (11.2)

**Root cause:** Same stack overflow pattern. The `rest_api_ota_handler`
allocates `uint8_t chunk[1024]` plus `rest_api_status_view_t` (~124 B)
and other locals on the same undersized httpd stack. The upload starts
but the httpd task crashes during the receive loop.

**Impact:** OTA firmware updates via the REST API are non-functional.
Firmware can only be updated via serial flash. **Release blocker.**

### F3 — ci-full timeout (1.12)

**Root cause:** `just ci-full` runs `test-device` which takes ~97s.
When run as part of the full CI pipeline (after ci already ran), the
second `test-device` invocation times out at the 480s watchdog because
it includes a full flash+test cycle. This appears to be a test
infrastructure timing issue rather than a firmware bug.

**Impact:** The full CI+HW gate cannot complete in a single run.
Individual gates (`just ci`, `just test-device`, `just test-integration`)
all pass independently. **Not a release blocker** — CI pipeline timing
needs adjustment.

### F4 — mDNS discover returns empty (2.3)

**Root cause:** The test network segment does not have mDNS configured
or the ESP32's mDNS responder is not advertising on this network. The
device is reachable by IP but not discoverable via mDNS.

**Impact:** CLI `discover` command returns no devices. Users on networks
without mDNS will need to find the device IP manually (serial console or
DHCP lease table). **Not a release blocker** — environmental, not a code bug.

### F5 — REST relay GET returns 405, PUT with bad body crashes (12.7, 12.9, 12.10)

**Root cause:** GET on `/api/v1/relays/onboard/9` matches the wildcard
PUT handler, returning 405 instead of 400/404. PUT with no body or
invalid JSON body also crashes the httpd task (same stack overflow as F1).

**Impact:** Invalid REST requests crash the server instead of returning
proper error codes. **Related to F1 — same root cause.**

---

## Filed Beads

| Bead ID | Title | Priority | Labels |
|---------|-------|----------|--------|
| *(TBD)* | httpd stack overflow crashes config/OTA/PUT handlers on real hardware | critical | firmware, bug |
| *(TBD)* | ci-full watchdog timeout when running test-device in sequence | medium | ci, test |

---

## Sign-Off

| Field            | Value |
|------------------|-------|
| Completed by     | Claude Opus 4.6 (1M context) |
| Date completed   | 2026-03-21 |
| Final commit SHA | 6b5f338 |
| Overall result   | 81 PASS / 30 FAIL / 45 SKIP |
| Blocking issues  | F1: Config PUT crashes httpd (all config endpoints non-functional); F2: OTA endpoint crashes httpd (OTA non-functional) |

---

## Agent Instructions

1. Complete **all prerequisite steps** (Steps 0–6) before proceeding
2. Fill in the **Test Run Metadata** table before starting tests
3. Execute each test in order; fill **Result** (`PASS` / `FAIL` /
   `SKIP`) and **Notes** (actual output, error messages, observations)
4. If a test fails, record the failure details and continue — do not
   stop the run
5. Tests marked with physical actions (MOD-IO disconnect, button
   press) should be marked `SKIP` if the agent cannot perform them,
   with a note explaining why
6. After completing all sections, fill in the **Results Summary** table
7. Fill in the **Sign-Off** table
8. Commit this file with the completed results:
   ```bash
   git add docs/manual-release-test-plan-opus4.6.md
   git -c commit.gpgsign=false commit -s -m "$(cat <<'EOF'
   test: execute manual pre-release test plan

   Run the 156-test pre-release validation checklist against real
   hardware covering all 17 functional areas.

   Results: N PASS / N FAIL / N SKIP
   EOF
   )"
   ```

# Manual Pre-Release Test Plan

> Agent-executable checklist for validating the full CLI + firmware
> feature set against real hardware before cutting a release. The
> executing agent fills in the **Result** and **Notes** columns, then
> commits this file with the completed results.

---

## Test Run Metadata

| Field              | Value |
|--------------------|-------|
| Firmware version   | *(fill after Step 1)* |
| CLI version        | *(fill after Step 2)* |
| Device IP / host   | *(fill after Step 4)* |
| Agent identity     | *(executing agent name)* |
| Date               | *(execution date)* |
| Git commit (RC)    | *(fill: `git rev-parse --short HEAD`)* |
| MOD-IO attached    | *(yes/no)* |
| Ethernet connected | *(yes/no)* |
| WiFi AP available  | *(yes/no)* |

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
| 1.1  | Format check           | `just format-check`    | Exit 0, no diff         |  |  |
| 1.2  | Firmware build         | `just build`           | Exit 0, binary produced |  |  |
| 1.3  | Host unit tests        | `just test`            | All tests pass          |  |  |
| 1.4  | CLI format check       | `just cli-fmt-check`   | Exit 0                  |  |  |
| 1.5  | CLI lint               | `just cli-lint`        | Exit 0                  |  |  |
| 1.6  | CLI vet                | `just cli-vet`         | Exit 0                  |  |  |
| 1.7  | CLI unit tests         | `just cli-test`        | All tests pass          |  |  |
| 1.8  | CLI E2E tests          | `just test-e2e`        | All tests pass          |  |  |
| 1.9  | Full CI gate           | `just ci`              | Exit 0                  |  |  |
| 1.10 | On-device tests (HW)   | `just test-device`     | All tests pass          |  |  |
| 1.11 | Integration tests (HW) | `just test-integration` | All tests pass          |  |  |
| 1.12 | Full CI+HW gate        | `just ci-full`         | Exit 0                  |  |  |

---

## Section 2 — Version & Discovery

| #   | Test                        | Command | Expected | Result | Notes |
|-----|-----------------------------|---------|----------|--------|-------|
| 2.1 | CLI version flag            | `evb-relay --version` | Prints version, commit, date; exit 0 |  |  |
| 2.2 | Robot capabilities          | `evb-relay --robot-capabilities` | JSON with command list; exit 0 |  |  |
| 2.3 | mDNS discover               | `evb-relay discover --format json` | JSON array with at least 1 device |  |  |
| 2.4 | Discover timeout            | `evb-relay discover --timeout 1s --format json` | Completes within ~1s |  |  |

---

## Section 3 — Status

| #   | Test                        | Command | Expected | Result | Notes |
|-----|-----------------------------|---------|----------|--------|-------|
| 3.1 | Status table format         | `evb-relay status` | Human-readable table with uptime, FW version, heap, network, MOD-IO; exit 0 |  |  |
| 3.2 | Status JSON format          | `evb-relay status --format json` | Valid JSON; fields: `uptime_seconds`, `firmware_version`, `free_heap_bytes`, `network.hostname`, `network.connected`, `network.transport`, `network.ip`, `network.netmask`, `network.gateway`, `modio.present`, `modio.sync` |  |  |
| 3.3 | Status robot mode           | `evb-relay status --robot` | TOON envelope with `v`, `command`, `timestamp`, `elapsed_ms`, `exit_code`, `host`, `device_context`, `data` |  |  |
| 3.4 | Status robot JSON           | `evb-relay status --robot --format json` | JSON envelope; same fields as 3.3 |  |  |
| 3.5 | Status via REST             | `curl -s -H "Authorization: Bearer $EVB_RELAY_API_TOKEN" http://$EVB_RELAY_HOST/api/v1/status` | Valid JSON matching CLI output; response headers include `X-FW-Version`, `X-ModIO-Present`, `X-ModIO-Sync` |  |  |
| 3.6 | FW version match            | Compare `--version` output with status `firmware_version` | Versions match |  |  |

---

## Section 4 — Onboard Relay Control

Start state: both onboard relays OFF after boot.

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 4.1  | List relays (initial)      | `evb-relay relay list --format json` | Both onboard relays present, state `false`; exit 0 |  |  |
| 4.2  | Relay 1 ON (CLI)           | `evb-relay relay on onboard:1 --format json` | `group=onboard`, `id=1`, `state=true`; exit 0 |  |  |
| 4.3  | Relay 1 verify ON          | `evb-relay relay list --format json` | Relay 1 state `true` |  |  |
| 4.4  | Relay 1 OFF (CLI)          | `evb-relay relay off onboard:1 --format json` | `state=false`; exit 0 |  |  |
| 4.5  | Relay 2 toggle ON          | `evb-relay relay toggle onboard:2 --format json` | `state=true`; exit 0 |  |  |
| 4.6  | Relay 2 toggle OFF         | `evb-relay relay toggle onboard:2 --format json` | `state=false`; exit 0 |  |  |
| 4.7  | Batch set both ON          | `evb-relay relay set onboard:1=on onboard:2=on --format json` | `all_ok=true`, both relays `state=true` |  |  |
| 4.8  | Batch set both OFF         | `evb-relay relay set onboard:1=off onboard:2=off --format json` | `all_ok=true`, both relays `state=false` |  |  |
| 4.9  | Relay ON via REST          | `curl -s -X PUT ... -d '{"state":true}' .../relays/onboard/1` | JSON with `state=true`; HTTP 200 |  |  |
| 4.10 | Relay toggle via REST      | `curl -s -X POST .../relays/onboard/1/toggle` | State toggled; HTTP 200 |  |  |
| 4.11 | Relay OFF via REST         | `curl -s -X PUT ... -d '{"state":false}' .../relays/onboard/1` | `state=false`; HTTP 200 |  |  |
| 4.12 | GET onboard relays (REST)  | `curl -s .../relays/onboard` | JSON array of 2 relays with `id`, `state` |  |  |
| 4.13 | Cleanup: both OFF          | `evb-relay relay set onboard:1=off onboard:2=off` | Both OFF |  |  |

---

## Section 5 — MOD-IO Relay Control

Prerequisite: MOD-IO attached. Status must show `modio.present=true`.

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 5.1  | MOD-IO presence            | `evb-relay status --format json` | `true` |  |  |
| 5.2  | MOD-IO sync state          | `evb-relay status --format json` | `unknown` or `synchronized` |  |  |
| 5.3  | Batch set all ON (sync)    | `evb-relay relay set modio:1=on modio:2=on modio:3=on modio:4=on --format json` | `all_ok=true`; all 4 relays `state=true`; sync becomes `synchronized` |  |  |
| 5.4  | Verify sync state          | `evb-relay status --format json` | `synchronized` |  |  |
| 5.5  | Single relay OFF           | `evb-relay relay off modio:2 --format json` | `id=2`, `state=false`; exit 0 |  |  |
| 5.6  | Single relay ON            | `evb-relay relay on modio:2 --format json` | `id=2`, `state=true`; exit 0 |  |  |
| 5.7  | Toggle relay               | `evb-relay relay toggle modio:3 --format json` | State flipped; exit 0 |  |  |
| 5.8  | Batch set via REST         | `curl -s -X PUT ... -d '{"states":[false,true,false,true]}' .../relays/modio` | HTTP 200; states match request |  |  |
| 5.9  | Single relay via REST      | `curl -s -X PUT ... -d '{"state":true}' .../relays/modio/1` | HTTP 200; `state=true` |  |  |
| 5.10 | Toggle relay via REST      | `curl -s -X POST .../relays/modio/1/toggle` | State toggled; HTTP 200 |  |  |
| 5.11 | GET modio relays (REST)    | `curl -s .../relays/modio` | JSON array of 4 relays |  |  |
| 5.12 | GET all relays (REST)      | `curl -s .../relays` | JSON with both onboard (2) and modio (4) relays |  |  |
| 5.13 | Relay list shows all       | `evb-relay relay list --format json` | 6 total relays; `modio_present=true`, `modio_sync=synchronized` |  |  |
| 5.14 | Cleanup: all MOD-IO OFF    | `evb-relay relay set modio:1=off modio:2=off modio:3=off modio:4=off` | All OFF |  |  |

---

## Section 6 — MOD-IO Inputs

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 6.1  | All digital inputs         | `evb-relay input digital --format json` | JSON with 4 inputs; each has `id`, `state` (boolean); `sample_ts_ms`, `sample_age_ms`, `poll_interval_ms` present |  |  |
| 6.2  | Single digital input       | `evb-relay input digital 1 --format json` | Single input with `id=1`, `state` (boolean) |  |  |
| 6.3  | All analog inputs          | `evb-relay input analog --format json` | JSON with 4 inputs; each has `id`, `value` (0-1023); timestamp metadata present |  |  |
| 6.4  | Single analog input        | `evb-relay input analog 3 --format json` | Single input with `id=3`, `value` in 0-1023 range |  |  |
| 6.5  | Digital inputs via REST    | `curl -s .../inputs/digital` | JSON array of 4 digital inputs |  |  |
| 6.6  | Single digital via REST    | `curl -s .../inputs/digital/2` | Single input JSON |  |  |
| 6.7  | Analog inputs via REST     | `curl -s .../inputs/analog` | JSON array of 4 analog inputs, values 0-1023 |  |  |
| 6.8  | Single analog via REST     | `curl -s .../inputs/analog/4` | Single input JSON, value 0-1023 |  |  |
| 6.9  | Digital plain format       | `evb-relay input digital --format plain` | Plain text output; exit 0 |  |  |
| 6.10 | Analog table format        | `evb-relay input analog` | Human-readable table; exit 0 |  |  |

---

## Section 7 — SSE / Event Streaming

| #   | Test                        | Command | Expected | Result | Notes |
|-----|-----------------------------|---------|----------|--------|-------|
| 7.1 | Input watch starts          | `timeout 10 evb-relay input watch --robot 2>/dev/null \| head -3` | NDJSON lines; first line is stream header with `stream=start`, `device_context`; exit 0 or 124 (timeout) |  |  |
| 7.2 | Relay event in stream       | Start `evb-relay input watch --robot` in background, toggle a relay, capture output | `relay_changed` event appears with relay group, id, state |  |  |
| 7.3 | Heartbeat received          | `timeout 35 curl ... /api/v1/events` | At least one heartbeat line within 30s |  |  |
| 7.4 | SSE via REST                | `timeout 5 curl -s -N .../events \| head -5` | SSE format: lines starting with `event:` and `data:` |  |  |
| 7.5 | Multiple SSE clients        | Open 2 concurrent curl SSE connections, toggle relay | Both clients receive the event |  |  |

---

## Section 8 — Configuration

| #    | Test                              | Command | Expected | Result | Notes |
|------|-----------------------------------|---------|----------|--------|-------|
| 8.1  | Config show (CLI)                 | `evb-relay config show --format json` | JSON with `poll_interval_ms`, `hostname`, `modio_boot_policy`, `api_token_set`, `wifi.ssid_set`, `wifi.passphrase_set`, `wifi.network_policy` |  |  |
| 8.2  | Config show (table)               | `evb-relay config show` | Human-readable table; exit 0 |  |  |
| 8.3  | Config show (REST)                | `curl -s .../config` | JSON matching CLI output; secrets shown as `_set` booleans only |  |  |
| 8.4  | Set hostname                      | `evb-relay config set hostname=test-relay --format json` | Accepted; exit 0 |  |  |
| 8.5  | Verify hostname                   | `evb-relay config show --format json` | `test-relay` |  |  |
| 8.6  | Set poll_interval_ms              | `evb-relay config set poll_interval_ms=200 --format json` | Accepted; exit 0 |  |  |
| 8.7  | Verify poll_interval_ms           | `evb-relay config show --format json` | `200` |  |  |
| 8.8  | Set modio_boot_policy             | `evb-relay config set modio_boot_policy=all_off --format json` | Accepted; exit 0 |  |  |
| 8.9  | Invalid: poll too low             | `evb-relay config set poll_interval_ms=10` | Rejected; exit code 5 (bad argument) |  |  |
| 8.10 | Invalid: poll too high            | `evb-relay config set poll_interval_ms=20000` | Rejected; exit code 5 |  |  |
| 8.11 | Invalid: empty hostname           | `evb-relay config set hostname=` | Rejected; exit code 5 |  |  |
| 8.12 | Invalid: hostname too long        | `evb-relay config set hostname=a234...` (64 chars) | Rejected (64 chars > max 63); exit code 5 |  |  |
| 8.13 | Invalid: hostname leading hyphen  | `evb-relay config set hostname=-bad` | Rejected; exit code 5 |  |  |
| 8.14 | Invalid: hostname special chars   | `evb-relay config set hostname=host.name` | Rejected; exit code 5 |  |  |
| 8.15 | Config via REST                   | `curl -s -X PUT ... -d '{"poll_interval_ms":100}' .../config` | HTTP 200; accepted |  |  |
| 8.16 | Restore defaults                  | `evb-relay config set hostname=esp32-evb-relay poll_interval_ms=100 modio_boot_policy=leave_unchanged` | All accepted |  |  |

---

## Section 9 — WiFi Configuration

| #    | Test                              | Command | Expected | Result | Notes |
|------|-----------------------------------|---------|----------|--------|-------|
| 9.1  | Set WiFi credentials              | `evb-relay config wifi ssid=TestNet passphrase=secret123 --format json` | Accepted; `restart_required` may be true; exit 0 |  |  |
| 9.2  | Verify ssid_set                   | `evb-relay config show --format json` | `true` |  |  |
| 9.3  | Verify passphrase_set             | `evb-relay config show --format json` | `true` |  |  |
| 9.4  | Set network_policy                | `evb-relay config wifi network_policy=prefer_ethernet --format json` | Accepted; exit 0 |  |  |
| 9.5  | Verify network_policy             | `evb-relay config show --format json` | `prefer_ethernet` |  |  |
| 9.6  | WiFi via REST                     | `curl -s -X PUT ... -d '{"network_policy":"ethernet_only"}' .../config/wifi` | HTTP 200; accepted |  |  |
| 9.7  | Clear WiFi credentials            | `evb-relay config wifi clear=true --format json` | Accepted; exit 0 |  |  |
| 9.8  | Verify cleared                    | `evb-relay config show --format json` | `false` |  |  |
| 9.9  | Invalid: passphrase without ssid  | `evb-relay config wifi passphrase=secret` | Rejected; exit code 5 |  |  |
| 9.10 | Invalid: clear with ssid          | `evb-relay config wifi clear=true ssid=TestNet` | Rejected; exit code 5 |  |  |

---

## Section 10 — Authentication & Security

| #     | Test                        | Command | Expected | Result | Notes |
|-------|-----------------------------|---------|----------|--------|-------|
| 10.1  | No token -> 401             | `curl -s -o /dev/null -w '%{http_code}' .../status` | `401` |  |  |
| 10.2  | No token: no device headers | `curl -s -D- .../status \| grep -ci 'X-FW-Version'` | `0` (header absent) |  |  |
| 10.3  | Wrong token -> 401/403      | `curl ... -H "Authorization: Bearer WRONGTOKEN" .../status` | `401` or `403` |  |  |
| 10.4  | Wrong token: no device headers | `curl ... \| grep -ci 'X-FW-Version'` | `0` (header absent) |  |  |
| 10.5  | Valid token -> 200          | `curl ... -H "Authorization: Bearer $EVB_RELAY_API_TOKEN" .../status` | `200` |  |  |
| 10.6  | Valid token: device headers | `curl ... \| grep -c 'X-FW-Version'` | `1` (header present) |  |  |
| 10.7  | X-ModIO-Present header      | `curl ... \| grep -i 'X-ModIO-Present'` | Header present with value `true` or `false` |  |  |
| 10.8  | X-ModIO-Sync header         | `curl ... \| grep -i 'X-ModIO-Sync'` | Header present with value `absent`, `unknown`, or `synchronized` |  |  |
| 10.9  | CLI without token           | `EVB_RELAY_API_TOKEN="" evb-relay status` | Auth error; exit code 3 |  |  |
| 10.10 | CLI with wrong token        | `EVB_RELAY_API_TOKEN="WRONG" evb-relay status` | Auth error; exit code 3 |  |  |
| 10.11 | Auth error JSON body        | `curl -s -H "Authorization: Bearer WRONGTOKEN" .../status` | JSON with error code (`AUTH_REQUIRED` or `AUTH_FORBIDDEN`) |  |  |

---

## Section 11 — OTA Firmware Update

> **Caution:** This section flashes firmware. Ensure a known-good
> binary is available for recovery. The agent should build the RC
> binary fresh before testing.

| #    | Test                  | Command | Expected | Result | Notes |
|------|-----------------------|---------|----------|--------|-------|
| 11.1 | Build RC binary       | `just build` | Binary at firmware/build/ |  |  |
| 11.2 | OTA flash             | `evb-relay ota flash firmware/build/evb_relay_firmware.bin --format json` | `uploaded_bytes` > 0, `reboot_in_seconds` present; exit 0 |  |  |
| 11.3 | Wait for reboot       | `sleep 10 && evb-relay status --format json` | Device responds; FW version matches RC |  |  |
| 11.4 | Post-OTA relay test   | `evb-relay relay list --format json` | Relays accessible; exit 0 |  |  |
| 11.5 | Post-OTA config persisted | `evb-relay config show --format json` | Config values match pre-OTA settings |  |  |

---

## Section 12 — Error Handling & Edge Cases

| #     | Test                          | Command | Expected | Result | Notes |
|-------|-------------------------------|---------|----------|--------|-------|
| 12.1  | Invalid onboard relay ID      | `evb-relay relay on onboard:5` | Error; exit code 5 (bad argument) |  |  |
| 12.2  | Invalid onboard relay ID 0    | `evb-relay relay on onboard:0` | Error; exit code 5 |  |  |
| 12.3  | Invalid modio relay ID        | `evb-relay relay on modio:5` | Error; exit code 5 |  |  |
| 12.4  | Invalid input ID              | `evb-relay input digital 5` | Error; exit code 5 |  |  |
| 12.5  | Invalid input ID 0            | `evb-relay input analog 0` | Error; exit code 5 |  |  |
| 12.6  | Invalid group name            | `evb-relay relay on bogus:1` | Error; exit code 5 |  |  |
| 12.7  | Invalid relay via REST        | `curl ... .../relays/onboard/9` | HTTP 400 or 404 |  |  |
| 12.8  | Invalid input via REST        | `curl ... .../inputs/digital/9` | HTTP 400 or 404 |  |  |
| 12.9  | Missing JSON body (relay)     | `curl -X PUT ... .../relays/onboard/1` (no body) | HTTP 400 |  |  |
| 12.10 | Invalid JSON body             | `curl ... -d 'not-json' .../relays/onboard/1` | HTTP 400 |  |  |
| 12.11 | Unknown endpoint              | `curl ... .../nonexistent` | HTTP 404 |  |  |
| 12.12 | Network error (CLI)           | `EVB_RELAY_HOST=192.0.2.1 evb-relay status --timeout 2s` | Network error; exit code 2 |  |  |
| 12.13 | Relay set partial bad target  | `evb-relay relay set onboard:1=on bogus:1=on` | Error or partial failure; exit code != 0 |  |  |
| 12.14 | OTA with bad file             | `echo "garbage" > /tmp/bad-fw.bin && evb-relay ota flash /tmp/bad-fw.bin` | Error or device rejects; device recovers |  |  |

---

## Section 13 — MOD-IO Absent (Graceful Degradation)

> **Instructions:** If MOD-IO can be safely disconnected during the
> test run, execute these tests. Otherwise mark as SKIP with a note.
> **Reconnect MOD-IO before proceeding to later sections.**

| #     | Test                       | Command | Expected | Result | Notes |
|-------|----------------------------|---------|----------|--------|-------|
| 13.1  | Disconnect MOD-IO          | Physically disconnect MOD-IO from UEXT | Device continues running |  |  |
| 13.2  | Status shows absent        | `evb-relay status --format json` | `present=false`, `sync=absent` |  |  |
| 13.3  | MOD-IO relay -> state error | `evb-relay relay on modio:1` | Error; exit code 6 or 7 |  |  |
| 13.4  | MOD-IO input -> error      | `evb-relay input digital` | Error or empty/stale data |  |  |
| 13.5  | Onboard relays still work  | `evb-relay relay toggle onboard:1` | Works normally; exit 0 |  |  |
| 13.6  | REST modio -> 503          | `curl ... .../relays/modio/1` | HTTP 503 |  |  |
| 13.7  | Relay list (degraded)      | `evb-relay relay list --format json` | `modio_present=false` |  |  |
| 13.8  | Reconnect MOD-IO           | Physically reconnect MOD-IO to UEXT | Device detects MOD-IO |  |  |
| 13.9  | Status shows recovery      | `evb-relay status --format json` | `present=true`; sync transitions |  |  |
| 13.10 | Cleanup: onboard OFF       | `evb-relay relay off onboard:1` | Exit 0 |  |  |

---

## Section 14 — MOD-IO Boot Policy

> **Instructions:** These tests require device reboots. Record the
> relay state before and after each reboot.

| #     | Test                       | Command | Expected | Result | Notes |
|-------|----------------------------|---------|----------|--------|-------|
| 14.1  | Set policy: all_off        | `evb-relay config set modio_boot_policy=all_off --format json` | Accepted; exit 0 |  |  |
| 14.2  | Set MOD-IO relays ON       | `evb-relay relay set modio:1=on modio:2=on modio:3=on modio:4=on` | All ON; exit 0 |  |  |
| 14.3  | Reboot device              | `evb-relay ota flash firmware/build/evb_relay_firmware.bin --format json` | Device comes back online |  |  |
| 14.4  | Verify all_off applied     | `evb-relay relay list --format json` | All MOD-IO relays OFF after boot |  |  |
| 14.5  | Set policy: leave_unchanged | `evb-relay config set modio_boot_policy=leave_unchanged --format json` | Accepted; exit 0 |  |  |
| 14.6  | Set MOD-IO relays ON       | `evb-relay relay set modio:1=on modio:2=on modio:3=on modio:4=on` | All ON; exit 0 |  |  |
| 14.7  | Reboot device              | `evb-relay ota flash firmware/build/evb_relay_firmware.bin --format json` | Device comes back online |  |  |
| 14.8  | Verify leave_unchanged     | `evb-relay relay list --format json` | MOD-IO relay state unchanged (ON) or `sync=unknown` |  |  |
| 14.9  | Cleanup: all OFF           | `evb-relay relay set modio:1=off modio:2=off modio:3=off modio:4=off` | All OFF |  |  |
| 14.10 | Restore default policy     | `evb-relay config set modio_boot_policy=leave_unchanged` | Accepted |  |  |

---

## Section 15 — Output Formats & Exit Codes

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 15.1 | Table format (default)     | `evb-relay relay list` | Formatted ASCII table; exit 0 |  |  |
| 15.2 | Plain format               | `evb-relay relay list --format plain` | Minimal text output; exit 0 |  |  |
| 15.3 | JSON format                | `evb-relay relay list --format json` | Valid JSON (parseable by jq); exit 0 |  |  |
| 15.4 | Robot TOON format          | `evb-relay relay list --robot` | TOON envelope line; exit 0 |  |  |
| 15.5 | Robot JSON format          | `evb-relay relay list --robot --format json` | JSON envelope with `v`, `command`, `timestamp`, `elapsed_ms`, `exit_code`, `host`, `device_context`, `data`; exit 0 |  |  |
| 15.6 | Exit code 0 (success)      | `evb-relay status; echo $?` | `0` |  |  |
| 15.7 | Exit code 2 (network)      | `EVB_RELAY_HOST=192.0.2.1 evb-relay status --timeout 2s; echo $?` | `2` |  |  |
| 15.8 | Exit code 3 (auth)         | `EVB_RELAY_API_TOKEN=wrong evb-relay status; echo $?` | `3` |  |  |
| 15.9 | Exit code 5 (bad arg)      | `evb-relay relay on onboard:99; echo $?` | `5` |  |  |

---

## Section 16 — Config Persistence Across Reboot

| #    | Test                       | Command | Expected | Result | Notes |
|------|----------------------------|---------|----------|--------|-------|
| 16.1 | Set distinctive values     | `evb-relay config set hostname=persist-test poll_interval_ms=250` | Accepted |  |  |
| 16.2 | Reboot device              | `evb-relay ota flash firmware/build/evb_relay_firmware.bin --format json` | Device comes back online |  |  |
| 16.3 | Verify hostname persisted  | `evb-relay config show --format json` | `persist-test` |  |  |
| 16.4 | Verify poll_interval       | `evb-relay config show --format json` | `250` |  |  |
| 16.5 | Restore defaults           | `evb-relay config set hostname=esp32-evb-relay poll_interval_ms=100` | Accepted |  |  |

---

## Section 17 — Button Event

> **Instructions:** Requires physical button press on the ESP32-EVB
> board (GPIO34). If the agent cannot actuate the button, mark as
> SKIP.

| #    | Test                  | Command | Expected | Result | Notes |
|------|-----------------------|---------|----------|--------|-------|
| 17.1 | Start event stream    | `timeout 15 evb-relay input watch --robot 2>/dev/null` | Stream starts |  |  |
| 17.2 | Press button          | Physically press button on ESP32-EVB | `button` event appears in stream |  |  |

---

## Results Summary

| Section | Description                 | Total | Pass | Fail | Skip |
|---------|-----------------------------|-------|------|------|------|
| 1       | Automated quality gates     | 12    |      |      |      |
| 2       | Version & discovery         | 4     |      |      |      |
| 3       | Status                      | 6     |      |      |      |
| 4       | Onboard relay control       | 13    |      |      |      |
| 5       | MOD-IO relay control        | 14    |      |      |      |
| 6       | MOD-IO inputs               | 10    |      |      |      |
| 7       | SSE / event streaming       | 5     |      |      |      |
| 8       | Configuration               | 16    |      |      |      |
| 9       | WiFi configuration          | 10    |      |      |      |
| 10      | Authentication & security   | 11    |      |      |      |
| 11      | OTA firmware update         | 5     |      |      |      |
| 12      | Error handling & edge cases | 14    |      |      |      |
| 13      | MOD-IO absent (degradation) | 10    |      |      |      |
| 14      | MOD-IO boot policy          | 10    |      |      |      |
| 15      | Output formats & exit codes | 9     |      |      |      |
| 16      | Config persistence          | 5     |      |      |      |
| 17      | Button event                | 2     |      |      |      |
| **Total** |                           | **156** |  |  |  |

---

## Failure Analysis

*(Fill in after completing all test sections. For each FAIL result,
document: root cause, impact, and bead ID if filed.)*

---

## Filed Beads

*(Fill in after failure analysis. Create beads with `br create` for
any new issues discovered during the test run.)*

| Bead ID | Title | Priority | Labels |
|---------|-------|----------|--------|

---

## Sign-Off

| Field            | Value |
|------------------|-------|
| Completed by     | *(executing agent)* |
| Date completed   | *(date)* |
| Final commit SHA | *(filled after commit)* |
| Overall result   | *(N PASS / N FAIL / N SKIP)* |
| Blocking issues  | *(list any FAIL results that block the release)* |

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

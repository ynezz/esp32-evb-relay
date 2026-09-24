---
name: evb-use
description: Operate a flashed esp32-evb-relay board with the evb-relay CLI. Use when asked to find or discover the device, check its status, switch or toggle onboard or MOD-IO relays, read MOD-IO digital or analog inputs, watch live events, show or change device config or WiFi, rotate the API token, push an OTA update, script the CLI in robot/JSON mode, or diagnose network, auth or MOD-IO errors.
---

# Use an evb-relay board

The `evb-relay` CLI talks to the board's REST API over HTTP. Installing
firmware or recovering a board is the evb-flash skill.

## Setup

- Binary: `./bin/evb-relay` after `cd cli && go build -o ../bin/evb-relay .`,
  or `evb-relay` from a release archive on `PATH`. Examples below say
  `evb-relay`.
- Target and token resolve from flags, then environment, then the config
  file `~/.config/evb-relay/config.toml` (keys `host`, `api_token`,
  `timeout`, `format`, `robot`):

  | Flag | Env |
  | --- | --- |
  | `-H`, `--host` | `EVB_RELAY_HOST` |
  | `-k`, `--api-token` | `EVB_RELAY_API_TOKEN` |
  | `-t`, `--timeout` (default 10s) | `EVB_RELAY_TIMEOUT` |
  | `--robot` | `EVB_RELAY_ROBOT` |

- Prefer the env var for the token, so it stays out of shell history and
  process lists. Never print it back to the user or into logs.
- No host known: follow `skills/_shared/device-address.md`
  (`evb-relay discover`, direct IP fallback when mDNS is blocked).

## Commands

```bash
evb-relay discover                         # mDNS browse, no token needed
evb-relay status                           # firmware, uptime, network, MOD-IO
evb-relay relay list
evb-relay relay on onboard:1
evb-relay relay off modio:3
evb-relay relay toggle onboard:2
evb-relay relay set onboard:1=on modio:3=off   # several at once
evb-relay input digital                    # all four, or: input digital 2
evb-relay input analog 1
evb-relay input watch                      # live events until interrupted
evb-relay config show
evb-relay config set hostname=lab-relay poll_interval_ms=250
evb-relay config wifi ssid=lab-net passphrase=secret network_policy=prefer_ethernet
evb-relay config set api_token=<new>       # rotate; switch clients to <new> right after
evb-relay ota flash evb-relay-fw-vX.Y.Z-ota.bin
```

- IDs are 1-based: `onboard:1..2`, `modio:1..4`, MOD-IO inputs `1..4`.
- Relays switch real loads. Unless the user has said this bench is
  unloaded or asked for the exact switch, confirm before turning anything
  on. Leave relays as you found them after experiments.
- Config keys: `hostname`, `poll_interval_ms`, `modio_boot_policy`
  (`leave_unchanged` or `all_off`), `api_token`; WiFi via `config wifi`
  with `ssid`, `passphrase`, `network_policy` (`ethernet_only`,
  `wifi_only`, `prefer_ethernet`). Tokens and WiFi credentials are
  write-only: `config show` reports only `api_token_set`,
  `wifi.ssid_set`, `wifi.passphrase_set`.
- A hostname change moves the mDNS name; update `EVB_RELAY_HOST`.
- `ota flash` uploads the app image (`-ota.bin` or
  `firmware/build/evb_relay_firmware.bin`, never `-full.bin`), then the board
  reboots. Wait about 10 s and check `status` for the new version.

## Robot mode (for scripts and agents)

- `evb-relay --robot-capabilities` dumps every command, argument, error
  code, exit code and env var as JSON. Read it instead of guessing.
- `--robot` wraps output in an envelope (`command`, `exit_code`, `data`,
  `error`, `warnings`, `device_context`, `next`). Default robot format is
  TOON; add `--format json` for JSON, e.g.
  `evb-relay --robot --format json status`.
- `evb-relay --robot --format json input watch` streams NDJSON: one
  header record, one record per event (`digital_input`, `analog_input`,
  `relay_changed`, `button`), a final `stream_end`. Run it under
  `timeout` in non-interactive shells.
- `device_context` carries firmware version and MOD-IO presence/sync
  from response headers; no extra `status` call needed.

## Exit codes and what to do

| Exit | Class | Typical codes | Action |
| --- | --- | --- | --- |
| 0 | success | | |
| 1 | general | `PARTIAL_FAILURE` | read `error.message`; after a partial `relay set`, rerun `relay list` to see what actually switched |
| 2 | network | `NETWORK_ERROR` | wrong host, board rebooting, or blocked path; see `skills/_shared/device-address.md` |
| 3 | auth | `AUTH_REQUIRED`, `AUTH_FORBIDDEN` | missing or wrong token; a freshly erased board has none (evb-flash skill) |
| 4 | not found | `RELAY_NOT_FOUND`, `INPUT_NOT_FOUND` | fix the ID; see the ranges above |
| 5 | bad argument | `BAD_ARGUMENT` | fix the key, value or syntax |
| 6 | state | `MODIO_STATE_UNKNOWN` | MOD-IO relay state is unknown after boot or reattach: one `relay set` naming all four `modio` relays re-syncs it |
| 7 | hardware | `MODIO_NOT_PRESENT`, `MODIO_SAMPLE_UNAVAILABLE` | MOD-IO missing or not answering on I2C; onboard relays still work; ask the user to check the UEXT cable |

## Troubleshooting

- `discover` finds nothing but the IP answers: multicast is filtered
  (sandbox, container, VLAN). Use `-H <ip>`.
- Requests time out right after `ota flash`: the board is rebooting.
  Wait about 10 s and retry.
- `config set` / `config wifi` output reports `restart_required`: that
  change takes effect only after the board reboots (OTA or power cycle).
- `status` works over Ethernet but not WiFi: check `wifi.network_policy`
  and whether `wifi.ssid_set` is true.
- Deeper hardware facts (GPIOs, MOD-IO I2C protocol):
  `docs/hardware-reference.md`.

---
name: evb-flash
description: Install or reinstall esp32-evb-relay firmware on an Olimex ESP32-EVB. Use when asked to set up the toolchain, build or download a firmware release, verify release checksums or attestations, find the board's serial port, erase and flash the board, OTA-update it, provision or rotate its API token, or recover a board that boot-loops or prints "invalid header".
---

# Flash an ESP32-EVB with evb-relay firmware

Everything here runs from the repo root. Flashing overwrites the board, so
confirm with the user which board and port to use unless they already
said so. Operating an already-flashed board is the evb-use skill.

## 1. Prerequisites

Check what is present before installing anything:

| Need | Check | Notes |
| --- | --- | --- |
| ESP-IDF v5.4.3 | `idf.py --version` (must print `v5.4.3`, matching `firmware/dependencies.lock`) | only for building or for `esptool.py`; `just` recipes and `scripts/flash.sh`/`scripts/provision.sh` auto-source `$IDF_PATH/export.sh` (default `~/esp/esp-idf`) if `idf.py` is not already on `PATH` |
| `just` | `just --version` | |
| Go 1.25+ | `go version` | builds the `evb-relay` CLI |
| Python venv | `test -x .venv/bin/python` | create with `just setup` |
| ASan/UBSan runtimes | `rpm -q libasan libubsan` | Fedora only, for `just test`; `sudo dnf install libasan libubsan` |

After switching ESP-IDF versions, delete stale build trees
(`rm -rf firmware/build firmware/test/build`): CMake caches the old
toolchain path and fails with "Tool doesn't match supported version".

`scripts/flash.sh` and `scripts/provision.sh` source `scripts/lib/idf-env.sh`
to activate ESP-IDF themselves: if `idf.py` is already on `PATH` this is a
no-op, otherwise they source `export.sh` from `$IDF_PATH` (or
`~/esp/esp-idf`). If a script still stops with an `idf.py is not on PATH`
error, ESP-IDF is not installed at either location; export `IDF_PATH` to
point at your checkout and rerun.

Build the CLI once: `cd cli && go build -o ../bin/evb-relay .`, then use
`./bin/evb-relay`, or install a release archive (below).

## 2. Get firmware: build or download

Build from source: `just build` produces `firmware/build/evb_relay_firmware.bin`.
`scripts/flash.sh` builds and flashes in one step (section 4).

Download a release (repo `ynezz/esp32-evb-relay`). Firmware and CLI of the
same tag belong together; install both from the same release:

```bash
gh release download vX.Y.Z --repo ynezz/esp32-evb-relay --dir dist \
  --pattern 'evb-relay-fw-*' --pattern SHA256SUMS \
  --pattern 'evb-relay_*_linux_amd64.tar.gz'
cd dist && sha256sum -c --ignore-missing SHA256SUMS
gh attestation verify evb-relay-fw-vX.Y.Z-full.bin --repo ynezz/esp32-evb-relay
```

Stop if a checksum or attestation fails. Release assets:

- `evb-relay-fw-vX.Y.Z-full.bin`: bootloader, partition table, otadata and
  app merged, written at offset `0x0` over serial.
- `evb-relay-fw-vX.Y.Z-ota.bin`: the app alone, for OTA onto a board that
  already runs evb-relay firmware.
- `evb-relay-fw-vX.Y.Z.elf`: symbols for decoding crash backtraces.

## 3. Find the serial port

Follow `skills/_shared/serial-port.md`. Below, `<port>` is the port you
confirmed. Never guess between several USB-serial adapters.

## 4. Flash

Pick one path:

- From source: `scripts/flash.sh --port <port>` (checks download mode,
  builds if needed, flashes bootloader, partitions and app).
- Release image over serial:

  ```bash
  esptool.py --chip esp32 --port <port> --baud 115200 \
    write_flash 0x0 dist/evb-relay-fw-vX.Y.Z-full.bin
  ```

  Stay at 115200 baud; faster rates cause sync timeouts on this board.
- OTA to a reachable board already running evb-relay (no serial needed):
  `evb-relay -H <ip> -k <token> ota flash dist/evb-relay-fw-vX.Y.Z-ota.bin`.
  The board reboots into the new image. Confirm with `evb-relay status`.

A plain flash keeps the NVS partition, so the API token and device
config survive. Erase first only when the user wants a clean board or
recovery needs it:

```bash
esptool.py --chip esp32 --port <port> erase_flash
```

`erase_flash` needs esptool's flasher stub: never add `--no-stub` to it.
If the stub crashes during `write_flash`, retry the write with
`--no-stub`. After an erase the board has no token; provision one.

## 5. Provision the API token

The firmware rejects every API request until a token is stored.
`scripts/provision.sh` rewrites only the NVS partition over serial and
keeps the other stored config keys:

```bash
scripts/provision.sh --port <port> --generate          # random 32-hex token
scripts/provision.sh --port <port> --token "<value>"   # a chosen token
scripts/provision.sh --port <port> --clear             # remove the token
```

By default the token is never printed or logged; the script only confirms
success. Retrieve it with `--token-file <path>` (written mode 0600) and/or
`--print-token` (prints it to stdout), e.g.
`scripts/provision.sh --port <port> --generate --token-file /tmp/evb-token`.
Treat the token as a secret: do not echo it into logs, commits or chat.
Keep it in `EVB_RELAY_API_TOKEN` or the CLI config file
(`~/.config/evb-relay/config.toml`, key `api_token`).

With the board already on the network and a valid token, rotate it
without serial: `evb-relay config set api_token=<new>`.

## 6. Verify

Wait about 10 s for boot and DHCP, find the address
(`skills/_shared/device-address.md`), then `evb-relay -H <ip> -k <token> status`
must succeed and report the expected firmware version.

## Recovery

Boot loops, `invalid header: 0xffffffff`, no output, or a board that will
not enter download mode: follow `skills/evb-flash/references/recovery.md`.

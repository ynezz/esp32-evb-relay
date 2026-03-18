# Hardware Reference

## ESP32-EVB Board

- **Chip:** ESP32-D0WD (revision v1.0)
- **MAC:** `bc:dd:c2:f2:aa:19`
- **Flash:** 4 MB
- **Preferred single-port alias:** `/dev/esp32-evb`
- **Preferred split flash alias:** `/dev/esp32-evb-flash`
- **Preferred split console alias:** `/dev/esp32-evb-console`
- **Serial baud:** 115200
- **Board FQBN:** `esp32:esp32:esp32-evb`

### Onboard Peripherals

| Peripheral | GPIO |
|------------|------|
| Relay 1    | 32   |
| Relay 2    | 33   |
| Button     | 34   |

### UEXT Connector (I2C)

| Signal | GPIO |
|--------|------|
| SDA    | 13   |
| SCL    | 16   |

- Bus frequency: 100 kHz
- Pull-ups are on-board (idle levels read high)

### Ethernet (LAN8710A PHY)

- PHY address: 0
- MDC: GPIO 23, MDIO: GPIO 18, RMII clock: GPIO 0
- First boot may show a transient EMAC init timeout; the driver
  retries and Ethernet comes up normally on the second pass.
- DHCP-assigned IP is reachable from the host.

### Serial Device Naming

- Do not hardcode guest `/dev/ttyS*` paths in repo defaults, CI, or
  runner notes. Under QEMU/virsh, the guest numbering is dynamic and may
  change when the domain XML or PCI layout changes.
- Prefer stable guest-side udev aliases:
  - `/dev/esp32-evb` for single-port setups
  - `/dev/esp32-evb-flash` and `/dev/esp32-evb-console` for split
    flash/control and live-UART setups
- An example guest-side rule file lives at
  [`tools/udev/99-esp32-evb-qemu-serial.rules.example`](../tools/udev/99-esp32-evb-qemu-serial.rules.example).
- For the current virsh/QEMU runner, `udevadm info` shows the pinned PCI
  addresses `0000:00:07.0` and `0000:00:09.0` for the two guest serial
  adapters. The example udev rules bind aliases to those PCI addresses,
  not to transient tty node names.

### Flashing

```bash
# Single-port setup using the stable default alias:
EVB_SERIAL_PORT=/dev/esp32-evb just test-device

# Split flash/control and live UART aliases:
EVB_FLASH_PORT=/dev/esp32-evb-flash \
EVB_SERIAL_PORT=/dev/esp32-evb-console \
just test-device

# Probe ROM download mode directly. flash.sh, provision.sh, test-device,
# and test-integration run this guardrail automatically before flashing.
./scripts/check-download-mode.sh --port /dev/esp32-evb

# For split-port runners, pass the live UART alias as well so the helper
# can show whether the probe only reset the app instead of reaching the
# ROM downloader.
./scripts/check-download-mode.sh \
  --port /dev/esp32-evb-flash \
  --console-port /dev/esp32-evb-console

# Compile
/tmp/arduino-cli compile --fqbn esp32:esp32:esp32-evb <sketch_dir>

# Upload (must use 115200 — 921600 causes sync timeouts)
/tmp/arduino-cli upload --port /dev/esp32-evb \
  --fqbn esp32:esp32:esp32-evb \
  --upload-property upload.speed=115200 <sketch_dir>

# Low-level flash (esptool, use --no-stub to avoid stub crashes)
python3 -m esptool --no-stub --chip esp32 --port /dev/esp32-evb \
  write_flash 0x10000 <binary.bin>
```

### Current Runner Caveat (2026-03-18)

- The self-hosted QEMU runner currently exposes two PCI 16550 guest
  ports. The live UART console most recently enumerated as
  `/dev/ttyS6`, but the guest `/dev/ttyS*` numbering itself is not
  stable and should only be treated as a diagnostic detail.
- `udevadm info` and `lspci` show both ports are guest-visible QEMU PCI
  16550A adapters (`Red Hat, Inc. QEMU PCI 16550A Adapter`), not a
  directly enumerated USB UART on the Linux guest.
- The guest currently exposes no `/dev/serial/by-id` aliases and no
  `/dev/gpiochip*` devices for a separate GPIO0/EN control path, which
  strongly suggests the missing download-mode assertion lives in the VM
  bridge or host-side wiring rather than in repo scripts.
- The guest PCI device at `0000:00:07.0` appears to be the control path
  and `0000:00:09.0` carries the live UART console. The example udev
  aliases map those to `/dev/esp32-evb-flash` and
  `/dev/esp32-evb-console` respectively.
- Brute-force probing of all 64 3-step DTR/RTS sequences on the control
  path while monitoring the live UART never entered ROM download mode.
  Every reboot stayed in `boot:0x1b (SPI_FAST_FLASH_BOOT)`.
- Re-running exact emulations of esptool's `ClassicReset` and
  `UnixTightReset` sequences on the control path still only produces a
  software reset on the live UART; the board continues to boot the app
  with `boot:0x1b` instead of entering the ROM downloader.
- A manual split-port probe that used the control path only for control
  pulses and the live UART for esptool data also failed. `esptool
  --before no_reset` on the live UART returned `Invalid head of packet`,
  which confirms the runner can reset the board but still cannot hold
  GPIO0 in the ROM-loader state.
- Re-running raw `esptool` probes from the guest still shows the same
  split behavior: the control path returns `No serial data received`,
  while the live UART reads boot/app console bytes such as `Invalid head
  of packet (0x20)` because the board keeps rebooting into the flashed app
  instead of entering ROM download mode.
- `scripts/check-download-mode.sh` now accepts `--console-port` (and
  defaults it from `EVB_SERIAL_PORT` when distinct) so failed download
  probes print a short live UART preview from the console path. That
  makes the split control-vs-console mapping explicit without re-running
  manual ad-hoc probes.
- The image currently flashed on the board reports app version
  `2ae9772-dirty` with build time `2026-03-18 07:46:58 UTC`, which
  predates commit `7815069` that added `network_init()` before
  `rest_api_start()`. That stale image still boot-loops with
  `tcpip_send_msg_wait_sem ... (Invalid mbox)` because it reaches the
  HTTP server before `esp_netif_init()` and Ethernet bring-up.
- The bundled Olimex board docs state that ESP32-EVB boards do not expose
  BOOT-button functionality by default. Manual forced boot mode requires
  a hardware rework around resistors `R46` and `R14`.
- Treat `Wrong boot mode detected` and `No serial data received` failures
  on this runner as hardware or infrastructure blockers until GPIO0/EN
  download-mode control is fixed outside the repo.
- Treat the current boot loop on the live UART path as stale-firmware evidence
  until download-mode control is restored and a post-`7815069` firmware
  build can be flashed onto the board.
- `scripts/check-download-mode.sh` now surfaces those signatures before
  `scripts/flash.sh`, `scripts/provision.sh`, `just test-device`, and
  `just test-integration` attempt more expensive flashing steps.

---

## Olimex MOD-IO (4-Relay Board)

**WARNING — MOD-IO and MOD-IO2 are different boards with different
I2C addresses and protocols. Do NOT confuse them.**

| Property | MOD-IO | MOD-IO2 |
|----------|--------|---------|
| I2C address | **`0x58`** | `0x21` |
| Relay write cmd | `0x10` | `0x40` |
| Relay read cmd | `0x40` | — |
| Relays | 4 | 2 |

### MOD-IO I2C Protocol (address `0x58`)

| Command | Byte | Payload | Description |
|---------|------|---------|-------------|
| Set relays | `0x10` | 1 byte bitmask (bits 0–3) | `0x01`=R1, `0x02`=R2, `0x04`=R3, `0x08`=R4, `0x0F`=all |
| Read digital inputs | `0x20` | — (read 1 byte) | 4 opto-isolated inputs |
| Read analog input | `0x30`–`0x33` | — (read 2 bytes) | 10-bit ADC, LSB:MSB |
| Read relay state | `0x40` | — (read 1 byte) | Current relay bitmask |
| Change address | `0xF0` | 1 byte new addr | Requires PROG jumper closed |

For the deployed MOD-IO board used in this project, command `0x40` is the
authoritative relay-state source. Older write-only / unknown-state assumptions
elsewhere in the repo came from earlier debugging against the wrong board and
address and should not guide new firmware or tests.

### Arduino Example (relay sweep)

```cpp
#include <Wire.h>

#define MODIO_ADDR        0x58
#define MODIO_RELAY_WRITE 0x10

void setup() {
  Wire.begin(13, 16);       // ESP32-EVB UEXT I2C pins
  Wire.setClock(100000);

  // Turn on relay 1
  Wire.beginTransmission(MODIO_ADDR);
  Wire.write(MODIO_RELAY_WRITE);
  Wire.write(0x01);
  Wire.endTransmission();
}
```

### I2C Debugging Tips

- Always scan the full 7-bit range (`0x01`–`0x7F`). MOD-IO sits at
  `0x58` which is above the commonly-used half-range `0x3F` cutoff.
- `Wire.endTransmission()` return codes: 0=ACK, 2=NACK addr,
  4=other error.
- The UEXT bus idles high (both SDA and SCL read `1`) when pull-ups
  are working. If idle levels are `00`, you are on the wrong pins.

### Verified Working (2026-03-16)

Full I2C scan on bus `13/16` finds exactly one device at `0x58`.
Relay sweep (masks `0x00`, `0x01`, `0x02`, `0x04`, `0x08`, `0x0F`,
`0x00`) completes with all `txErr=0`. Relay state readback confirms
`0x00` after final all-off command.

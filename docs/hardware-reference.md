# Hardware Reference

## ESP32-EVB Board

- **Chip:** ESP32-D0WD (revision v1.0)
- **MAC:** `bc:dd:c2:f2:aa:19`
- **Flash:** 4 MB
- **Preferred serial alias:** `/dev/esp32-evb`
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

- The runner passes the CH340 USB-serial adapter (`1a86:7523`) into the
  guest via USB device passthrough. The guest sees a native USB-serial
  port at `/dev/ttyUSB*` (ch341 driver).
- Do not hardcode `/dev/ttyUSB0` or any other transient device path in
  repo defaults, CI, or runner notes.
- Prefer the stable guest-side udev alias `/dev/esp32-evb` for the
  current single-port runner.
- Reserve `/dev/esp32-evb-flash` and `/dev/esp32-evb-console` for
  setups that really expose separate flash/control and live-UART paths.
- An example guest-side rule file lives at
  [`tools/udev/99-esp32-evb-qemu-serial.rules.example`](../tools/udev/99-esp32-evb-qemu-serial.rules.example).
- That example rule matches the CH340 USB VID/PID (`1a86:7523`) and
  creates the single-port `/dev/esp32-evb` alias.

### Flashing

```bash
# Single-port setup using the stable default alias:
EVB_SERIAL_PORT=/dev/esp32-evb just test-device

# Hypothetical split-port setup with extra hardware:
EVB_FLASH_PORT=/dev/esp32-evb-flash \
EVB_SERIAL_PORT=/dev/esp32-evb-console \
just test-device

# Probe ROM download mode directly. flash.sh, provision.sh, test-device,
# and test-integration run this guardrail automatically before flashing.
./scripts/check-download-mode.sh --port /dev/esp32-evb

# For hypothetical split-port runners, pass the live UART alias as well
# so the helper can show whether the probe only reset the app instead of
# reaching the ROM downloader.
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

### Current Runner Caveat

- The self-hosted QEMU runner currently exposes the board as a single
  guest-visible QEMU PCI 16550A adapter (`Red Hat, Inc. QEMU PCI 16550A
  Adapter`) under PCI slot `0000:00:09.0`.
- The guest currently exposes no `/dev/serial/by-id` aliases, so the
  repo-standard `/dev/esp32-evb` alias should be created with a guest
  udev rule that matches the stable PCI parent slot.
- On 2026-03-18 the guest tty node happened to enumerate as
  `/dev/ttyS4`. Treat that only as historical diagnostics, not an
  operational default.
- `udevadm info -a -n /dev/ttyS4` was the command used to confirm the
  correct parent match for the alias rule is `KERNELS=="0000:00:09.0"`.
- Runnable commands in this repo should continue to use
  `/dev/esp32-evb`, not a transient `/dev/ttyS*` node.
- The image flashed after restoring download-mode access reports
  app version `0.0.0-dev` and reaches Ethernet DHCP successfully before
  starting the REST API.
- The bundled Olimex board docs state that ESP32-EVB boards do not expose
  BOOT-button functionality by default. Manual forced boot mode requires
  a hardware rework around resistors `R46` and `R14`.
- `scripts/check-download-mode.sh` still runs before `scripts/flash.sh`,
  `scripts/provision.sh`, `just test-device`, and `just test-integration`
  so serial regressions fail fast with a clearer signature.

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

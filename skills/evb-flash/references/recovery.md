# Recovering an ESP32-EVB

Read the boot log first; it tells you which case you are in. The console
runs at 115200 baud. Agents have no TTY for `idf.py monitor`, so capture a
bounded window with pyserial (from the ESP-IDF Python env or `.venv`):

```bash
timeout 20 python3 -c "
import serial, sys, time
s = serial.Serial('<port>', 115200, timeout=1)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False  # reset pulse
end = time.time() + 15
while time.time() < end:
    sys.stdout.write(s.read(512).decode('utf-8', 'replace')); sys.stdout.flush()
"
```

Close the capture before flashing: only one process may hold the port.

## Symptoms

| Boot log shows | Meaning | Fix |
| --- | --- | --- |
| `invalid header: 0xffffffff`, repeating | app region is blank (erased flash, or only a bootloader was written) | write the full image (`write_flash 0x0 ...-full.bin`) or run `scripts/flash.sh` |
| `invalid header` with other values, or `ota data partition invalid` | wrong image at the wrong offset (for example an `-ota.bin` written at `0x0`) | `erase_flash`, then write `-full.bin` at `0x0` |
| panic / `Guru Meditation` / `abort()` loop | firmware crashes at boot | decode the backtrace against the matching `.elf`; flash a known-good release; report the log |
| `Ethernet got IP: ip=...` then nothing on the network | firmware fine, network path blocked | see `skills/_shared/device-address.md` |
| nothing at all | wrong port, port busy, no power, or bad cable | recheck `skills/_shared/serial-port.md`; `fuser <port>` |

## Download mode will not engage

1. `scripts/check-download-mode.sh --port <port>` shows what esptool sees.
2. Free the port (`fuser <port>`) and replug the USB cable.
3. The CH340 auto-reset normally works without any button. On
   passthrough setups (QEMU PCI serial) the reset lines may not reach the
   chip; see the CI runner section of `docs/hardware-reference.md`.
4. Still stuck: stop and report. Forcing download mode by hand needs a
   hardware rework (`R46`/`R14`) that only the user can do.

## Clean restart

```bash
esptool.py --chip esp32 --port <port> erase_flash       # needs the stub
esptool.py --chip esp32 --port <port> --baud 115200 \
  write_flash 0x0 dist/evb-relay-fw-vX.Y.Z-full.bin
scripts/provision.sh --port <port> --generate
```

Erasing wipes the token and all device config (hostname, WiFi, relay
boot policy). Tell the user before doing it.

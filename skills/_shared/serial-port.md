# Finding the ESP32-EVB serial port

Shared by the evb-flash and evb-test-device skills.

The board's USB-serial bridge is a WCH CH340 (USB vendor ID `1a86`).
Every script and `just` recipe takes the port from `--port`,
`EVB_FLASH_PORT` or `EVB_SERIAL_PORT`, defaulting to `/dev/esp32-evb`.

## Pick the port

1. If `/dev/esp32-evb` exists, use it. It is a udev alias someone set up
   for this board on purpose.
2. Otherwise list stable names: `ls -l /dev/serial/by-id/`. The EVB shows
   up as an entry containing `1a86` (for example
   `usb-1a86_USB_Serial-if00-port0`). Use the by-id path, not the
   `ttyUSB*` node it points to; the number changes across replugs.
3. No by-id directory (containers, some VMs): fall back to
   `ls /dev/ttyUSB* /dev/ttyACM*` and compare the list with the board
   unplugged versus plugged in, or read `dmesg | tail` right after
   plugging it in.

## Rules

- **Never open, probe or flash a port you have not identified as the
  EVB.** Other USB-serial adapters on the host (FTDI, CP210x, other
  CH340s) belong to other hardware; opening them can reset those devices.
  With more than one candidate and no way to tell which one is the EVB,
  ask the user.
- Only one process may own the port at a time. Close monitors and kill
  stale readers (`fuser <port>`) before flashing.
- `Permission denied` on the port means the user is not in the
  `dialout` group (Fedora, Debian). Report it; do not `chmod` device nodes.
- The CH340 drives EN/GPIO0 itself, so esptool's default auto-reset gets
  into ROM download mode without anyone pressing a button. To confirm
  the port reaches the ROM loader, run
  `scripts/check-download-mode.sh --port <port>`. It resets the chip, so
  run it only on the confirmed EVB port.

## Optional: a stable alias

For a bench that keeps the board, a udev rule creates `/dev/esp32-evb`.
CH340s usually have no serial number, so match the USB path
(`udevadm info -q property -n /dev/ttyUSB0 | grep ID_PATH=`), e.g.
`/etc/udev/rules.d/99-esp32-evb.rules`:

```text
SUBSYSTEM=="tty", ENV{ID_PATH}=="pci-0000:00:14.0-usb-0:2:1.0", SYMLINK+="esp32-evb"
```

This is a host change; propose it and leave installing it (`sudo`) to the
user. The QEMU passthrough CI runner uses a different rule, see
`tools/udev/99-esp32-evb-qemu-serial.rules.example`.

# Quickstart

Pre-built binaries for this release: an ESP32-C6 firmware image and a Flipper Zero FAP. This
covers flashing both and pairing them. For the full picture (protocol, security model, roadmap),
see [README.md](README.md) and [docs/](docs/); this page only covers using the release binaries.

## What you need

- An **ESP32-C6-DevKitC-1-N4** (4 MB flash) connected via USB.
- A **Flipper Zero** running **Unleashed firmware `unlshd-092`** (or compatible), with its SD
  card accessible (via qFlipper or the Flipper's own file browser).
- [`esptool`](https://github.com/espressif/esptool) installed (`pip install esptool`) to flash
  the ESP32 binary.

## 1. Flash the ESP32

Release assets: `bootloader.bin`, `partition-table.bin`, `flipper_esp32_over_ble.bin`.

```
esptool.py --chip esp32c6 -p COMx -b 460800 write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0     bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 flipper_esp32_over_ble.bin
```

Replace `COMx` with the board's serial port. The board reboots automatically once flashing
finishes.

## 2. Install the FAP on the Flipper

Copy the release's `flipper_esp32_over_ble.fap` onto the Flipper's SD card, at
`/ext/apps/Connectivity/flipper_esp32_over_ble.fap` — via qFlipper's file browser, or any
tool that can write to the SD card. Then launch it from the Flipper's Apps menu
(Connectivity category).

## 3. Pair the two devices

1. Power/reset the ESP32. A brand-new board (no stored pairing) opens a **120-second pairing
   window** automatically — nothing else to do on that side.
2. On the Flipper, launch the app and press **OK** to start pairing. You'll see
   `Waiting for ESP32...` → `Connected, exchanging keys...` → `Confirming...` → `Saving...` →
   **`Paired`** (LED goes solid blue).
3. If you see `Failed: <reason>`, reset the ESP32 for a fresh pairing window and try again — a
   board only accepts one pairing attempt per window.

Once paired, the Flipper auto-reconnects and re-authenticates on every future launch — no
OK-press needed again. From the Home menu you can reach whatever capabilities the connected
board advertises (Wi-Fi scan, BLE scan, wardriving, GPS, publish-to-wdgwars.pl, depending on
what's built into this release).

## More detail

- [docs/USER_GUIDE.md](docs/USER_GUIDE.md) — full walkthrough of every screen and feature.
- [docs/PROTOCOL.md](docs/PROTOCOL.md) — the wire contract.
- [docs/PAIRING.md](docs/PAIRING.md) — the pairing ceremony, step by step.
- [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) — what's currently implemented.

# Build Baselines

This file records the pinned inputs for phase 1. The baseline images must be built before project behavior is added.

## ESP32-C6

- Board: ESP32-C6-DevKitC-1-N4
- ESP-IDF: v5.5.2
- Target: `esp32c6`
- Flash size: 4 MB, detected read-only with `esptool flash_id` on COM9
- Project: `esp32/`
- ESP-IDF v5.5.2 includes NimBLE central-mode support, mbedTLS X25519/HKDF-SHA-256/HMAC-SHA-256/AES-256-GCM, and encrypted-NVS support — standard components of this and prior ESP-IDF releases, confirmed rather than assumed from the fact that the version happened to already be installed. (Runtime sessions use AES-256-GCM, not AES-128-GCM as originally specified — see `docs/PLAN.md` step 6.)

## Heltec WiFi LoRa 32 V2 (Phase 4, in progress since 2026-09-16)

Second ESP32 target board for display and LoRa capabilities (see `docs/PLAN.md` step 7 capability roadmap). This is a different chip family from the pinned C6 baseline above, not a peripheral addition to it:

- Classic ESP32 (dual-core Xtensa), not the C6's RISC-V.
- SX1276 LoRa radio and SSD1306 OLED display onboard.
- No native USB — requires a USB-UART bridge chip to flash, unlike the C6's native USB-Serial/JTAG.

**Board bring-up confirmed read-only 2026-09-16** (Phase 4 step 1):

- Board revision: silkscreen reads **"WiFi LoRa 32 V2"** (not V2.1).
- Chip: ESP32-D0WDQ6, revision v1.0.
- Flash: **8 MB** (Winbond), detected read-only with `esptool flash_id` — matches the V2/V2.1 8MB expectation in the hardware doc below, rules out the older V1's 4MB.
- MAC: `a4:cf:12:03:ba:58`.
- USB-UART bridge: Silicon Labs CP210x, enumerated as COM10 (not stable across sessions/reboots — reconfirm before hardware work, same caveat as the C6's COM9/Flipper's COM8).
- Toolchain: the classic `esp32` (Xtensa) toolchain was not installed prior to this (this project had only ever installed `esp32c6`'s RISC-V toolchain); installed via `idf_tools.py install --targets=esp32`. `idf-env.json` now lists both `esp32` and `esp32c6` as selected targets.
- An unmodified `hello_world` example built and flashed cleanly for the `esp32` target, confirmed booting over serial.

Do not carry forward C6 pin mappings to this board — see `docs/hardware/esp32-c6-devkitc-1/README.md`, which already documents this distinction.

Full hardware reference (pinout, board-revision ambiguity, per-vendor-documentation caveats): [docs/hardware/heltec-wifi-lora-32-v2/README.md](hardware/heltec-wifi-lora-32-v2/README.md). Phase 4 design and step-by-step status (architecture decision confirmed, gate-override decision, step tracking): `docs/PLAN.md`'s "Phase 4: Heltec WiFi LoRa 32 V2 board support" section.

## OLIMEX MOD-ESP32-C5 (started 2026-09-25)

Third ESP32 target board, `esp32c5/`. A different chip family from both prior boards: RISC-V
like the C6, but with a **dual-band Wi-Fi 6 (2.4 GHz + 5 GHz)** radio. Initial pass (Phase 8 Step
2) scoped to 2.4 GHz only; lifted the same day (Step 3) — this board now scans both bands and
has `wardriving` ported (see `docs/PLAN.md`'s Phase 8 section for the scope-reversal narrative).

- Board: OLIMEX MOD-ESP32-C5 Rev. A, module ESP32-C5-WROOM-1-N8R4.
- Chip: ESP32-C5, revision v1.0, single core + LP core, 240 MHz, BLE 5 (LE), IEEE 802.15.4.
- Flash: **8 MB**, detected read-only via `esptool flash_id` on COM11.
- MAC: `d0:cf:13:ff:fe:e0:88:40`.
- USB: native USB-Serial/JTAG (no bridge chip), same reset/flash handling as the C6.
- ESP-IDF `v5.5.2` already includes `esp32c5` SoC support; the `riscv32-esp-elf` toolchain is
  shared with the C6 target (`idf_tools.py install --targets=esp32c5` found nothing new to
  install beyond registering the target).
- GPS: ATGM336H wired to GPIO4 (ESP32 RX ← GPS TX) / GPIO5 (ESP32 TX → GPS RX) — the board's
  UEXT-connector UART pins, not GPIO2/GPIO3 (those are wired to UEXT I2C on this board and are
  also JTAG strapping pins).
- **No onboard pushbutton** — the factory-reset gesture (BOOT-hold 5s) used on the C6/Heltec
  has no hardware input here; deferred, see `docs/BACKLOG.md` BL15.
- Full hardware reference (pin map, schematic, strapping-pin notes):
  [docs/hardware/olimex-mod-esp32-c5/README.md](hardware/olimex-mod-esp32-c5/README.md).

## Flipper

- Distribution: Unleashed stable
- Release: `unlshd-092`
- Commit: `3c9be0fdd9d301a9436765099a2d1780b36a1795`
- Reported API: 88.4
- Repository: https://github.com/DarkFlippers/unleashed-firmware
- Project: `flipper/`

The FAP targets only this pinned API. Compatibility with later Unleashed API revisions is a future phase 2 enhancement.

## Baseline status

- [x] Install ESP-IDF v5.5.2 on Windows and repair the target RISC-V toolchain
- [x] Measure physical C6 flash size and confirm the partition table
- [x] Build the unmodified ESP32 baseline
- [x] Fetch the pinned Unleashed checkout
- [x] Build the standalone FAP baseline

The FAP was built with the pinned Unleashed Windows `fbt.cmd` wrapper using a temporary copy under `applications_user/flipper_esp32_over_ble`, because this FBT revision resolves `APPSRC` only from known app directories. The project-specific artifact is `build/f7-firmware-D/.extapps/flipper_esp32_over_ble.fap` in that checkout.

The ESP32 baseline build completed successfully with ESP-IDF v5.5.2. Verified artifacts are `esp32/build/flipper_esp32_over_ble.elf` (3,590,924 bytes), `esp32/build/flipper_esp32_over_ble.bin` (161,888 bytes), `esp32/build/flipper_esp32_over_ble.map` (2,828,886 bytes), `esp32/build/flasher_args.json` (959 bytes), and `esp32/build/project_description.json` (195,509 bytes).

The ESP32-C6 flash-ID query was read-only. COM4 and COM3 were busy; COM9 responded as an ESP32-C6 in USB-Serial/JTAG mode. `esptool flash_id` detected a 4 MB flash, confirming the 4 MB project configuration and current partition table.

## Step 2 transport verification

The fixed-payload BLE transport was verified on the physical ESP32-C6 and Flipper Zero on 2026-09-02.

- Flipper profile advertised successfully with the v2 service UUID; the earlier GAP error 146 was eliminated by using UUID-only advertising data.
- ESP32 discovered the peer advertisement and connected as the central.
- ATT MTU negotiation completed at 256 bytes.
- Service and both 128-bit characteristics were discovered.
- The notification CCCD was discovered and enabled.
- ESP32 wrote `ESP32-C6 transport smoke test`.
- Flipper logged receipt of the exact payload and returned `FLIPPER-ACK`.
- ESP32 received and verified the `FLIPPER-ACK` prefix in the fixed-size notification.
- After disconnect reason 08, the ESP32 reconnected and the Flipper received the payload again.

ESP32 build and flash passed with hash verification. Known remaining work at the time (step 1/2):
ESP32 logs still reported that NVS was not initialized before Bluetooth startup, although RF
calibration fell back successfully; pairing, persistence, encryption, CBOR framing, and
capabilities were still future roadmap work. All of that has since shipped — see
[docs/SESSION_MEMORY.md](SESSION_MEMORY.md) for current status.
# Build Baselines

This file records the pinned inputs for phase 1. The baseline images must be built before project behavior is added.

## ESP32-C6

- Board: ESP32-C6-DevKitC-1-N4
- ESP-IDF: v5.5.2
- Target: `esp32c6`
- Flash size: 4 MB, detected read-only with `esptool flash_id` on COM9
- Project: `esp32/`
- ESP-IDF v5.5.2 includes NimBLE central-mode support, mbedTLS X25519/HKDF-SHA-256/HMAC-SHA-256/AES-256-GCM, and encrypted-NVS support — standard components of this and prior ESP-IDF releases, confirmed rather than assumed from the fact that the version happened to already be installed. (Runtime sessions use AES-256-GCM, not AES-128-GCM as originally specified — see `docs/PLAN.md` step 6.)

## Heltec WiFi LoRa 32 V2 (future, not yet started)

Planned as a second ESP32 target board for display and LoRa capabilities (see `docs/PLAN.md` step 7 capability roadmap). This is a different chip family from the pinned C6 baseline above, not a peripheral addition to it:

- Classic ESP32 (dual-core Xtensa), not the C6's RISC-V.
- SX1276 LoRa radio and SSD1306 OLED display onboard.
- No native USB — requires a USB-UART bridge chip to flash, unlike the C6's native USB-Serial/JTAG.

Not yet pinned to a specific board revision or ESP-IDF target config. Do not carry forward C6 pin mappings to this board — see `docs/hardware/esp32-c6-devkitc-1/README.md`, which already documents this distinction.

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

ESP32 build and flash passed with hash verification. Known remaining work: ESP32 logs still report that NVS was not initialized before Bluetooth startup, although RF calibration falls back successfully. Pairing, persistence, encryption, CBOR framing, and capabilities remain future roadmap work.
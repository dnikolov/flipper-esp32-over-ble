# Project History

This document records the setup and baseline work completed for the Flipper-to-ESP32-over-BLE project. It is intended as a readable handoff and project-history record.

## 2026-09-01: Project direction and Phase 1 decisions

The project began as a documentation-only workspace. Before implementation, the required hardware, firmware, delivery model, and BLE roles were clarified.

Confirmed decisions:

- Target ESP32 board: **ESP32-C6-DevKitC-1-N4**.
- ESP-IDF target: `esp32c6`.
- ESP32 framework: ESP-IDF.
- Flipper firmware base: **Unleashed stable**.
- Unleashed release: `unlshd-092`.
- Flipper delivery: standalone external FAP targeting `f7`.
- BLE architecture: Flipper is the peripheral/GATT server; ESP32-C6 is the central/GATT client.
- Phase 1 scope: establish reproducible build and hardware baselines before adding BLE behavior.

Security and protocol direction recorded for later implementation:

- Reset-gated trusted-environment X25519 pairing.
- HKDF-SHA-256 and HMAC-SHA-256 for key derivation and authentication.
- AES-128-GCM for runtime sessions.
- Sequence and replay protection.
- App-owned FAP persistence with an explicit local-access threat boundary.

## 2026-09-01: Versions and repositories pinned

The Flipper firmware source was changed from the generic upstream reference to the requested Unleashed stable checkout.

- Repository: `https://github.com/DarkFlippers/unleashed-firmware`
- Release: `unlshd-092`
- Commit: `3c9be0fdd9d301a9436765099a2d1780b36a1795`
- Reported Unleashed API: `88.4`
- ESP-IDF: `v5.5.2`
- ESP-IDF installation: `C:\Users\Deyan\esp\esp-idf`

The pins were recorded in `docs/PLAN.md` and later in `docs/BASELINES.md`.

## 2026-09-01: Baseline project scaffolding

Created the minimum source structure needed to exercise both toolchains:

- `esp32/CMakeLists.txt`
- `esp32/sdkconfig.defaults`
- `esp32/partitions.csv`
- `esp32/main/CMakeLists.txt`
- `esp32/main/main.c`
- `flipper/application.fam`
- `flipper/flipper_esp32_over_ble.c`
- `docs/BASELINES.md`

The ESP32 application only logs `ESP32-C6 build baseline`. The FAP entry point is intentionally inert and returns successfully. No BLE or protocol implementation was started during Phase 1.

Initial ESP32 configuration used:

- ESP32-C6 target.
- 4 MB flash setting.
- Custom partition table.
- NVS at `0x9000`, size `0x6000`.
- PHY data at `0xf000`, size `0x1000`.
- Factory app at `0x10000`, size `0x180000`.

## 2026-09-01: ESP-IDF installation and initial failure

ESP-IDF v5.5.2 was installed and `idf.py` became available after the installer completed.

The first ESP32 build reached the linker but failed because the RISC-V toolchain installation was incomplete. The following required files were missing:

- `bin\\riscv32-esp-elf-gcc.exe`
- `riscv32-esp-elf\\lib\\libc.a`
- `riscv32-esp-elf\\lib\\libnosys.a`
- `lib\\gcc\\riscv32-esp-elf\\14.2.0\\libgcc.a`

The toolchain directory existed but contained only a partial set of files. The installer had reported success, but direct compiler and library checks disproved that result.

## 2026-09-01 to 2026-09-02: RISC-V toolchain repair

Only the corrupted generated tool directory was removed. The ESP-IDF source checkout and project files were preserved.

The missing RISC-V package was restored from the verified local Espressif archive/install source. Verification then succeeded:

- `riscv32-esp-elf-gcc --version` runs successfully.
- Compiler version: GCC `14.2.0`.
- Required compiler and runtime archive paths are present.

This repair fixed the original linker-toolchain problem, but the first clean configure exposed missing ESP-IDF component contents as a separate issue.

## 2026-09-02: ESP-IDF component repairs

### protobuf-c

CMake reported that this source file was missing:

`components/protobuf-c/protobuf-c/protobuf-c.c`

The nested checkout existed but was at an incompatible/incomplete state. The ESP-IDF submodule declaration was checked, and the `protobuf-c` submodule was initialized/restored. The expected source file became available.

### esp_wifi binary library

The next build reported that the ESP32-C6 Wi-Fi binary was missing:

`components/esp_wifi/lib/esp32c6/libcore.a`

Investigation showed that `components/esp_wifi/lib` was an empty unborn Git repository with no commit and no usable remote. The ESP-IDF superproject recorded the required submodule revision:

- Remote: `https://github.com/espressif/esp32-wifi-lib.git`
- Pinned commit: `01d52d9e69032c486015dc28b08c3bf6aaf348a9`

The declared remote was restored in the empty submodule, the pinned commit was fetched and checked out, and the archive was verified:

- File: `components/esp_wifi/lib/esp32c6/libcore.a`
- Size: `4,108` bytes
- Commit: `01d52d9e69032c486015dc28b08c3bf6aaf348a9`

The ESP-IDF checkout also emitted an unrelated warning involving broken nested OpenThread Git metadata. OpenThread was not modified because it was not needed to complete the project build.

## 2026-09-02: ESP32 baseline build

After the toolchain and missing component dependencies were restored, the baseline build was run with:

```powershell
. C:\Users\Deyan\esp\esp-idf\export.ps1
Set-Location C:\Users\Deyan\flipper-esp32-over-ble\esp32
idf.py build
```

The build completed successfully.

Verified artifacts:

- `esp32/build/flipper_esp32_over_ble.elf` - `3,590,924` bytes
- `esp32/build/flipper_esp32_over_ble.bin` - `161,888` bytes
- `esp32/build/flipper_esp32_over_ble.map` - `2,828,886` bytes
- `esp32/build/flasher_args.json` - `959` bytes
- `esp32/build/project_description.json` - `195,509` bytes

The ESP-IDF size check reported:

- Binary size: `0x27860`
- Smallest app partition: `0x180000`
- Free space: `0x1587a0`, reported as `90%`

## 2026-09-02: Flipper FAP baseline build

The standalone FAP manifest was accepted by the pinned Unleashed build system.

On Windows, the Unleashed `fbt.cmd` wrapper was used. A temporary application copy under `applications_user/flipper_esp32_over_ble` was required because this FBT revision resolves `APPSRC` only from recognized application directories.

Verified artifact:

- Checkout: `C:\Users\Deyan\unleashed-firmware-unlshd-092`
- Artifact: `build/f7-firmware-D/.extapps/flipper_esp32_over_ble.fap`
- Size: `596` bytes

## 2026-09-02: Hardware identification and flash measurement

The connected board was identified without flashing:

- Serial port: `COM9`
- Chip: ESP32-C6
- Revision: `v0.2`
- Connection mode: USB-Serial/JTAG
- MAC observed: `ac:eb:e6:ff:fe:da:0b:20`

`COM3` and `COM4` were busy during probing.

A generic chip query initially reported unknown embedded flash. A dedicated read-only query then succeeded:

```powershell
. C:\Users\Deyan\esp\esp-idf\export.ps1
esptool --chip esp32c6 --port COM9 flash_id
```

Result:

```text
Manufacturer: 20
Device: 4016
Detected flash size: 4MB
```

No flash, erase, or write operation was performed. The measured 4 MB size confirms the current ESP32 configuration and partition-table assumption.

The board documentation notes that a DevKitC-1 v1.2 guide may describe an 8 MB ESP32-C6-WROOM-1(U) variant, so the physical read was retained as the source of truth for this N4 board.

## Phase 1 outcome

Phase 1 acceptance is complete:

- Target board confirmed.
- Target flash size measured read-only.
- ESP-IDF v5.5.2 installed.
- RISC-V toolchain repaired and verified.
- Required ESP-IDF component dependencies restored.
- ESP32 baseline builds successfully.
- Pinned Unleashed stable checkout verified.
- Standalone FAP baseline builds successfully.
- Baseline evidence recorded in `docs/BASELINES.md`.

## 2026-09-02: Step 2 transport configuration confirmed

The implementation configuration for Step 2 was confirmed before code changes:

- Both a Flipper Zero and the ESP32-C6 are available for real end-to-end testing.
- With no saved ESP32 pairing record, the Flipper app will require an explicit user pair/connect action.
- With a saved pairing record, the Flipper app will try to connect automatically to that ESP32.
- The ESP32-C6 will scan and try to connect automatically at boot.
- The first transport test will use a fixed payload. The subsequent increment will introduce the protocol CBOR envelope.
- Only one active connection is allowed.
- Reconnect uses bounded exponential backoff with a maximum of five automatic retries.
- The FAP may temporarily replace the default Bluetooth profile while active and must restore it on exit and recoverable failures.

These choices are saved as the current extensible Step 2 configuration. No Step 2 implementation code has been added yet.

## 2026-09-02: Step 2 fixed-payload transport implementation

The first Step 2 implementation slice was added on both devices.

### ESP32-C6 central

`esp32/main/main.c` now implements:

- Automatic boot-time scanning filtered by the v2 service UUID.
- One active BLE connection.
- MTU exchange with a fallback path.
- Service, characteristic, and notification CCCD discovery.
- Notification subscription.
- A fixed ASCII smoke-test write using write-with-response.
- Bounded notification logging.
- Disconnect cleanup and bounded exponential reconnect delays of 1, 2, 4, 8, and 16 seconds, with a maximum of five retries.

`esp32/main/CMakeLists.txt` declares the Bluetooth component dependency, and the active ESP32 configuration enables NimBLE central mode and one maximum connection.

### Flipper peripheral

`flipper/flipper_esp32_over_ble.c` now implements:

- An explicit user action to start the service when no saved pairing exists.
- A placeholder saved-pairing state for the future automatic-connect workflow.
- The v2 custom GATT service, write characteristic, and notify characteristic.
- Advertising only while the service is active.
- One connection, bounded fixed-payload reception, and a `FLIPPER-ACK` notification response.
- Cleanup that stops advertising, disconnects, releases GATT state, and restores the default Bluetooth profile.

The ESP32 build passed with ESP-IDF v5.5.2. The Flipper FAP build passed with the pinned Unleashed API 88.4. The resulting FAP artifact is `build/f7-firmware-D/.extapps/flipper_esp32_over_ble.fap`, size `5,668` bytes.

The real-device transport smoke test is still pending: discovery, fixed write/notification exchange, and disconnect recovery must be exercised with the available Flipper Zero and ESP32-C6. CBOR framing, pairing, persistence, encryption, and capabilities remain deferred.

## 2026-09-02: Flipper service activation fix

During the first Flipper test, the screen returned from `Service active` to the inactive state about one second after pressing OK. Investigation against the pinned Unleashed Bluetooth lifecycle found that `bt_profile_start()` changes the active Bluetooth profile and emits a transient `BtStatusOff` event while the previous advertising session is stopped. The FAP had treated every `BtStatusOff` event as fatal, so it immediately called `stop_service()`, restored the default profile, and deactivated itself.

The FAP now ignores this normal transient `BtStatusOff` event while its custom profile is active. `BtStatusUnavailable` remains a fatal condition and still triggers cleanup. The updated FAP was rebuilt successfully against Unleashed API 88.4.

- Updated FAP artifact: `build/f7-firmware-D/.extapps/flipper_esp32_over_ble.fap`
- Updated artifact size: `5,664` bytes
- No hardware was flashed during this fix.
- The updated FAP must be reinstalled on the Flipper before repeating the smoke test.

## 2026-09-02: Flipper waiting-versus-connected UI fix

The FAP initially displayed `Service active` as soon as its local GATT profile and advertising started. That made the screen look active even when the ESP32 was powered off, because local advertising is not proof of a peer connection.

The state model now separates advertising from connection:

- Before starting: `OK: start pair/connect`
- Advertising with no ESP32 connection: `Waiting for ESP32...`
- After `BtStatusConnected`: `ESP32 connected`

`service_active` is set only after a connection event, while the separate advertising state tracks the local waiting mode. The OK action also refuses to start another profile while the existing profile is waiting. The updated FAP was synchronized to the pinned Unleashed checkout and built successfully.

- Updated FAP artifact: `build/f7-firmware-D/.extapps/flipper_esp32_over_ble.fap`
- Updated artifact size: `5,728` bytes
- No hardware was flashed during this fix.

## 2026-09-02: Fixed-payload transport hardware verification

The real-device smoke test passed on the ESP32-C6-DevKitC-1-N4 at `COM9` and the Flipper Zero at `COM8`.

### Root causes fixed

- ESP32 NimBLE 128-bit UUID initializers were corrected to the Flipper GATT UUID byte order.
- Descriptor discovery now uses the valid service handle range instead of starting after the notify value.
- CCCD matching now uses the CCCD UUID without relying on the descriptor callback's auxiliary handle.
- Flipper advertising was reduced to UUID-only data, eliminating `set_discoverable failed 146`.
- ESP32 notification verification accepts the fixed 64-byte notification value and checks the `FLIPPER-ACK` prefix.

### Verified result

ESP32 logs showed the v2 advertisement, connection, ATT MTU 256, service and characteristic discovery, CCCD handle 17, notification subscription, smoke payload write, notification receipt, and `notification payload: FLIPPER-ACK`.

Flipper logs showed successful profile startup and advertising, receipt of `ESP32-C6 transport smoke test`, disconnect reason 08, automatic reconnection, and receipt of the payload again after reconnect. The final ESP32 image was built, flashed to `COM9`, and hash-verified. The FAP was built against Unleashed API 88.4 and synchronized into the pinned checkout.

## 2026-09-03: Step 3 on-device smoke test — stack-overflow crash and fix

Wired the already-tested `framing.c`/`cbor_codec.c` codec into both firmwares' live BLE transport, replacing the step-2 fixed-payload exchange with a fragmented CBOR `error` record sent and decoded in both directions (ESP32<->Flipper).

First hardware attempt crashed the Flipper immediately on starting the BLE profile: `MPU fault, possibly stack overflow`. A single blocked run also showed a "silent hang" after MTU exchange with zero further activity on either side, which cost significant time to investigate before being ruled out: a GATT attribute-count/registration-failure hypothesis (does the `FlipperGattCharacteristicDataCallback` switch used to fix a separate overread bug need more attribute-table slots than the `6` reserved?) — checked against `targets/f7/ble_glue/furi_ble/gatt.c` and `dev_info_service.c`; registration succeeds either way, not the cause.

### Root cause

Stack overflow on the Flipper's `BleEventWorker` FreeRTOS thread, which only has a **1280-byte stack** (`furi_thread_alloc_ex("BleEventWorker", 1280, ...)`, confirmed in `ble_event_thread.c`; `profile_event_handler` runs synchronously on it via the ST BLE stack's event pump). Two stacked causes:

1. `flipper/flipper_esp32_over_ble.c`'s smoke-test code declared its CBOR encode buffers (`payload_buf[256]`, `record_buf[768]`) as **stack-local** inside the reply path (~1,100+ bytes in one frame).
2. The shared `feb_fragment_record()` in `framing.c` (both firmwares) itself stack-allocates a 772-byte fragment buffer (`FEB_FRAG_HEADER_SIZE + FEB_MAX_RECORD_SIZE`) regardless of the actual per-fragment capacity — by itself ~60% of the whole thread budget, and directly in the same call chain.

### Fix

Moved all of the above buffers from stack-local to file-scope `static` storage, in both `flipper/flipper_esp32_over_ble.c` and both copies of `framing.c` (`esp32/main/`, `flipper/`) — safe because there is one active BLE connection and BLE events dispatch single-threaded/serially (verified, not assumed). No protocol/wire-format change; `framing.h`/`cbor_codec.h`/`cbor_codec.c`/`docs/PROTOCOL.md` untouched.

### Verified result

Both boards reflashed (ESP32 via `idf.py -p COM9 flash`, hash-verified; Flipper via `fbt.cmd launch`) and retested with simultaneous live serial captures on `COM9`/`COM8`. Full round trip succeeded with no crash: ESP32 sent a 156-byte `error` record (10 fragments), Flipper decoded it correctly and replied with its own `error` record (9 fragments), which the ESP32 reassembled and decoded correctly. Full detail (exact log lines, byte counts, artifact sizes) is in `docs/SESSION_MEMORY.md`'s 2026-09-03 entries.

Step 3 (record framing) is now fully closed, including the on-device confidence pass. Step 4 (radio coexistence validation) is next per `docs/PLAN.md`.

## Current project state and handoff

The fixed-payload BLE transport is implemented and verified end to end. Phase 2 follow-up work can begin after rereading:

- `docs/PLAN.md`
- `docs/PROTOCOL.md`
- `docs/PAIRING.md`
- `docs/DECISIONS.md`

Preserve these constraints during Phase 2:

- Keep the pinned Unleashed release and API compatibility target.
- Keep ESP-IDF target `esp32c6` and the verified 4 MB flash configuration unless hardware changes.
- Keep Flipper as BLE peripheral/GATT server and ESP32-C6 as central/GATT client.
- Preserve the reset-gated pairing, X25519, HKDF-SHA-256, HMAC-SHA-256, AES-128-GCM, sequence/replay protection, and explicit persistence threat-boundary decisions.
- Avoid flashing or erasing the board unless explicitly requested.

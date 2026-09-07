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

Both boards reflashed (ESP32 via `idf.py -p COM9 flash`, hash-verified; Flipper via `fbt.cmd launch`) and retested with simultaneous live serial captures on `COM9`/`COM8`. Full round trip succeeded with no crash: ESP32 sent a 156-byte `error` record (10 fragments), Flipper decoded it correctly and replied with its own `error` record (9 fragments), which the ESP32 reassembled and decoded correctly. (Exact log lines, byte counts, and artifact sizes from this test were recorded at the time but were not carried forward verbatim into this condensed history.)

Step 3 (record framing) is now fully closed, including the on-device confidence pass. Step 4 (radio coexistence validation) is next per `docs/PLAN.md`.

## 2026-09-03: Step 4 (radio coexistence) validated on real hardware

Before building capabilities that share the ESP32-C6's single 2.4 GHz radio with the BLE link to
the Flipper (Wi-Fi scanning, and later BLE observer scanning for wardriving), a design session
scoped a dedicated coexistence-validation step ahead of pairing/session work rather than leaving it
to final validation. Scope was narrowed to BLE/Wi-Fi only — 802.15.4 has no code yet and was
deferred to the future Zigbee/Thread recon phase.

A throwaway ESP32-only test harness (`esp32/coex_test/`, never wired into the real `main.c`) ran a
single paused-BLE-scan baseline plus an ascending four-point sweep of concurrent Wi-Fi-scan +
BLE-observer-scan duty cycles, up to NimBLE's own default fast-scan parameters (~100% duty). The
Flipper app's launch was confirmed live-scriptable over its own CLI (`loader open <full SD-card
path>` — external FAPs aren't found by name — plus a `press`/`short`/`release` input triple in that
exact order, since the GUI's input dispatcher silently drops a bare `short`), though this proved
unreliable for the actual overnight run (repeated "semaphore timeout" write hangs on the Flipper's
serial port, root cause never found) and the app was launched manually instead.

An unattended overnight run, driven by a new PowerShell orchestrator
(`tools/coex/run_coex_sweep.ps1`) that flashed/monitored/auto-recovered the ESP32, executed all
five sweep points back to back. **All five passed cleanly on the first attempt: zero disconnects,
zero hard fails, zero degradations, even at the theoretical 100%-duty-cycle maximum.** This set the
recommended interval bounds later used for the wardriving/`ble_scan` capability (conservative
~10% duty up to ~100%, Wi-Fi rescans anywhere from continuous to every 30s — all proven stable).
One accepted gap: because zero disconnects occurred, the sweep never actually exercised the
merged-reconnect-scan mechanism (recovering a disconnect via the same BLE-observer scan pass rather
than a dedicated reconnect scan) — left as a backlog item for the eventual full-system validation
step.

Two non-hardware orchestration bugs surfaced and were fixed during the run: a Windows file-sharing
conflict between the orchestrator's `Add-Content` log writer and Git-Bash `tail`/`grep` reading the
same file concurrently crashed the orchestrator twice (fixed by wrapping every `Add-Content` call
in try/catch); both times the ESP32 hardware itself was unaffected, since sweep state lives in
on-device NVS rather than in the orchestrator.

Step 4 is closed; step 5 (trusted-environment pairing) followed.

## 2026-09-03 to 2026-09-05: Step 5 (trusted-environment pairing) implemented and hardware-verified

### Design, and a real feasibility blocker

Before writing any pairing code, a design session found that a standalone Flipper FAP cannot link
against the firmware's own X25519, HKDF, or HMAC-SHA-256 — every `mbedtls_*` symbol is present in
firmware source but unexported at the pinned Unleashed API (88.4), and no HKDF symbol exists at
all. Only raw-key AES-GCM (`furi_hal_crypto_gcm_*`) is usable from a FAP. Every alternative that
avoided writing new asymmetric crypto into the FAP also gave up the "protects against passive BLE
capture during pairing" property, so the decision was to keep the X25519/HKDF/HMAC design as
specified and hand-roll/port the missing primitives into the Flipper firmware: SHA-256/HMAC/HKDF
from spec, and X25519 ported from `curve25519-donna.c` (Adam Langley's BSD-licensed, public-domain-
derived portable implementation) rather than derived in-house, since field arithmetic over
2^255-19 is easy to get subtly wrong. The ESP32 side needed only one new Kconfig option
(`CONFIG_MBEDTLS_HKDF_C=y`); everything else was already available via mbedtls.

The same research also found that `docs/references/flipper-firmware/upstream` was a mislabeled
checkout of vanilla `flipperdevices/flipperzero-firmware`, not the pinned Unleashed `unlshd-092` —
fixed shortly after by re-fetching the mirror from the correct repository at the pinned commit.

Several real wire-format gaps were closed before implementation: `T`'s `service_uuid` byte order
(pinned to RFC 4122 big-endian, the same bug class as the step-2 UUID mismatch above), the
previously-unspecified pairing-record envelope (`{version, type, board_id, payload}`, distinct
from the session-bearing envelope since no session exists yet), and "one attempt per reset window"
clarified to mean literally one attempt total, consumed by any outcome.

### Implementation, and two real hardware bugs

Shared `pairing.h`/`pairing_crypto.h` contracts and a golden end-to-end test vector (self-validated
against a from-scratch pure-Python X25519/RFC-7748 reference, since no Python crypto library was
available) were written first; both firmwares then implemented independently and passed their
host-native tests (ESP32 42/42, Flipper 67/67). The ESP32's X25519 needed a hand-written RFC 7748
Montgomery ladder built directly on mbedtls's bignum primitives, since mbedtls's high-level
`mbedtls_ecp_mul()` path rejects low-order points in a way incompatible with the header's required
total-function (void, no error return) contract.

The first live pairing ceremony between both physical devices surfaced two real bugs, found and
fixed the same session:

1. **ESP32 write-fragment capacity ignored the Flipper's fixed GATT characteristic size.** The
   ESP32 fragmented `pair_init` against the real negotiated ATT MTU (~249 bytes/fragment), but the
   Flipper's Write characteristic has a fixed 64-byte max attribute length regardless of MTU —
   writes were rejected with ATT error 0x0D. Not caught by the step-3 smoke test, which had forced
   a tiny fixed fragment size. Fixed by clamping the ESP32's effective write MTU to the Flipper's
   real 64-byte cap before computing fragment capacity.
2. **Stack overflow in the ported X25519 code, on the Flipper's 1280-byte `BleEventWorker`
   thread** — the same failure class already seen in step 3's framing code. `curve25519-donna`'s
   internal ladder functions (`cmult`/`fmonty`) stacked to nearly 2.4 KB together, almost double
   the entire thread budget. Fixed by converting every crypto scratch buffer in
   `pairing_crypto.c`/`pairing.c` to file-scope `static`, matching the mitigation pattern already
   established for step 3's `framing.c`.

A minor LED bug (the blink never stopped on success, since `sequence_blink_start_blue`'s blink
subsystem needs an explicit `sequence_blink_stop`, not just a static-color message) was found and
fixed the same session. After all three fixes, **the first real X25519/HKDF/HMAC pairing ceremony
between the two physical devices succeeded end to end** — the first time this project's actual
pairing crypto, not just host-native vectors, had run successfully.

**Lesson drawn and folded into both agent definitions the same day:** all three bugs had passed
every host-native test and still failed on hardware, because desktop test stacks are roughly
1000x the size of the Flipper's real BLE-callback stack — a structural hole in the verification
pyramid that host tests can never close for this bug class.

### Hardware verification, and two residual findings

A dedicated verification pass (methodology designed in its own session first, since reboot-survival
and reset-and-repair are entangled by step 5's design — every reset unconditionally opened a
pairing window until step 6 changed that) confirmed all three "done when" checks: a pairing
survives a Flipper-alone reboot (byte-identical `.dat` file before/after), a reset-and-repair
replaces only the real board's record while a planted dummy second-board file stays untouched, and
a code audit confirmed neither `K_shared` nor `pairing_secret` ever crosses the wire (no BLE
sniffer hardware exists in this project to do a literal RF capture).

Two real findings surfaced and were backlogged (not fixed) during this pass:

- Pairing files were being saved under the built-in Bluetooth service's data directory
  (`/ext/apps_data/bt/pairings/...`) rather than this app's own, because `APP_DATA_PATH`
  resolution is keyed to the calling thread's registered app ID, and the pairing-save code runs on
  the BLE stack's own thread, not the FAP's. (Fixed during step 6, see below.)
- The custom BLE profile appeared to stay connectable after a successful pairing, so a Flipper left
  open on the "Paired" screen could silently complete a second full ceremony if the ESP32 reset
  again — an unintended consequence of "one explicit OK-press to start the app" not meaning "one
  ceremony per app session." (Resolved by design during step 6's reset-vs-runtime-auth change, see
  below.)

Step 5 is fully closed, including hardware verification.

## 2026-09-06: Step 6 (authenticated runtime sessions) implemented and hardware-verified

### Design: reset-vs-runtime-auth, and a real crypto blocker

A design session resolved the step-5-era open question of what a reset should do once a working
`pairing_secret` already exists: **on boot, if a secret is stored, the ESP32 now attempts runtime
auth (`hello`/`hello_ack`/`client_auth`) first, and only opens a pairing window if no secret exists
yet or that attempt is rejected with a new `unknown_board` error** — not on every boot. This
directly resolves the step-5 silent-re-pair finding above (the custom profile no longer needs a
fresh OK-press distinction, since a window simply doesn't open while a secret already works) and
revises the earlier "physical reset alone authorizes re-pairing" framing in `docs/DECISIONS.md`/
`docs/PAIRING.md`. A genuine proof-verification failure (as opposed to "never paired") is
deliberately *not* treated the same way — it's logged and rate-limited but does not auto-open a
window, since conflating the two would let a radio attacker force pairing-window openings just by
corrupting proofs in transit. Explicit re-pairing while both sides hold a valid secret is handled
entirely by the Flipper's local "unpair" action; the ESP32 needs no special action, since its next
connection just gets `unknown_board`.

Before writing the session-crypto contracts, a second real feasibility blocker was found: the
Flipper's only exported raw-key AES-GCM primitive is hardcoded to a 256-bit key at the hardware
level (`furi_hal_crypto.c`'s key-init path unconditionally selects a 256-bit key schedule) — there
is no 128-bit path, and software mbedtls GCM isn't a fallback (every `mbedtls_*` symbol is
unexported, per step 5's earlier finding). **The user chose to revise the protocol from
AES-128-GCM to AES-256-GCM** rather than port a second software AES implementation into the Flipper
firmware, which would have repeated the ported-crypto stack-overflow risk that had already caused
real hardware crashes. `docs/PROTOCOL.md`, `docs/DECISIONS.md`, `docs/PAIRING.md`,
`docs/BASELINES.md`, and `docs/STANDALONE_FAP.md` were all updated; HKDF already supported
32-byte output, so this needed no new primitive, just `length=32` instead of `16`.

An in-firmware, no-PC/no-Flipper physical factory-reset gesture was scoped for right after step 6
(see below) as the recovery path when neither a PC nor a working Flipper session is available; a
Flipper-triggered remote factory reset was explicitly rejected — factory reset must always be a
physical/local-access action on the ESP32, a deliberate security boundary.

### Implementation and hardware verification

Both firmwares implemented `session_crypto.c`/`session.c` against frozen contracts (ESP32:
`mbedtls_gcm_*`; Flipper: `furi_hal_crypto_gcm_*`), passed host-native tests (27/27 and 42/42), and
built clean. The pairing-file path-resolution bug from step 5 was fixed in the same pass — not by
caching `APP_DATA_PATH`'s expansion as originally planned (it's a compile-time string macro, not a
runtime call, so caching it wouldn't have helped), but by calling the exported
`storage_common_resolve_path_and_ensure_app_directory()` once from the app's own thread and caching
the fully-resolved path, which then works correctly regardless of which thread uses it later.

On real hardware, all three "done when" checks (shared vectors, tamper/replay/sequence-gap
rejection, reset-vs-runtime-auth boot decision) passed. A real consequence of the path-resolution
fix: any board paired before this session's flash needed a fresh pairing ceremony, since the old,
buggy pairing-file location doesn't carry forward — this doubled as the live test of the new
`unknown_board` fallback and reset-vs-runtime-auth boot logic, both of which worked exactly as
designed. A same-day LED bug (the runtime-auth success path never stopped the blink subsystem,
unlike the pairing-ceremony's own success path) was found and fixed.

Step 6 is fully closed, including hardware verification.

### Post-step-6 stability fixes

A cluster of stability work landed the same week, largely prompted by real user-observed behavior
rather than a design session:

- **In-firmware factory-reset gesture** (ESP32-only, no FAP/BLE involvement): holding the onboard
  BOOT button (GPIO9) for 5 continuous seconds erases the NVS partition and restarts, falling back
  into the existing boot logic. Confirmed via the hardware guide that reading GPIO9 and driving
  GPIO8 (the onboard **WS2812 addressable** RGB LED, not a plain GPIO — driven via the RMT
  peripheral) well after boot falls outside this project's strapping-pin restriction window.
  **Hardware-verified**: early release aborts silently; a full 5s hold erases the stored secret and
  reopens a pairing window.
- **FAP-close/reopen stuck-connection bug, root-caused.** Closing the Flipper app calls
  `bt_disconnect()`, which for this app's custom (non-RPC) profile only stops advertising — it does
  not terminate an already-established connection. The actual teardown comes from the next call,
  `bt_profile_restore_default()`, which causes an abrupt BLE-coprocessor restart rather than a
  clean disconnect PDU, so the ESP32 has to wait out NimBLE's long default connection-supervision
  timeout before it notices. **Fixed** by giving the ESP32 its own 30-second idle-connection
  timeout (piggybacked on the existing periodic reassembly-timeout callout, per
  `docs/PROTOCOL.md`'s already-specified idle policy) that proactively terminates and reconnects —
  **hardware-verified**, closing the loop in ~30s instead of requiring a physical ESP32 reset.
- **A second, initially-mysterious bug surfaced by the literal repro: a BLE scan "stall" lasting
  1-3 minutes after a FAP close/reopen.** Root-caused to the BLE **controller's**
  duplicate-advertisement filter, which is keyed on device address only and never periodically
  flushed: when the FAP exits, the Flipper briefly advertises its default (non-matching) profile
  from the same MAC, which gets cached as "already seen"; when the FAP relaunches and starts
  advertising the real v2 payload from the same address, the controller silently filters every
  report until cache pressure from other nearby devices evicts the entry, sometimes minutes later.
  Nothing was actually hung — the radio, host, and scan all kept working; a separate never-reset
  lifetime log-line counter (capped at 40 events) had been making healthy fast reconnects look
  identical to genuinely stalled ones in the logs, which sent the first diagnosis in the wrong
  direction. **Fixed** by scanning with `filter_duplicates = 0` (host-side filtering on the service
  UUID match already does the real work) and replacing the dead log-line cap with an uncapped
  periodic summary counter. **Hardware-verified**: a FAP close/reopen cycle now reconnects in ~3
  seconds instead of 100+.
- **`MAX_RECONNECT_RETRIES` hard-stop fix.** The reconnect state machine still gave up permanently
  after 5 consecutive GAP-level connect failures, contradicting the step-4-documented "bounded
  backoff then indefinite slow retry" policy — more consequential now that step 6 made the board
  scan/reconnect continuously and unattended. Fixed by giving connect failures the same two-phase
  exponential-then-flatten shape already used for runtime-auth failures (a new 30-second slow
  cadence, deliberately not reusing the auth path's 5-minute cadence, which exists to throttle
  repeated bad credentials rather than transient link loss). **Hardware-verified**: the full ramp
  (1s -> 2s -> 4s -> 8s -> 16s -> flattening to 30s indefinitely) was confirmed on real hardware by
  physically moving the Flipper out of range.
- **Backlogged, not fixed:** replacing the 30-second idle-timeout's disconnect-and-reconnect cycle
  with a heartbeat/keep-alive, so a genuinely-idle-but-healthy session doesn't visibly flicker its
  LED/screen every 30 seconds (cosmetic only — the reconnect itself always completes correctly).
  This is a wire-protocol change needing its own design session, not a quick patch, and the user
  explicitly asked to backlog it rather than fix the cosmetic symptom now.

## 2026-09-07: Full-repository code review, pre-step-7 fixes, and step 7 (capability registry)

### Code review and pre-step-7 convergence fixes

A full read-through of both firmwares' shared CBOR/framing/session-crypto code produced 24
findings (`docs/CODE_REVIEW_FINDINGS.md`), six of which were fixed immediately (via
`docs/CODE_REVIEW_FIX_PLAN.md`) because they sit directly under the payload schemas and
protected-record path step 7 was about to build on. Headline finding: a real out-of-bounds read in
the Flipper's `feb_cbor_skip_value()` (missing a bounds guard the ESP32 side already had),
reachable from any malformed pre-authentication write. The two firmwares' `feb_cbor_skip_value()`
implementations had also drifted to accept different CBOR value types — root cause: the function
had zero direct test coverage on either side — and were converged to a single allowed set
(uint/bytes/text/array/map), with `docs/PROTOCOL.md` gaining the previously-unstated rule spelling
this out. Four of the six fixes were Flipper-only, since in most divergences the ESP32's copy was
already the stricter, correct one.

All six fixes were verified via new shared test vectors and, afterward, a dedicated hardware
re-verification pass (both the runtime-auth path and a fresh pairing ceremony) confirmed no
regression on real hardware.

### Step 7: board identity and capability registry

A design session scoped step 7 narrowly: only the `capability_query`/`capability_response`
plumbing (reporting `board`/`firmware`/`features` after runtime auth), explicitly deferring the
generic `command`/`status` message handling and any real `wifi_scan` handler to a separate
follow-on step. Key decisions: the Flipper queries capabilities exactly once per `board_id` (on
first successful runtime auth, if no cache file exists yet) and never automatically re-queries — a
deliberate, explicit exception to the project's usual re-verify-every-session pattern, justified
because a capability list is non-sensitive cached metadata, not a credential; the only refresh path
is a full unpair+re-pair. `board`/`firmware` are hand-maintained constant strings; `features` is a
hardcoded compile-time list (`["wifi_scan"]` at the time) with no runtime hardware-detection
framework yet.

Both firmwares implemented in parallel directly against the already-fully-specified wire shape (no
contract-freeze step needed). This is the first code on either side that decrypts a real protected
(post-auth) record at all. A regression from the established `BleEventWorker`-stack-budget bug
class was caught and fixed during implementation (new `AppEvent` capability fields pushed a couple
of stack-local structs over budget; converted to `static`, matching the by-now-standard
mitigation). **Hardware-verified**: a brand-new capability-cache file appearing on the Flipper (in
a directory that didn't exist before this session) is direct proof the exchange completed, decoded,
and persisted correctly, and the user visually confirmed the physical screen renders the new
capability line.

## 2026-09-07: wifi_scan capability implemented and hardware-verified

The first real capability command. A design session fixed the wire shape: manual "Scan now"
trigger only, results shown in a new scrollable on-device view (nothing persisted — that's
wardriving's future job), capped at the 32 strongest APs by RSSI. Two real wire-format gaps were
closed: SSID must be a CBOR byte string (not text — real SSIDs aren't guaranteed valid UTF-8, and
the codec has no UTF-8 validation on text fields), and RSSI must be an unsigned `+128`-offset
integer, since the canonical-CBOR rules already forbid negative integers in a payload map.

Both firmwares implemented against the frozen contract and independently discovered and converged
on the same fix for a genuine nesting-depth gap (`status.result`'s three real container levels
would otherwise exceed the shared nesting-depth limit — resolved by giving `command.arguments`/
`status.result` their own fresh depth budget). A `-fstack-usage` measurement against the real
compiler flags (prompted by this project's now-recurring `BleEventWorker` stack-overflow bug class)
found the Flipper's worst-case CBOR-skip recursion left only ~20% stack headroom, below this
project's 30% bar — fixed by moving a per-recursion-level scratch array to indexed static storage,
restoring headroom to ~50%.

The first hardware test found two more real bugs: a Flipper-side send-buffer size miscalculation
(fixed — it had failed on literally every attempt, deterministically, since the size comment forgot
to count the CBOR map's own key strings) and, once that was fixed, **a new ESP32-side stack
overflow**: the `nimble_host` FreeRTOS task (a tight ~4 KB budget) crashed with a stack-protection
fault a few seconds into every scan, because the batch-send function held its entire per-scan
working set (two 32-entry result arrays plus scratch buffers) as stack-locals. Fixed with the same
static-storage pattern used everywhere else in this project for this bug class. After the fix:
three consecutive scans, zero crashes (previously 2/2 crashed). A follow-up measurement confirmed
the ESP32's own CBOR-skip recursion has ample headroom (66%) and needed no fix. The user visually
confirmed the results screen renders correctly on real hardware, closing out wifi_scan's hardware
verification (one accepted gap: no non-ASCII SSID was available nearby to exercise that specific
render path on real hardware, though it's covered by host-native tests).

## Current project state and handoff

As of 2026-09-07 (commit `63ec936`, "Through wifi_scan: step 7 capability registry and wifi_scan
capability, hardware-verified"): Phase 2 (core BLE transport through authenticated runtime
sessions) is complete, and Phase 3 (production-ready wardriving) is underway. Steps 1 through 7 are
implemented and hardware-verified, including the follow-on `wifi_scan` capability. See
`docs/SESSION_MEMORY.md` for exactly what's next and any open backlog items, and `docs/PLAN.md` for
the full roadmap and per-step "done when" criteria.

Preserve these constraints going forward:

- Keep the pinned Unleashed release/API and ESP-IDF version (`docs/BASELINES.md`).
- Keep ESP-IDF target `esp32c6` and the verified 4 MB flash configuration unless hardware changes.
- Keep Flipper as BLE peripheral/GATT server and ESP32-C6 as central/GATT client.
- Preserve the reset-gated pairing, X25519, HKDF-SHA-256, HMAC-SHA-256, **AES-256-GCM** (revised
  from AES-128-GCM during step 6, since the Flipper's hardware AES-GCM primitive is hardcoded to a
  256-bit key), sequence/replay protection, and the explicit app-owned/physical-access-accepted
  persistence threat boundary.
- Avoid flashing or erasing either board unless explicitly requested.
- This project has hit the same `BleEventWorker`/task-stack-overflow bug class repeatedly (steps
  3, 5, 7, and wifi_scan) — any new BLE-callback-path code on either firmware should default to
  file-scope `static` storage for non-trivial buffers, per the established convention, and get a
  real `-fstack-usage` check before being trusted at a tight budget.

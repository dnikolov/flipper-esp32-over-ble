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

## 2026-09-08: ble_scan capability implemented and hardware-verified

The second real capability, a passive BLE advertisement scan manually triggered from the Flipper
(mirroring the existing `wifi_scan` "Scan now" pattern). Implementation matched the frozen wire
protocol from [PROTOCOL.md](PROTOCOL.md)'s "`ble_scan` command and status payloads" section.

### Implementation and hardware verification

Both firmwares implemented against the frozen contract: ESP32 (`esp32/main/main.c`)'s
`handle_ble_scan_command`, `ble_scan_send_next_batch`, `ble_scan_window_close_cb`, etc.; Flipper
(`flipper/flipper_esp32_over_ble.c`)'s `send_ble_scan_command`, `draw_ble_scan_results`, results
view. Both `cbor_codec` implementations (`esp32/main/cbor_codec.c/.h`, `flipper/cbor_codec.c/.h`)
gained the `ble_scan` command/status/result CBOR shapes. Host-side tests (`tests/esp32/test_framing_cbor.c`,
`tests/flipper/test_flipper_codec.c`, `tests/vectors/`) were extended to cover them. A new
`esp32/main/location.c` / `esp32/main/location.h` provides the fixed-coordinate GPS-stub
`location_get_fix()` interface per the "`ble_scan`, `wardriving`, and the GPS-stub reorder"
design — groundwork for the future `wardriving` capability but not itself wired into any
capability yet. Its host tests (`tests/esp32/test_location.c`) are included.

The Flipper's capability cache is queried only once ever per board (a pre-existing,
already-documented limitation) — since it was cached before `ble_scan` existed, the Flipper
initially showed a stale wifi_scan-only capability list and the Right button did nothing.
Deleting the stale cache file and relaunching forced a fresh `capability_response` including
`ble_scan`. After that: runtime session re-authenticated cleanly, the Right-button trigger sent
the `ble_scan` command, the ESP32 logged `ble_scan started (request_id=1)` then completed with
`sending ble_scan status (complete, 9 device(s) this batch)`, no crashes/reboots on either device.
The user then visually confirmed on the physical Flipper screen that the 9 results rendered
correctly (address, name, RSSI, address type, scrollable). This capability-cache staleness is
expected/already-documented behavior, not a new bug — the existing USER_GUIDE.md already describes
it as a known limitation.

Per existing design decision: **Left triggers `wifi_scan`, Right triggers `ble_scan`** on the main
screen when the connected board's capability line advertises it.

## 2026-09-08: Codebase and agent cost-efficiency pass

A review of file sizes/read-cost across the codebase and the two developer subagents (folded
here 2026-09-11 from the now-retired `docs/OPTIMIZATION.md`; still-open items from that review
moved to [BACKLOG.md](BACKLOG.md)).

- **Wardriving codec shape reconciled** between `esp32/main/cbor_codec.h`/`.c` and
  `flipper/cbor_codec.h`/`.c` — a real structural divergence (tagged union vs. two
  always-present named fields; two presence flags vs. one), not just cosmetic. See
  [LESSONS.md#wardriving-struct-shape-divergence](LESSONS.md#wardriving-struct-shape-divergence).
- **`tools/check_shared_headers.py` added** — diffs macro values and function prototypes between
  each esp32/flipper header pair. Does not catch struct-body shape divergence (see above); still
  read the actual struct on both sides for any new composite/optional-field/union shape.
- **Both agent files restructured**: narrative/incident writeups moved to `docs/LESSONS.md`,
  agent files cut to imperative rules + links (`esp32-developer.md` ~4.7k→2.7k tokens,
  `flipper-developer.md` ~6.3k→4.0k tokens). Near-duplicate sections shared between the two
  agent files were merged into one `LESSONS.md` entry instead of two copies. A read-discipline
  rule was added to both: grep for the symbol first, then `Read` with `offset`/`limit`, for any
  file over ~800 lines.
- **Split `cbor_codec.c`/`.h` per capability** on both firmwares: gone, replaced by
  `cbor_primitives.c`/`.h`, `cbor_records.c`/`.h`, `cbor_wifi_scan.c`/`.h`, `cbor_ble_scan.c`/`.h`,
  `cbor_wardriving.c`/`.h`, plus a small `cbor_internal.h` per side for implementation-only
  shared macros. Each `cbor_codec.h` is now a thin umbrella `#include`-ing the five split
  headers, so nothing that already included it needed to change.
  `esp32/main/CMakeLists.txt` and three `tests/esp32/build*.ps1` scripts (all three link the
  codec, not just one) were updated to list the five new files. All host-native suites and a
  full clean `idf.py build` pass with zero new warnings; `tools/check_shared_headers.py` reports
  `OK` for the split umbrella on both sides.
  **Orchestrator follow-up:** adding the five new split-header pairs to
  `check_shared_headers.py`'s `HEADER_PAIRS` surfaced a pre-existing bug in the script's own
  macro-value regex (`\s*` between a macro name and its value matched newlines, so a bare
  include-guard `#define X` followed by a blank line swallowed the *next* line's content as the
  guard's fake "value"). Fixed (`\s*` → `[ \t]*`); re-run reports `OK` on all 11 header pairs.
  Also found and fixed: `tests/flipper/build_pairing.ps1` and `build_session.ps1` still
  hardcoded the deleted `cbor_codec.c` (missed by the flipper-developer agent's task, which only
  named `build.ps1`) — both updated and re-verified passing (67/67, 57/57).
- **Normalized the ESP32 build invocation**: added `tools/build_esp32.ps1` (clears `MSYSTEM`,
  sources `export.ps1`, runs `idf.py build`, tails output). `.claude/settings.json`'s three
  near-duplicate `idf.py build` allow entries collapsed into one entry for this script.

## 2026-09-11: Wardriving dedup distance-threshold fix, AES-GCM sequence cap enforced, BLE active scanning enabled

Three small, independent fixes landed the same session (folded here 2026-09-11 from the
now-retired `docs/FINDINGS_BACKLOG.md`, which tracked them ad hoc as they landed):

- **Wardriving location-dedup threshold was 1000x too large** (`3111fa2`). The 2026-09-10
  dedup module's own comment math was correct (30m ≈ 2700 units at 1e7-scaled lat/lon), but
  `wardriving_dedup.c`'s `should_log_record()` compared against `3000000ULL` instead of `2700ULL`
  — requiring ~33km of movement before logging a new position instead of the intended 30m.
  Found independently by this session and by Gemini's review (BUG-02); fixed by correcting the
  constant.
- **AES-GCM 24-bit sequence cap now enforced on both firmwares** (`3111fa2`). PROTOCOL.md's
  nonce construction only encodes the low 24 bits of the sequence counter, so a direction's
  sequence reaching `2^24 - 1` would repeat a nonce under the same session key — a GCM
  catastrophic failure — but nothing checked for it. Added `FEB_SESSION_SEQUENCE_MAX =
  0xFFFFFFu` on both sides; `queue_and_send_protected()` (ESP32) and each Flipper command sender
  now refuse to encrypt/send at the cap and terminate the connection, and both receive paths
  reject/close on an incoming sequence at or past it. Three of four independent reviews
  (Copilot, Gemini, Grok) converged on this as their top-priority finding.
- **BLE active scanning enabled for `ble_scan`** (`e92aad9`). Changed from passive
  (`params.passive = 1`) to active scanning: active scanning sends scan requests, and devices
  often include their full name only in the scan-response data passive scanning never sees.
  Expected cost: ~10-20% latency per device. Not yet extended to `wardriving`'s own capture
  engine, and no runtime on/off toggle yet — see [BACKLOG.md](BACKLOG.md).

## 2026-09-09: `wardriving` implemented on both firmwares (build/host-test-verified, hardware pending)

The composite capability tying `wifi_scan`/`ble_scan` into an autonomous capture loop with
flash-backed persistence, per the "`ble_scan`, `wardriving`, and the GPS-stub reorder" design
in [PLAN.md](PLAN.md).

**ESP32 side:** `handle_wardriving_command()` (start/stop, full field-presence/bounds
validation, a bidirectional busy guard shared with manual `wifi_scan`/`ble_scan` via the same
`wifi_scan_in_progress`/`ble_scan_in_progress` flags), an autonomous interval/window-driven
Wi-Fi/BLE capture engine reusing `wifi_scan_done_handler()`/`ble_scan_catalog_advertisement()`'s
collection logic and the existing merged-reconnect-scan event path (no second dedicated scan
runs), a checksummed append-only circular log on a new `wardrive` raw-flash partition
(`esp32/main/wardriving_log.c`/`wardriving_record_format.c`, sector-generation-ordered FIFO
eviction, a per-record "undrained" flag cleared via a single crash-safe 1→0 flash write instead
of a separately-persisted head/tail pointer), and unsolicited backlog-drain-on-session-establish.
`idf.py build` is clean; a new host-native suite (`tests/esp32/test_wardriving_log.c`) covers the
flash log's checksum/header-packing/eviction-ordering logic and the interval-field
validation/default-substitution rule (`wardriving_resolve_start_intervals()`,
`esp32/main/wardriving_validate.h`/`.c`).

One real interpretation ambiguity was resolved during implementation: whether a requested
source's interval field(s) may be omitted on `start`. Resolved in favor of
[PROTOCOL.md](PROTOCOL.md)'s "Interval bounds and defaults" section (which states the
point-4/most-aggressive default is substituted on omission — `wifi_interval_ms=0` i.e.
continuous, `ble_window_ms=30`, `ble_interval_ms=30`) over the field table's terser "required
when X in sources" wording, confirmed correct by cross-checking the Flipper's
`send_wardriving_start_command()`, which always omits these fields (no interval-entry UI in v1)
and relies on exactly this ESP32-side default. PROTOCOL.md itself is unambiguous on this point
going forward; this paragraph exists only to record how the ambiguity was found and closed.

**Flipper side:** a one-tap start/stop control/status screen (`AppScreenWardriving`, reachable
via Up from the main screen when the board advertises `wardriving`; no source-picker or
interval-entry UI, per the v1 design decision — requested `sources` are derived from whichever
of `wifi_scan`/`ble_scan` the board actually advertises, and interval fields are omitted so the
ESP32 applies its own documented default); `status` dispatch now routes by the decoded `state`
string (wifi_scan/ble_scan's `partial`/`complete` vs. wardriving's `started`/`data`/`stopped`),
correctly accepting an unsolicited `status(state="data", request_id=0)` backlog-drain record
without treating it as unmatched; and incremental WiGLE CSV export
(`flipper/wardriving_csv.c`/`.h`, a pure/host-testable formatting module separate from the
Furi-dependent file-I/O in `flipper_esp32_over_ble.c`) writing one timestamped file per
connected session, appended record-by-record (never buffered in RAM), with `FirstSeen`
timestamps reconstructed by anchoring the newest-seen record to the Flipper's wall clock and
backdating the rest from their reported boot-relative delta. `fbt.cmd fap_flipper_esp32_over_ble`
is clean; `tests/flipper/test_flipper_codec.c` covers the CSV row/header formatting and the
timestamp-backdating arithmetic.

**ESP32-side design decisions made where the docs left specifics open** (full rationale comments
in `esp32/main/wardriving_record_format.h`/`wardriving_log.c`/`main.c`'s
`handle_wardriving_command()`):

- `wardrive` partition: custom "data" subtype `0x40`, offset `0x190000`, size `0x270000` (624 x
  4096-byte sectors, exactly using the rest of this board's verified 4 MB flash after `factory`).
- On-flash record checksum: CRC-32/ISO-HDLC ("zlib" variant), over the encoded CBOR payload only
  (not the record header).
- `ble_window_ms <= ble_interval_ms` is enforced as a structural sanity check beyond
  [PROTOCOL.md](PROTOCOL.md)'s literal text, and a duplicate `sources` entry (e.g.
  `["wifi","wifi"]`) is rejected `invalid_command` — neither is a PROTOCOL.md sentence, both are
  judgment calls.
- A persistent flash-write failure (`FEB_WARDRIVING_FLASH_FAILURE_LIMIT` = 3 consecutive
  `wardriving_log_append()` failures) triggers an unprompted self-stop with an accompanying
  `error`/`status(state="stopped")` pair, per PROTOCOL.md's state-model table.

**Flipper-side stack risk, only partially measured.** While implementing WiGLE CSV export,
large formatter locals reachable from `BleEventWorker` (`handle_wardriving_status()` ->
`wardriving_csv_write_record()` -> `feb_wardriving_csv_format_row()` -> `csv_write_field()`)
were proactively converted to file-scope `static` per this project's established convention
(`docs/LESSONS.md#ble-event-worker-stack-budget`) — a fifth instance of the same bug class this
project has hit four times as an actual hardware crash (steps 3, 5, 7, wifi_scan), this time
caught by convention before it became one. What remains **unmeasured**: the full reachable
chain's real stack cost, since it also passes through the reused
`feb_cbor_decode_wardriving_record()` decode path (not written for this feature, inherited as
is). No `-fstack-usage` check was run — do this before or during hardware verification, not
after a crash.

**Not yet exercised on real hardware** on either side — no forced-disconnect test of the
merged-reconnect-scan path under live wardriving BLE capture, no extended unattended run
validating flash-log wraparound/power-loss on the physical board, and no on-device confirmation
of the Flipper's control screen, backlog-drain/live-data dispatch, or exported CSV contents
against a real capture.

## 2026-09-10: wardriving hardware-verified (stack overflow + GATT-write-flood/EBUSY bugs found and fixed)

### Hardware verification attempt

The first real-device test of the `wardriving` capability implemented 2026-09-09 was launched
with `sources=[wifi,ble]` and live serial monitoring (`idf_monitor.py --port COM9 --timestamps`).
The test ran a steady capture session (~24 seconds, ~4 records per batch, backlog growing from 0
to ~185 as collection outpaced send rate), then a clean user-initiated stop. Two real bugs
surfaced during this run, found and fixed in the same session before a re-verification pass.

### Bug 1 — `nimble_host` task stack overflow (recurring class)

**Symptom:** starting wardriving crashed with `Guru Meditation Error: Core 0 panic'ed (Stack
protection fault)` in FreeRTOS task `"nimble_host"`. The crash wrote one record to the flash log
before failing, which meant every subsequent reboot re-triggered the documented
unsolicited-backlog-drain-on-session-establish behavior, hitting the same overflow and trapping
the board in a self-sustaining crash loop independent of further user action, until the
Flipper's BLE link was broken.

**Root cause:** `wardriving_send_next_batch()` in `esp32/main/main.c` declared its trial-encode
scratch variable (`feb_wardriving_status_result_payload_t trial`, which embeds a 32-entry record
array) as a plain stack-local — measured via `-fstack-usage` at 2608 bytes, ~64% of `nimble_host`'s
4096-byte task stack (`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096`). Its sibling function
`ble_scan_send_next_batch()` already had this exact pattern fixed (declared `static`);
`wardriving_send_next_batch()` was simply never converged with its already-fixed twin. This is
the **fifth** instance of the recurring `nimble_host`/task-stack-overflow bug class (previously
steps 3, 5, 7, and `wifi_scan` — see `docs/LESSONS.md#nimble-host-stack-budget` for the
structural problem).

**Fix:** moved `trial` to file-scope `static`, matching `ble_scan_send_next_batch()`. Re-measured
via `-fstack-usage`: 2608 → 32 bytes.

**Confirmed NOT a recursion/re-entrancy bug:** all `ble_gap_disc()` re-arms route through
`ble_npl_callout_reset()` onto the NimBLE host's own event queue, not synchronous recursive
calls (verified against ESP-IDF's `components/bt/porting/npl/freertos/src/npl_os_freertos.c`).

### Bug 2 — GATT-write flood + scan-restart collision (latent pattern, wardriving-triggered)

**Symptom:** with the stack-overflow fix flashed and confirmed fixed, starting wardriving no
longer crashed, but instead: a burst of `sending wardriving status(data)` GATT writes fired
back-to-back, then `pairing fragment 1 write failed: 6` (NimBLE `BLE_HS_ENOMEM`), several more
`GATT write failed: 7` (`BLE_HS_ENOTCONN`), then `disconnected: reason=534`, then on reconnect:
`wardriving: re-trigger ble_gap_disc failed: 15` (`BLE_HS_EBUSY`) and `wardriving self-stopped
(internal_error)`. The board recovered and reconnected fine afterward; wardriving just stopped
itself and required a restart.

**Root cause 1 (the write flood):** `wardriving_send_next_batch()` cleared its
`wardriving_tx_in_flight` flag (and called `wardriving_log_mark_drained()`) as soon as a
record's **first** BLE fragment was handed to NimBLE via `queue_and_send_protected()`, not after
the whole record was actually delivered — only `write_complete()`'s `tx_done_action` dispatch
confirms full delivery of all fragments. `wifi_scan`/`ble_scan` have the byte-identical pattern
but never manifested it, because nothing re-triggers a new send except a slow, user-initiated
`command`. `wardriving`'s own capture-completion timers fire automatically every
`wardriving_ble_window_ms`/`wardriving_wifi_interval_ms` (as low as 30 ms at the default
"point-4" cadence), so a new batch could be kicked off before the previous record's tail
fragments were still in flight, clobbering the single shared `tx_fragment_*` state with a
concurrent send — an **actual violation of `docs/PROTOCOL.md#fragmentation`'s
single-in-flight invariant**, not just a config-tuning issue. A mid-record write failure also
silently lost records: marking "drained" before delivery was confirmed meant that record was
discarded forever if any fragment failed.

**Root cause 2 (the EBUSY/self-stop):** the reconnect-scan restart was correctly gated on
`!wardriving_ble_active` in one of its three call sites (`BLE_GAP_EVENT_DISC_COMPLETE`) per the
existing docs/PLAN.md "Revised long-run reconnect policy" (the "merged-reconnect-scan" invariant
— never run a dedicated reconnect scan while wardriving's BLE source owns discovery), but NOT in
the other two (`BLE_GAP_EVENT_DISCONNECT`'s reconnect branches, and `reconnect_task()`). A
disconnect during active wardriving capture raced two `ble_gap_disc()` calls, one losing with
`EBUSY` — which wardriving's then-unconditional handling treated as fatal and self-stopped on.

**Fix:** (1) `wardriving_send_next_batch()` now always chains through a `TX_DONE_CONTINUE_WARDRIVING`
path (never clears in-flight/marks-drained early) — a new file-scope `wardriving_pending_drain_count`
tracks what to mark drained, applied only once `write_complete()` confirms full delivery, reset on
`BLE_GAP_EVENT_CONNECT`. (2) Moved the `!wardriving_ble_active` guard inside `start_scan()`
itself so all three callers are covered by construction, and made `wardriving_ble_interval_cb()`
retry after a short delay on `BLE_HS_EBUSY` specifically (a connect attempt from `gap_event()`'s
own merged-reconnect match can transiently own GAP master state), instead of self-stopping
outright per the general principle that `BLE_HS_EBUSY` from a resource genuinely owned by *this
firmware's own code* is a coordination bug to fix, not a peer condition to surrender on.

A related `docs/LESSONS.md` entry already existed for this pattern (`wardriving-tx-in-flight-cleared-before-delivery-confirmed`);
the stack-overflow bug (Bug 1 above) is a **new recurring class** now documented.

### Hardware re-verification pass

Both fixes were flashed together, and a full retry was run: `idf_monitor.py --port COM9` live
capture, started wardriving with `sources=[wifi,ble]` (request_id=1), ran ~24 seconds sending
steady batches of ~4 records at a time (backlog draining progressively, growing from 0 to ~185),
then user-initiated exit with `wardriving stopped (request_id=2)` and normal disconnect. **Zero
crashes, zero `GATT write failed`, zero `EBUSY`/self-stop during this run. Both bugs confirmed
fixed on real hardware.**

### Still open (NOT yet "done when" per PLAN.md)

Three items remain before wardriving's "done when" bar is fully met (forced-disconnect test
under live BLE capture, extended unattended flash-log wraparound/power-loss run, and CSV export
SD-card confirmation) — see `docs/SESSION_MEMORY.md`'s "Known open items" for current status.

## 2026-09-10: wardriving BLE duty-cycle starvation (found and fixed)

While reproducing one of the three still-open items above (a forced-disconnect scenario under
live wardriving BLE capture), a new, distinct bug surfaced: pressing OK on the Flipper's
wardriving screen to send `stop` appeared to do nothing, the connection would drop on its own
every ~30-40 seconds, and it would auto-reconnect but immediately repeat the same cycle.

**Root cause (confirmed via a live `idf_monitor.py --port COM9` capture):** wardriving's default
cadence (`ble_window_ms=30, ble_interval_ms=30`, i.e. 100% BLE observer duty — step 4's
"point-4"/most-aggressive setting, `docs/PLAN.md`) leaves the active BLE connection no
serviceable airtime once real application traffic (not step 4's synthetic load) is on it. The
capture showed, immediately after `wardriving started`: a continuous stream of
`GAP procedure initiated: discovery` (a fresh scan restart every ~20-30ms) for ~48 straight
seconds with **zero further GATT writes logged** — the Flipper's `stop` command never landed —
ending in `disconnected: reason=534` (NimBLE's `BLE_HS_ERR_HCI_BASE` encoding of HCI 0x16,
"Connection Terminated By Local Host": the ESP32 gave up on its own connection, not the peer).
It auto-reconnected within seconds each time (the merged-reconnect-scan mechanism itself works
correctly), then immediately repeated the same storm-then-disconnect cycle.

This is exactly the condition `docs/PLAN.md`'s backlog had flagged and deferred pending evidence
("only build [a pause-on-degradation fallback] if real wardriving traffic shows concurrent
operation is unstable — step 4's sweep found it stable under synthetic load"). Real traffic does
show it, on real hardware.

**Fix:** rather than building the degradation-detection/pause-resume mechanism the backlog item
proposed, the user chose the simpler fix — raise `FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS` from
30ms to 500ms (`esp32/main/wardriving_validate.h`), keeping `ble_window_ms` at its 30ms default.
That's ~6% BLE observer duty instead of 100%, giving the connection real, regular gaps to be
serviced. Bounds are unchanged (`ble_interval_ms` ∈ [30, 1000]ms), so 500ms is a same-session
choice available to any client, not a new bound. `wifi_interval_ms`'s default (0, continuous) was
deliberately left unchanged — WiFi scanning was active in the same reproduction (`sources=[wifi,
ble]`) and is suspected to independently compete for the ESP32-C6's single shared 2.4GHz radio via
IDF's coexistence arbiter, but that hasn't been isolated/validated the way step 4 isolated the BLE
points, so it's tracked separately in `docs/PLAN.md`'s Backlog rather than guessed at here.

Host tests (`tests/esp32/build_wardriving.ps1`) pass unchanged — the interval-resolution tests
assert against the named default constants, not hardcoded values, so no test needed updating for
the new number itself; only a few comments referencing the old "point-4 default" were corrected
for accuracy. Reflashed and hardware re-verified same session: the BLE discovery-restart storm
is gone (no more 20-30ms `GAP procedure initiated: discovery` spam), and the connection auto-
reconnects cleanly. A second, distinct issue surfaced during that same re-verification pass — see
below.

## 2026-09-10: wardriving CSV export writes a row per observation, not per unique device

While re-verifying the BLE duty-cycle fix above, the user separately noticed the exported WiGLE
CSV has one row per drained observation, so a stationary device seen repeatedly accumulates many
near-identical rows. Checked this against the actual WiGLE ecosystem convention first (official
CSV spec at `api.wigle.net/csvFormat.html`, a real sample export, Kismet's `wiglecsv` docs): this
is correct, standard behavior, not a bug — WiGLE CSV is one row per observation by design,
`FirstSeen` means "this row's timestamp," not "first time this network was ever seen." Still,
the user wanted repeats collapsed for practical file-size/readability reasons, so this became a
feature request rather than a bug fix.

**Research:** surveyed three prior projects for how they handle this. `bettercap` (MIT) —
`modules/wifi/wifi_recon.go`'s `Session.WiFi.AddIfNew()`: a keyed map, updated in place on a
repeat sighting rather than appended to. `wardriver_rev3` (GPL-3.0, code not reusable but the
*pattern* isn't copyrightable) — a fixed 512-entry MAC-history ring buffer, skip if the address
is still in the recent window, evict-oldest when full. An unlicensed Hak5-payload project,
`pineapple_pager_wdgwars` (no LICENSE file found — treated as inspiration only, not reusable) —
the most refined policy: write a row if the BSSID is new this session, OR moved >= 30m, OR RSSI
improved >= 6dB, OR >= 300s elapsed, plus "no GPS fix, no row."

**Design decision (with the user):** implemented independently in `flipper/wardriving_csv.c`/
`.h` (`feb_wardriving_dedup_table_t`/`feb_wardriving_dedup_should_write()`), not the ESP32 side —
the flash log and wire protocol stay a complete, honest observation-by-observation record; only
the CSV export layer (the one point WiGLE is actually the consumer) collapses repeats. Adopted
pineapple_pager_wdgwars's OR-gate shape but with two changes from the user's explicit steer:
table capacity set to 256, not wardriver_rev3's 512 (a Flipper app has much less free RAM to
spare than either reference project's dedicated hardware) — the "conservative window" choice;
and the time-elapsed clause dropped entirely, not adopted at any threshold — with the current
fixed-coordinate GPS stub, a row written only because 300s passed would be identical in every
field except `FirstSeen`, which is exactly the kind of duplicate this exists to remove. Final
policy: write if the address is new this session, OR RSSI improved by >= 6dB, OR moved >= 30m.
The move clause is real, working code, just inert until real GPS lands (distance from a fixed
coordinate to itself is always 0) — a deliberate no-op today, not a stub to fill in later.

**Verification:** new host tests (`tests/flipper/test_flipper_codec.c`'s `test_wardriving_dedup()`/
`test_wardriving_dedup_eviction()`) cover new-address-always-written, sub-threshold RSSI repeat
skipped, at-threshold RSSI repeat written, sub-threshold movement skipped, at-threshold movement
written, a second distinct address tracked independently, the same 6 bytes under a different
`payload_kind` (wifi vs ble) NOT treated as a repeat, and ring-buffer eviction correctness at
exactly capacity + 1. All 479 host-test checks pass (`tests/flipper/build.ps1`). Also verified
against the real FBT toolchain (`fbt.cmd fap_flipper_esp32_over_ble`, build only, no flash) since
this was the first use of `<math.h>` (`cos`/`sqrt`, for the movement distance check) anywhere in
this codebase — linked and built cleanly with no `application.fam` changes needed. Not yet
hardware-verified (no flash/live capture performed for this change as of this writing).

## 2026-09-10: idle-timeout outbound-activity bug fixed and hardware-verified; new start/backlog-drain collision found (known issue, not fixed)

### Symptom and reproduction

During a live wardriving hardware test (using the freshly-rebuilt Flipper FAP with the CSV
dedup fix from the entry above), the ESP32 disconnected a few seconds into every run, with the
Flipper falling back to its "waiting for esp32" screen. A live serial monitor capture showed no
panic or backtrace — a clean, deliberate self-disconnect logged by the firmware itself
(`idle authenticated connection (30550 ms without a record); terminating`), not a crash.

### Root cause

`last_record_activity_ms` (`esp32/main/main.c:258`), the 30-second idle-timeout's reference
clock, was only updated on inbound records received from the Flipper (`BLE_GAP_EVENT_CONNECT`
and the inbound reassembly path) — never on outbound sends. Wardriving's traffic is
one-directional after the initial `start` command: the ESP32 pushes `status(data)` batches
continuously with nothing coming back, so the idle-timeout fired exactly 30s after the last
inbound record even while the connection was actively transferring useful data outbound.
`docs/PROTOCOL.md`'s own spec text ("closes an idle connection after 30 seconds without a
record") is already undirected/correct — only the ESP32's implementation had narrowed it to
"received." This is a new, fourth failure pattern distinct from the three bug classes already
in this file's other 2026-09-10 entries (nimble_host stack overflow, GATT-write-flood/EBUSY
collision, BLE duty-cycle starvation).

### Fix

`write_complete()` (`esp32/main/main.c`, the single callback every outbound fragment funnels
through) now stamps `last_record_activity_ms` immediately after a record's last fragment is
confirmed sent, before dispatching `tx_done_action` — covering every outbound record type
(pairing, hello/client_auth, wifi_scan/ble_scan/wardriving status, errors, capability
responses) from one shared point rather than duplicating the stamp per call site.

### Verification

Built clean (`idf.py build`), flashed to the physical ESP32-C6 on COM9. Live-monitored:
confirmed 49+ seconds of continuous one-way outbound backlog-drain traffic survived with no
false idle-disconnect, where the same traffic pattern died at exactly 30s before the fix.
**Hardware-verified.**

### New known issue found during the same verification pass (not fixed — deferred at user's request)

While verifying the fix above, a fresh `start` command from the Flipper landed while the
automatic "unsolicited backlog drain" (`docs/PROTOCOL.md`) was already actively streaming a
large queued backlog (1278 records, left over from the earlier crash-loop). `handle_wardriving_
command()`'s `start` path acknowledges with a plain `send_protected()` call (`main.c` ~line
2247), which funnels through `queue_and_send_protected()` (`main.c` ~lines 957-974) — and that
function unconditionally overwrites the shared `tx_done_action`/fragment-send state with no
check of whether a wardriving batch send (`wardriving_tx_in_flight`) is already in progress.
The `start` ack's own completion then dispatches `TX_DONE_NONE`, permanently clobbering the
drain's `TX_DONE_CONTINUE_WARDRIVING` continuation — all further outbound sending silently
stops (BLE capture keeps running in the background, filling the flash log, but nothing more
transmits) until the now-correctly-working idle-timeout disconnects the connection 30 seconds
later.

This is the same general bug *class* as this file's GATT-write-flood/EBUSY entry above
(unguarded shared TX/fragment state), but a different, previously-unseen manifestation — it
only surfaces when an explicit `start` collides with an already-in-flight backlog drain, a
timing window none of the prior verification runs happened to hit.

**No data loss**: records are only marked "drained" in the flash log after a confirmed delivery
ack, so whatever was mid-transfer when the stall began remains undrained and is automatically
resent on the next reconnect's backlog drain.

**Known issue, not fixed** — deferred at the user's explicit request. Candidate fix (not
implemented): gate `handle_wardriving_command()`'s `start` acknowledgment behind the same
`wardriving_tx_in_flight` check the drain path itself uses, deferring/queuing the ack instead
of sending it unconditionally.

### Also found and fixed this session: the Flipper FBT build was not actually clean

Rebuilding the Flipper FAP (to get the CSV-dedup fix from the entry above onto physical
hardware) surfaced a real build failure that host-native tests couldn't catch:
`flipper/wardriving_csv.h`'s `FEB_WARDRIVING_DEDUP_MOVE_METERS` macro was a bare `30.0` literal,
which the FBT ARM toolchain's `-fsingle-precision-constant` flag treats as `float` — tripping
`-Werror=double-promotion` against the `double`-returning distance function that consumes it at
`wardriving_csv.c:274`, despite that same file already documenting this exact trap in a comment
at lines 236-240. Fixed with an explicit `(double)` cast, matching the file's own established
convention elsewhere. Host tests unaffected (still 479/479 — the host toolchain doesn't set
that flag); the real-FBT build is now clean. **This corrects the previous entry above's claim
of "a clean real-FBT build" for the CSV dedup fix — that claim was inaccurate as committed; it
is accurate only as of this correction.**

## 2026-09-10: BLE duty-cycle fix left wardriving nearly blind to BLE devices; window widened (verification pending)

### Symptom

After the BLE duty-cycle-starvation fix above, a short live wardriving test produced a CSV with
WiFi records but **zero BLE records**, even though the ESP32 had been asked to capture both
sources.

### Root cause

Not a bug in the CSV writer or status dispatch (both `flipper/wardriving_csv.c` and
`flipper_esp32_over_ble.c`'s `handle_wardriving_status()` treat WiFi and BLE symmetrically, and
the shared GPS-fix gate in `esp32/main/main.c` is identical and satisfied for both sources). The
actual cause: the duty-cycle fix raised `ble_interval_ms`'s default to 500ms but left
`ble_window_ms` at 30ms (`esp32/main/wardriving_validate.h`), dropping BLE scan duty from ~100%
to ~6% — a 30ms scan burst followed by 470ms of no BLE scanning at all, repeating. WiFi scanning
is unaffected (`wifi_interval_ms` stays 0/continuous), which is exactly why WiFi records kept
appearing while BLE didn't. Confirmed as a detection-probability artifact, not a deeper bug: the
user re-ran wardriving for a longer stretch and BLE records did show up.

### Fix (built and flashed; live verification still pending)

Raised `FEB_WARDRIVING_BLE_WINDOW_DEFAULT_MS` from 30 to 100 (`ble_interval_ms` left at 500),
taking duty from ~6% to ~20% — a bigger catch-window per burst without approaching the 100% duty
that caused the original starvation bug. Chosen empirically as a conservative step up from the
hardware-verified-safe 6% point, explicitly **not** based on step 4's synthetic-load coexistence
sweep ("10%-100% all proven stable") — that sweep used a throwaway test harness
(`esp32/coex_test/`) that never exercised real authenticated-session traffic, and already missed
the actual duty-starvation bug once; it isn't trustworthy evidence for picking a new value under
real load. `docs/PROTOCOL.md`, `docs/CAPABILITIES.md`, and `docs/PLAN.md` updated to match the
new default.

Build is clean and the fix is flashed to the physical ESP32-C6 as of this entry. **Not yet
live-verified**: the session ended before a multi-minute wardriving run could confirm (a) no
idle-timeout/duty-starvation-style disconnect at ~20% duty, (b) BLE records land reliably and
faster than at the old ~6%. This is the first thing to check in the next session.

## 2026-09-10: Wardriving ESP32 deduplication — reduce flash bloat and transfer time

**Root cause observed**: Users reported wardriving taking a very long time to transfer records
from ESP32 to Flipper. Investigation showed excessive duplicate observations: every WiFi/BLE scan
window logs the same devices again, creating massive flash logs (mostly redundant) and slow BLE
transfers.

**Solution**: Implement deduplication on the ESP32 side before logging to flash. This is
complementary to the Flipper's CSV-export dedup (which only helps the final output, not transfer
speed) and applies the same criteria to flash logging: **only log if new address, RSSI improved
≥6dB, or location moved ≥30m**.

### Implementation (wardriving_dedup.c/h)

- **128-slot hash table** indexed by 6-byte MAC address (XOR-fold modulo).
- Tracks per-address state: address, last RSSI offset, last lat/lon.
- Before logging any WiFi or BLE record, queries the table and applies dedup gate.
- Hash collisions (different address in same slot) silently evict the old entry — acceptable
  tradeoff given ~128 table slots and ~64-128 unique devices per capture type in typical
  wardriving range.
- Resets table on wardriving start/stop (no stale state between runs).

**Integration**:
- `wardriving_dedup_and_maybe_append()` called instead of `wardriving_log_append()` in both
  `wifi_scan_done_handler()` (line 1143) and `ble_scan_window_close_cb()` (line 1625).
- `wardriving_dedup_reset()` called at start of both `handle_wardriving_command()` stop path
  (line 2050) and start path (line 2184).
- Module designed as pure C (no ESP-IDF dependencies) for future host-side testing.

**Result**: Flash log now contains only "meaningful" observations per the dedup criteria. Measured
improvement will be validated by live wardriving run in next session, but expected: massive
reduction in transferred record count, hence BLE transfer time.

Build verified clean (commit 47f57ff). **Not yet hardware-tested**: dedup filtering behavior
will be confirmed by running wardriving on real hardware and comparing record counts before/after.

## 2026-09-11: Wardriving reconnect stall — passive BLE re-arm never catches the merged reconnect scan (investigation ongoing; first candidate fix did not hold)

**Symptom (live hardware session)**: start wardriving from the Flipper, then close the Flipper
FAP or otherwise let the BLE link drop while wardriving is running. Normal disconnects (no
wardriving running) reconnect within ~10ms in the monitor log (`disconnected` -> `found v2
peer, connecting`). With wardriving's BLE source active, the ESP32 instead restarted discovery
every ~500ms for 2+ minutes (250+ restarts) with `found v2 peer, connecting` never appearing —
the Flipper FAP stuck on "waiting for esp," recoverable only by stopping wardriving or
rebooting the board.

**Initial hypothesis, and why it was wrong**: the theory going in was that the Flipper's
128-bit service UUID lives in the scan-response PDU (plausible byte-budget argument: a 128-bit
UUID AD entry is 18 bytes, tight against ADV_IND's 31-byte legacy limit alongside flags/name),
making it invisible to wardriving's passive-only re-arm scan. Reading the actual Flipper GAP
source refuted this: `gap_advertise_start()`
(`docs/references/flipper-firmware/upstream/targets/f7/ble_glue/gap.c`) only programs scan
response data when `mfg_data_len > 0`, and `flipper/flipper_esp32_over_ble.c` never sets
`mfg_data`/`mfg_data_len` at all — no scan response is ever configured. The UUID
(`set_advertisment_service_uid()`) is written directly into the primary `ADV_IND` payload
handed to `aci_gap_set_discoverable()`, along with an empty name (`adv_name` is always two
null bytes). Total AD content is ~21 bytes, comfortably inside the legacy budget. The UUID is
visible to any scanner, passive or active.

**What actually distinguishes the working and broken paths**: `esp32/main/main.c`'s
`start_scan()` (the dedicated reconnect scan, proven reliable) has always scanned **active**.
`wardriving_ble_interval_cb()`'s periodic re-arm, and the `start`-time BLE-source scan config in
`handle_wardriving_command()`, scanned **passive**. Per `docs/PLAN.md`'s "Revised long-run
reconnect policy," `start_scan()` is a deliberate no-op whenever wardriving's BLE source owns
discovery — reconnect matching is supposed to piggyback on whichever scan pass is currently
running instead. That means wardriving's passive re-arm was the *only* scan pass available for
reconnect matching in that state, and it never once caught a match. This merged-reconnect
mechanism had been flagged as an untested gap since step 4 ("Accepted gap: the merged
reconnect-scan behavior was never exercised... zero disconnects occurred in the sweep" —
`docs/PLAN.md`) and carried into wardriving's step-9 "done when" bar as an explicit open item
("forced-disconnect test under live BLE capture," `docs/SESSION_MEMORY.md`). This session was
the first real exercise of it, and it failed outright.

The exact mechanism by which passive scanning fails to deliver a match here (duty-cycle
misalignment with the Flipper's advertising cadence vs. the BT/Wi-Fi coexistence arbiter
plausibly deprioritizing passive-only RX windows, something else in the closed-source
ESP32-C6 BLE controller) was not isolated — the closed-source controller blob and NimBLE's
scan-parameter defaulting (`ble_gap_disc_fill_dflts()`, identical for passive/active) gave no
static evidence of a mechanism, so the fix is validated by directly observed before/after
behavior, not a confirmed root mechanism. Worth an RF capture if this ever regresses.

**Fix**: switched both wardriving BLE scan configs (`wardriving_ble_interval_cb()` and the
`start`-path config in `handle_wardriving_command()`) from `params.passive = 1` to
`params.passive = 0`, matching `start_scan()`. `params.filter_duplicates = 0` was already set
on both (a controller-dup-filter concern unrelated to scan type, per the 2026-09-10 scan-stall
fix), so this does not reintroduce that earlier bug. See `docs/LESSONS.md`'s
"wardriving-passive-scan-reconnect-stall" entry for the generalizable lesson.

Build-verified clean (`idf.py build`). Hardware re-verification of the fix (forcing a disconnect
during live wardriving BLE capture and confirming a fast reconnect) is still pending — the
physical board was under a live monitor session at the time of this fix and was deliberately
not flashed or disturbed.

**2026-09-11 follow-up: retested live, and the fix above did not hold.** Same session, same
board: wardriving started (both sources — the v1 Flipper UI always requests every source the
board advertises), FAP closed, then over 70+ seconds and 130+ confirmed-active discovery
restarts, neither of `BLE_GAP_EVENT_DISC`'s two possible match outcomes (`found v2 peer,
connecting` / `pairing window closed; not connecting`) ever logged — refuting the
active-vs-passive theory outright, not just leaving its mechanism unconfirmed. Static
re-investigation this session ruled out `connection_handle` staleness, a `scan_record_matches()`
logic difference, and an EBUSY/never-actually-arming failure (all read directly against source,
not assumed). Leading unconfirmed suspect: Wi-Fi/BLE coexistence starvation from wardriving's
concurrent, gapless Wi-Fi source. Full investigation detail, and the next isolation step
(reproduce BLE-source-only, no Wi-Fi), is in `docs/LESSONS.md`'s
"wardriving-passive-scan-reconnect-stall" entry — **do not treat this bug as closed** based on
the "Fix" text above; see `docs/SESSION_MEMORY.md` for current status.

**2026-09-11, third capture: a claimed "wardriving-independent" reproduction did not hold up
against its own log.** A later session captured a fresh monitor log
(`esp32_monitor3.log`) and reported it as a bare, wardriving-free reconnect stall — three
idle-timeout disconnects, the first two reconnecting instantly, the third stalling forever
in the same 500 ms-restart pattern — and argued this ruled out the Wi-Fi coexistence
suspect above, since no wardriving was supposedly running. Reading the actual log
line-by-line refutes that framing: `wardriving started (request_id=3, wifi=1 ble=1)` is
logged at the 80965 ms mark (22:32:33), a full 66 seconds *before* the third idle-timeout
disconnect at 146615 ms (22:33:39), and no stop/self-stop for either source appears anywhere
in the rest of the ~185 s capture. The first two disconnects (34595 ms/22:31:47 and
66605 ms/22:32:19) genuinely predate that `wardriving started` line and did reconnect
instantly, as claimed — but the third, stalling one occurred entirely inside an active
`wifi=1 ble=1` wardriving run, not on the bare path.

Confirmed directly against `esp32/main/main.c`: `start_scan()` (the dedicated reconnect
scan used by the first two disconnects) is a no-op whenever `wardriving_ble_active` is
true (by design, since 2026-09-10 — see the entry above), so on the third disconnect
`BLE_GAP_EVENT_DISCONNECT`'s `DISCONNECT_REASON_NORMAL` branch called `start_scan()`,
which returned immediately, and the only discovery activity for the rest of the capture
was `wardriving_ble_interval_cb()`'s own 100 ms-window/500 ms-period re-arm (matching
`FEB_WARDRIVING_BLE_WINDOW_DEFAULT_MS`/`FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS` in
`esp32/main/wardriving_validate.h`) — the exact mechanism already under investigation
above, not a distinct bug in the plain reconnect path. `wifi_interval_ms` was still at its
gapless default (`FEB_WARDRIVING_WIFI_INTERVAL_DEFAULT_MS = 0`), so this capture is a third
data point *consistent with* the still-unconfirmed Wi-Fi/BLE coexistence-starvation
suspect, not evidence against it. The plain `start_scan()` path itself was not exercised at
all during the failing disconnect and remains unimplicated — both its exercises in this
capture (disconnects one and two) worked, as they always have.

**2026-09-11, fourth entry: per-source selection added to the Flipper UI specifically to run
the isolation test above.** Every prior capture in this investigation started wardriving with
both sources (`wifi=1 ble=1` — the v1 Flipper UI had no way to request a subset), so the
leading Wi-Fi/BLE coexistence-starvation suspect was never actually isolated from a bare
BLE-only run. `send_wardriving_start_command()` in `flipper/flipper_esp32_over_ble.c` now
builds `sources` from a user selection (`app->wardriving_use_wifi`/`wardriving_use_ble`,
toggled via Left/Right on the wardriving screen when the board advertises both `wifi_scan`
and `ble_scan`) instead of unconditionally including every source the board advertises; no
ESP32-side change was needed (`handle_wardriving_command()` already validated and honored
whichever subset of `sources` it was given). Build-verified only, not yet flashed — the next
step is to actually run the isolation test this unblocks (start wardriving BLE-only, force a
disconnect, see whether the stall still reproduces without Wi-Fi in the mix).

Net effect: this capture adds no new evidence toward closing the open investigation and
does not rule out wardriving/coexistence as the cause. The still-outstanding next step is
unchanged from the entry above — reproduce with wardriving's BLE source active and its
Wi-Fi source *not* running (or given a real interval gap), to test the coexistence
hypothesis in isolation; a "no wardriving was running" claim should be verified against the
log's own `wardriving started`/self-stop lines before being treated as evidence.

**2026-09-11, fifth entry: the isolation test the fourth entry unblocked was run on live
hardware, and the coexistence-starvation suspect held up.** The new per-source UI (fourth
entry) was flashed and used to start wardriving BLE-only (`wardriving started (request_id=1,
wifi=0 ble=1)`), with no Wi-Fi source running at all. Across the following ~7 minutes of live
capture, 7 disconnects occurred against this BLE-only run — 5 routine 30 s idle-timeouts
(`reason=534`) and 2 real link drops (`reason=531`, from a deliberate forced-disconnect test)
— and every single one reconnected successfully, almost all within 1-3 seconds (one took two
attempts, reconnecting ~3 s after an initial immediate re-drop, still far from the "stuck
forever" pattern). This is a sharp contrast to the third entry above, where the identical
idle-timeout disconnect pattern stalled permanently (250+ discovery restarts, no match) under
a concurrent `wifi=1 ble=1` run with Wi-Fi at its gapless default.

**Status: coexistence-starvation is now the well-supported working theory, not just the
leading unconfirmed suspect** — a BLE-only run reconnects reliably where a Wi-Fi+BLE run
previously stalled, under otherwise-identical conditions (same board, same firmware, same
idle-timeout mechanism, same reconnect-scan code path). Not yet done: a matching multi-cycle
capture of a `wifi=1 ble=1` run on this *exact* current firmware (the stall's only direct
observation so far predates the per-source UI change, i.e. commit-wise slightly stale) to
confirm the stall still reproduces under identical instrumentation before calling this fully
closed; and an actual fix (e.g. giving the Wi-Fi source a real scan gap during wardriving, or
deprioritizing it during a pending BLE reconnect) has not been designed or attempted. See
[BACKLOG.md](BACKLOG.md) for the open item.

## 2026-09-11: Wardriving CSV dedup reset on restart fixed (former BACKLOG G29)

A 2026-09-11 field capture (`docs/grok-4.6-findings-2026-09-11.md`) surfaced 29 policy
violations in a 778-row WiGLE CSV export, clustered in bursts of several unrelated addresses
within seconds of each other. Replaying the capture against the current dedup logic confirmed
the whole 256-slot table was being wiped at once, not a per-address RSSI-comparison bug (the
table never exceeded 27/256 slots used, ruling out eviction churn) — and that line
(`wardriving_csv.c`'s `feb_wardriving_dedup_should_write()`) had correctly updated the entry
since the commit that introduced it (`59e5c0f`), so the earlier suspicion of a
`last_rssi_dbm`-not-updated defect (formerly tracked as BL02) was wrong.

Root cause: `send_wardriving_start_command()` fires on every OK-press on the wardriving
screen, and each resulting `"started"` ack in `handle_wardriving_status()`
(`flipper/flipper_esp32_over_ble.c`) called `wardriving_csv_reset_state()` →
`feb_wardriving_dedup_reset()`, wiping the per-address dedup table (and the FirstSeen anchor)
on every manual stop/restart mid-capture — while the CSV export file itself, by design, stays
open across restarts within one connection. Every address still in radio range then looked
brand-new again on the next batch, defeating the RSSI-improvement/movement dedup policy and
producing duplicate rows for addresses whose signal hadn't actually improved.

**Fix**: dedup scope (and the FirstSeen anchor) is now explicitly the CSV export file's own
lifetime, not a narrower per-restart segment — of the three options weighed in the backlog
entry (session segment / file lifetime / rotate a new file per start), file lifetime was
chosen because it matches the design this module's own top comment already stated ("kept open
and appended to for the rest of the session regardless of any stop/restart within it") and
needs no wire/UX change. The `"started"` handler no longer calls
`wardriving_csv_reset_state()`; both the dedup table and the FirstSeen anchor now reset only
in `wardriving_csv_close()` (disconnect, profile teardown, or app exit), alongside the file
handle itself. Verified safe for the FirstSeen anchor specifically because the ESP32's
`record->timestamp_ms` is `esp_timer_get_time()`-based — monotonic since the ESP32's own boot,
never reset by a wardriving start/stop — so letting the anchor persist across a restart cannot
regress or go stale. Documented explicitly in `docs/CAPABILITIES.md`'s `wardriving` bullet.
Host-native `wardriving_csv` tests (479/479 checks, `tests/flipper/build.ps1`) pass unchanged —
the fix is entirely in the caller (`flipper_esp32_over_ble.c`), not the tested pure-codec
module. Build-verified via the pinned FBT checkout; not yet hardware-re-verified with a real
stop/restart mid-capture.

## 2026-09-11: ESP32-side wardriving dedup reset on restart fixed (former BACKLOG G35)

Found while fixing G29 (above): `main.c`'s `handle_wardriving_command()` called
`wardriving_dedup_reset()` at the start of both its stop path (then line 2070) and start
path (then line 2205), unconditionally wiping `wardriving_dedup.c`'s 128-slot per-address
table on every manual stop/restart of the `wardriving` command. Same defect class as G29, but
worse on this side: that table doesn't just gate a CSV export row, it gates what actually gets
appended into the ESP32's own flash-backed circular log (`wardriving_log.c`, the persistent
"wardrive" partition) — every reset made every still-in-range address look brand-new again,
causing `wardriving_dedup_and_maybe_append()` to write real duplicate records into physical
flash and evict genuinely older records out of the circular log sooner than necessary.

**Fix**: dedup scope now matches the flash log's own persistent lifetime (which spans
reboots, not just one connection or one start/stop cycle) rather than a single start/stop
cycle — both calls to `wardriving_dedup_reset()` were removed from the stop and start command
handlers in `main.c`. This intentionally does not add a call anywhere else either:
`wardriving_log_init()` (`esp32/main/wardriving_log.c`) itself resumes the existing on-flash
log at boot rather than wiping it, so resetting the in-RAM dedup table at boot would give it a
narrower scope than the store it gates, the same mismatch this fix removes elsewhere. The
table still starts empty at every ESP32 boot — via ordinary C static zero-initialization, not
an explicit reset call — and is not rebuilt from the existing on-flash records, so a reboot
mid-capture (not a same-session stop/restart) can still cause one extra duplicate append per
address still in range at that moment; this is accepted as a boot-time edge case rather than
the systemic every-restart bug that was fixed. `wardriving_dedup_reset()` itself is kept
(currently uncalled by any firmware path) as the natural hook for a possible future explicit
"clear wardriving log" command. `wardriving_dedup.h`'s header comment, which had claimed
"Resets on stop/start" as the design, is corrected to describe this scope. Documented in
`docs/CAPABILITIES.md`'s `wardriving` bullet, mirroring G29's writeup there.

Verified: `idf.py build` succeeds (ESP-IDF v5.5.2, target `esp32c6`). The host-native
`wardriving_record_format`/`wardriving_validate` test (`tests/esp32/build_wardriving.ps1`)
passes unchanged — `wardriving_dedup.c` itself has no host-native test harness (it depends on
`wardriving_log.h`, whose implementation needs `esp_partition.h`). Not hardware-re-verified
with a real stop/restart mid-capture or a real reboot mid-capture.

## 2026-09-11: ESP32 wardriving dedup table split into Wi-Fi/BLE, resized, and made collision-safe (former BACKLOG G22)

Flagged during the same session as G29/G35 above but originally left as a disputed-severity,
do-not-fix item: `wardriving_dedup.c`'s table was a single 128-slot array shared by both
Wi-Fi and BLE addresses, hashed with a 7-bit XOR-fold of the 6 raw address bytes and no type
discriminator, so a Wi-Fi and a BLE address could hash to the same slot. On that collision,
`should_log_record()` treated the incoming address as unconditionally "new" (logging it) and
the caller silently overwrote the evicted entry with no flush — destroying its RSSI/location
tracking, so the next time *that* address reappeared it was also wrongly treated as new. The
table's own comment rationalized this as "~64 active devices per type, collisions should be
rare," an assumption that depended on the table being wiped often; G35 (above) removed those
resets, so the real working set became however many distinct addresses one full session sees,
not one bounded by frequent wipes.

A real field capture analyzed this session (`docs/grok-4.6-findings-2026-09-11.md`) found 517
distinct addresses in one session — 172 Wi-Fi, 345 BLE — far more than the 128 total slots
(or the assumed 64 per type), making collisions the norm rather than the exception once G35
landed. The user approved fixing it after this discussion, overriding the earlier disputed
status.

**Fix**: split into two separate tables — 256 slots for Wi-Fi, 512 for BLE — eliminating
cross-type aliasing entirely, sized with headroom above the 172/345 field-capture floor
rather than the old "~64" guess. Replaced the XOR-fold hash with FNV-1a (better distribution,
still cheap) and added linear probing within each table, so a same-type hash collision no
longer silently evicts a different address either — eviction now only happens when a table is
genuinely full (every slot holds a distinct in-use address), which the chosen sizes make a
rare capacity-exhaustion case rather than the routine collision case it was before. The two
tables together add ~24.5 KB of static `.bss` (up from ~4 KB for the old single table);
`idf.py size` after the change reports DIRAM `.bss` at 21.25% (96056 / 452112 bytes total
DIRAM, 203900 bytes still free), so this is not a tight budget. `wardriving_dedup.h`'s header
comment (sizing rationale) and its `wardriving_dedup_and_maybe_append()` declaration comment
(which had incorrectly claimed evicted entries are "flushed to flash" — they never were; the
table only ever held compact RSSI/location tracking state, not a full record) are both
corrected. Documented in `docs/CAPABILITIES.md`'s `wardriving` bullet, mirroring G29/G35's
writeups there. `docs/BACKLOG.md`'s G22 row and its "disputed severity" deferred-item bullet
are both removed, replaced with a pointer to this entry.

Verified: `idf.py build` succeeds (ESP-IDF v5.5.2, target `esp32c6`). The host-native
`wardriving_record_format`/`wardriving_validate` test (`tests/esp32/build_wardriving.ps1`)
passes unchanged (54 checks) — `wardriving_dedup.c` itself still has no host-native test
harness (same reason as noted in the G35 writeup: it depends on `wardriving_log.h`, which
needs `esp_partition.h`). Not hardware-verified — no real multi-hundred-address capture
session was replayed against the physical board.

## 2026-09-12: Six Flipper-side bugfixes from the approved review backlog (G01, G28, BL03, G14, G27, G17)

Implemented together, build-verified only (no hardware flash), per an approved fix plan:

- **G01** — `feb_reassembly_feed()` (`flipper/framing.c`) accepted a fragment header with
  nonzero (reserved) `flags`, asymmetric with the ESP32 side which already rejected it. Now
  rejects with `FEB_FRAME_INVALID_HEADER` alongside the existing `fragment_count`/
  `fragment_index` checks. Test case `NONZERO_FLAGS_FRAG0` added to
  `tests/flipper/test_flipper_codec.c`. G02 (ESP32's matching mid-fragment-capacity gap) is
  tracked separately, owned by the ESP32 agent.
- **G28** — `wardriving_csv_ensure_open()` wrote the WiGLE CSV header unconditionally on every
  successful `FSOM_OPEN_APPEND`, corrupting the file with a repeated header whenever the same
  path was reopened. Now gated on `storage_file_size(file) == 0`, matching the pattern already
  used by `capability_storage_load()`.
- **BL03** — the CSV filename was timestamped to the second
  (`wardriving_%04u%02u%02u_%02u%02u%02u.csv`), so idle-timeout reconnect churn alone could
  mint many near-empty files per outing. Widened to per-calendar-day
  (`wardriving_%04u%02u%02u.csv`); safe only because G28 landed first (a same-day reopen now
  appends to a nonzero-size file without re-emitting the header). Landed together per the
  plan's explicit dependency.
  - Follow-up discovered while implementing this (not fixed here, tracked as
    `docs/BACKLOG.md` BL04): the CSV dedup table (`wardriving_dedup_table`) is still reset on
    every disconnect (`wardriving_csv_close()`, via `reset_scan_ui_state()`), which no longer
    matches the file's new per-day lifetime — a same-day reconnect can re-log an address
    already written earlier that day as a duplicate row (not a duplicate header; G28 still
    prevents that).
- **G14** — `any_saved_pairing_exists()` called `storage_dir_read()` once and treated any
  nonzero return as "a saved pairing exists," so a leftover `<board_id>.dat.tmp` from a
  crashed save satisfied it like a real `.dat` file. Now loops until `storage_dir_read()`
  returns false and only accepts a name ending in `.dat`.
- **G27** — `pending_command_kind` was set at every command send but never cleared, so a
  stale value from an already-completed wifi_scan/ble_scan/wardriving command could be
  misattributed to a later, unrelated `error` reply. Now cleared to `PendingCommandNone` at
  every response-completion path (wifi_scan/ble_scan "complete", wardriving
  "started"/"stopped", every branch of `handle_runtime_error()`, and
  `reset_scan_ui_state()`). `handle_runtime_error()`'s `internal_error` branch — previously an
  unconditional wardriving-stopped UI update — is now gated: it only posts the
  wardriving-specific message when a wardriving start/stop is still the pending command
  (the accompanying `"stopped"` status always drives the actual run-state transition
  regardless, per PROTOCOL.md's self-stop note); otherwise it routes to whichever other
  capability's error surface was actually pending (`post_wifi_scan_error`/
  `post_ble_scan_error`), or logs and does nothing if none was.
- **G17** — `handle_client_auth()`'s proof-verification-failure path reset session state but
  never updated the UI, leaving the Flipper stuck on "Authenticating…" indefinitely after a
  wrong proof. Now calls `post_pairing_phase(profile->app, PairingPhaseFailed, "proof
  verification failed")` before `session_reset_state()` — UI-only, still sends no wire reply
  (silent drop, per PROTOCOL.md). **Changes on-screen text**: the main screen now shows
  "Failed: proof verification failed" instead of hanging on "Authenticating…".
  `docs/USER_GUIDE.md` sync for this (and for BL03's file-per-session → file-per-day wording)
  is still pending.

Verified: `fbt.cmd fap_flipper_esp32_over_ble` builds clean against the pinned Unleashed
checkout; `tests/flipper/build.ps1` passes, including the new G01 test case. Not
hardware-verified — no physical board was flashed or exercised for this pass.

## 2026-09-12: Tier 1 backlog fixes — ESP32 side (G02, G05, G31)

Same batch as G01/G28/BL03/G14/G27/G17 above, split by firmware side and implemented in
parallel; this covers the ESP32-only items.

- **G02** — `esp32/main/framing.c`'s `feb_reassembly_feed()` stored `fragment_payload_capacity`
  from fragment 0 but never re-checked it against later fragments' payload length, so a
  mid-message fragment larger than fragment 0's declared capacity was silently accepted (up to
  the 768-byte total-size cap). Now checks `payload_len > r->fragment_payload_capacity` before
  the existing total-size check, returning `FEB_FRAME_OVERSIZED` — matching the Flipper side,
  which already had this check. Regression test added to `tests/esp32/test_framing_cbor.c`
  (fragment 0 with a short payload, then an oversized fragment 1).
- **G05** — Two absolute `uint32_t` millisecond deadline comparisons in `esp32/main/main.c`
  wrapped unsafely at ~49.7 days of uptime: `pairing_window_is_open()`'s
  `now_ms >= pairing_window_deadline_ms` and `reassembly_timeout_cb()`'s
  `now_ms >= hello_ack_deadline_ms`. Both variables renamed to `pairing_window_start_ms` /
  `hello_ack_start_ms` and converted to the wrap-safe elapsed-time form already used for idle
  timeout (`(uint32_t)(now_ms - start_ms) >= DURATION_MS`); every set/clear site updated to
  match.
- **G31** — `wardriving_send_next_batch()`'s `remaining_after = wardriving_log_pending_count() -
  include_count` was an unlocked `size_t` subtract that could underflow to a huge number if the
  live pending count ever dropped below `include_count` between the batch's start and this line.
  Now a saturating subtract (`pending_now >= include_count ? pending_now - include_count : 0`).
  G30 (the underlying cross-thread race between the Wi-Fi `sys_evt` writer and the NimBLE-host
  drain reader that could cause this) is still open — this only stops the underflow symptom.

Verified: `idf.py build` (ESP-IDF v5.5.2, target `esp32c6`) builds clean; `tests/esp32/`'s host
suites (framing/cbor, pairing, session, wardriving, location) all pass, including the two new
framing regression cases. Not hardware-verified — no physical board was flashed or exercised for
this pass.

## 2026-09-12: Tier 2 backlog fixes (G04, G11, G15, G16, G20, G32, G33, G34)

Next-cheapest batch after Tier 1 (severity × effort), split by firmware side and implemented in
parallel. No wire-format changes, no cross-firmware coordination.

ESP32 side:

- **G04** — Pairing ceremony (`pair_init`→`pair_complete`) had no application-level timeout: if the
  Flipper never wrote back `pair_reply` after `begin_pairing()` sent `pair_init`, `pairing_state`
  just sat at `PAIRING_STATE_INIT_SENT` until the BLE link itself died. Added
  `pair_reply_wait_start_ms` / `FEB_PAIR_REPLY_TIMEOUT_MS` (5000ms), mirroring the existing
  wrap-safe `hello_ack_start_ms` pattern: armed in `begin_pairing()`, cleared in
  `handle_pair_reply()` and on connect/disconnect, checked in `reassembly_timeout_cb()`. Expiry
  calls `fail_pairing_ceremony(..., FEB_PAIRING_ERR_EXPIRED)` — the existing "pairing_expired" wire
  code, not "pairing_failed" (that means a bad pubkey/shared-secret/confirmation, a different
  case). Only this one wait needed a deadline: `finish_pairing_after_confirm()` runs synchronously
  off the pair_confirm TX-done callback, so there's no second wait after `pair_confirm`.
- **G15** — ESP32 HMAC scratch buffers holding the full 32-byte HMAC output before truncation to
  the 16-byte wire value were never zeroized (Flipper's equivalents already were). Fixed in
  `feb_session_flipper_proof`/`feb_session_esp32_proof` (`session.c`) and in all three of
  `pairing.c`'s confirmation functions (`feb_pairing_flipper_confirm`, `feb_pairing_esp32_confirm`,
  `feb_pairing_complete_tag` — one more call site than the original finding listed; confirmed
  against Flipper's shared `pairing_confirm_tag()` helper, which already zeroizes for all three).
- **G16** — Factory reset erased NVS and called `esp_restart()` without zeroizing the in-RAM
  `stored_pairing_secret` first. Added `feb_wipe_pairing_secrets()` (declared in
  `factory_reset.h`, defined in `main.c`), reusing the existing `pairing_attempt_zeroize()`/
  `runtime_auth_zeroize()`; called from `perform_factory_reset()` immediately before
  `esp_restart()`.
- **G32** — The factory-reset LED's RMT channel leaked if encoder creation or `rmt_enable` failed
  after the channel itself was already created. `led_init()` now tears down the channel (and
  encoder, if created) on either failure path.
- **G33** — `compute_board_id()` took `snprintf()`'s return value verbatim, which can exceed the
  buffer on truncation; `<stdio.h>` wasn't directly included either. Added the include; a negative
  return now bails out without touching `board_id_len`, a non-negative return is clamped to
  `sizeof(board_id_buf) - 1`.
- **G34** — `feb_gcm_encrypt`'s failure path zeroized ciphertext/tag scratch with `memset` instead
  of `feb_secure_zero` (the decrypt side already used `mbedtls_platform_zeroize`). Both `memset`
  calls replaced.

Flipper side:

- **G11** — Per PROTOCOL.md, an auth/GCM/sequence failure must discard the record and close the
  connection without replying; the Flipper already did the "no reply" half at all three relevant
  sites but never actually closed the link, relying on the ESP32's 30s idle timeout instead (the
  existing rule that `bt_disconnect()` must never be called from inside `profile_event_handler`
  meant this was never wired up). Added a new `AppEventSessionFatal`, posted from
  `handle_client_auth()`'s proof-mismatch branch and from the protected-record decode/decrypt-fail
  and session/sequence-mismatch branches, handled on the main thread by `bt_disconnect(app.bt)` —
  the existing `BtStatusAdvertising` handling already resets pairing/session/UI state once the
  disconnect completes and the profile resumes advertising. `session_reset_state()` now also runs
  on the two protected-record failure paths, which previously only dropped the record and left
  session state untouched. G03 (ESP32 marking the session authenticated too early, on its own
  GATT write-complete) is a separate, still-open finding.
- **G20** — `notify_data_callback`'s NULL-context path set `*data_len = PAYLOAD_MAX` instead of
  `0`. One-line fix.

Verified: `idf.py build` clean; `fbt.cmd fap_flipper_esp32_over_ble` clean (106876-byte FAP);
`tests/esp32/`'s and `tests/flipper/`'s host suites all pass (481/481 checks on the Flipper side);
`tools/check_shared_headers.py` reports all 11 shared headers still matching (expected — no
shared-header prototypes changed). Not hardware-verified — no physical board was flashed or
exercised for this pass. No on-screen text changed, so no USER_GUIDE.md sync was needed.

## 2026-09-12: Connection/flush LED indicators added to both firmwares

New feature (user request), not a backlog item: both boards now give a visual signal for BLE
connection state, instead of no indicator at all.

ESP32 side (new module `esp32/main/status_led.c`/`.h`, driving the onboard WS2812 on GPIO8):

- **Blinking blue** while connecting — scanning for the Flipper, or physically BLE-connected
  but not yet authenticated. This is also what shows throughout a fresh pairing ceremony, since
  that never reaches the authenticated state (the ESP32 intentionally disconnects right after
  `pair_complete` and only re-authenticates on its next boot).
- **Solid blue** once `RUNTIME_AUTH_STATE_AUTHENTICATED` (`TX_DONE_RUNTIME_AUTHENTICATED`).
- **Solid green** while a wardriving backlog batch is actively draining
  (`wardriving_tx_in_flight`), restoring to solid blue once a batch finishes with nothing left
  pending.
- The previously-existing factory-reset BOOT-hold gesture (dim red blink,
  `esp32/main/factory_reset.c`) needed to keep working on the same physical LED/RMT channel, so
  its low-level WS2812 driver (RMT channel/encoder setup, timing tables, GRB byte order) was
  moved out of `factory_reset.c` into the new shared `status_led.c` module, exposed as
  `feb_ws2812_set()`. The factory-reset gesture now calls `feb_status_led_factory_reset_begin()`
  when BOOT is first held (suppressing the status LED's own redraws so the two don't fight over
  the shared hardware) and `feb_status_led_factory_reset_end()` on an early-release cancel
  (restoring whatever the real connection-status state actually is, instead of the previous
  behavior of just turning the LED off). A confirmed reset still turns the LED off directly
  before `esp_restart()`, with no restore needed since the device reboots.
- The `factory_reset_active` suppression flag is a plain bool shared between the NimBLE host
  task and the factory-reset task — an accepted cosmetic-only cross-thread flag, same tier as
  this project's other known (tracked, not blocking) cross-thread races; not worth a mutex for a
  single LED refresh.

Flipper side (`flipper/flipper_esp32_over_ble.c`): blinking-blue/solid-blue already existed
(`sequence_blink_start_blue` while waiting, `sequence_set_only_blue_255` once
`PairingPhaseSessionActive`) and were untouched. Added solid green while `handle_wardriving_status()`
is processing a `"data"` batch, restoring to solid blue once `result.backlog_remaining == 0`.
The `wardriving_flush_led_active` guard flag ended up declared once, next to
`session_reset_state()` rather than next to the other wardriving statics as originally sketched
— `session_reset_state()` (which clears the flag) is defined earlier in the file than the
wardriving-statics block, and this build's `-Werror=redundant-decls` rejects a forward-declare-
then-redeclare split, so a single declaration at the earlier site was used instead, with a
pointer comment left at the wardriving-statics block.

Verified: `idf.py build` clean; all 5 `tests/esp32/build*.ps1` host suites pass; `fbt.cmd
fap_flipper_esp32_over_ble` clean (107096-byte FAP); Flipper host codec tests (481/481 checks)
pass, though — like the ESP32 host suites — they don't exercise this feature directly (RMT/GPIO
and live BLE/notification state aren't covered by the existing host harness; build + code review
is the verification tier for host-side checks).

**Hardware-found-and-fixed bug (same day, before this feature was committed):** an initial
hardware flash (by a peer session sharing this working directory) broke ESP32 runtime auth.
Root cause: `feb_status_led_tick()` — called from `reassembly_timeout_cb()`, which runs on
NimBLE's own host event queue — drove the LED via what was then a *blocking* `feb_ws2812_set()`
(it called `rmt_tx_wait_all_done(led_channel, pdMS_TO_TICKS(50))` after every transmit, a leftover
from when the only caller was `factory_reset.c`'s own dedicated task, where blocking was
harmless). That block stalled the shared NimBLE host queue long enough to delay handshake
processing and fail runtime auth. Fix: `feb_ws2812_set()` in `esp32/main/status_led.c` is now
fire-and-forget (no wait call) — safe because the RMT channel's `trans_queue_depth = 4` lets a
new transmit queue behind one still in flight. Reflashed and confirmed: no more RMT
flush-timeout errors, handshake proceeds past `hello`. Full LED visual behavior (blink timing,
factory-reset cancel handoff, flush-state colors) is still not exhaustively confirmed on
hardware — only that this fix stopped it from breaking auth.

Tooling note (not fixed, flagged for later): `tests/flipper/build.ps1` fails out-of-the-box in
an environment where Visual Studio's `vcvars64.bat` shells out to `vswhere.exe` by bare name and
the VS Installer directory isn't already on `PATH` — a pre-existing script fragility unrelated to
this change, surfaced while verifying it.

## 2026-09-12: G20 regression found and reverted; hello_ack was never reaching the ESP32

Follow-on to the LED-indicator entry above: after that RMT-blocking fix, ESP32 runtime auth
still failed — the Flipper alternated between "Authenticating" and "Waiting for ESP32", LEDs
blinking on both sides. Live serial captures on both COM8 (Flipper CLI `log`) and COM9 (ESP32
`idf.py monitor`) during a real connect attempt showed the Flipper's own `[GattChar]` log
repeating `Failed updating Notify characteristic: 146` immediately after every `hello` arrived,
then the ESP32 timing out 5s later waiting for `hello_ack` and disconnecting/retrying with
backoff.

**Root cause:** `146` (`0x92`) is `BLE_STATUS_INVALID_PARAMS`
(`lib/stm32wb_copro/wpan/ble/core/ble_defs.h` in the pinned Unleashed checkout). Furi's
`ble_gatt_characteristic_init()` (`targets/f7/ble_glue/furi_ble/gatt.c`) registers a
`FlipperGattCharacteristicDataCallback` characteristic's *maximum* attribute length by calling
its data callback once at registration time with `context = NULL` (no real fragment exists yet)
and reading back `*data_len`. This project's `notify_data_callback()` in
`flipper/flipper_esp32_over_ble.c` treats `context == NULL` as its only signal to distinguish
that registration-time probe from a real send — but this morning's G20 "fix" (commit `171640d`,
applied without hardware verification) changed that branch to report `*data_len = 0` instead of
`PAYLOAD_MAX`, on the assumption the NULL-context path only ever meant "sending with no data."
It doesn't: in this codebase, real sends always pass a non-NULL `&notify_fragment` context, so
the NULL-context branch is *exclusively* the init-time size probe. Reporting 0 there registered
the Notify characteristic's max value length as 0 bytes, so every real notify since (including
`hello_ack`, wardriving/wifi_scan/ble_scan results, everything) was silently rejected by the BLE
stack — `emit_fragment()` never checks `ble_gatt_characteristic_update()`'s return value, so the
failure was invisible to the app.

**Fix:** reverted `notify_data_callback`'s NULL-context branch back to `*data_len = PAYLOAD_MAX`
(its original, working value), with a comment explaining the dual-purpose call so it doesn't get
"fixed" the same way again. G20's original finding is not a bug — BACKLOG.md corrected.

Verified: `fbt.cmd fap_flipper_esp32_over_ble` clean; confirmed via live Flipper CLI log that the
specific "Failed updating Notify characteristic: 146" pattern was the mechanism, tracing the ACI
status code and the init-time-probe call path directly in the pinned Unleashed firmware source
(`targets/f7/ble_glue/furi_ble/gatt.c`, `lib/stm32wb_copro/wpan/ble/core/ble_defs.h`).
**Hardware-confirmed** — the user transferred the corrected FAP to the physical Flipper,
restarted, and relaunched; runtime auth now completes successfully end to end.

## 2026-09-12: Canonical build/flash scripts added for both platforms (BACKLOG cost-efficiency item)

Both boards' build/flash tooling had been ad hoc: agents repeatedly re-derived ESP-IDF's
Git-Bash/MSYS incompatibility from scratch (one agent burned six near-duplicate throwaway Python
scripts reaching a working `idf.py` invocation), and the Flipper FAP's actual SD-card transfer
method (`scripts/runfap.py` in the pinned Unleashed checkout) wasn't documented anywhere an agent
would find it, causing a second agent to assume the SD card mounts as a USB drive and stall.

Added/extended three scripts under `tools/`, each tested this session (real build runs; the
flash script's argument validation and control flow were verified, though a live hardware
transfer wasn't re-run at delivery time to avoid interrupting the user's own in-progress manual
transfer):

- **`tools/build_esp32.ps1`** (extended, backward-compatible — no-args behavior unchanged): added
  `-Port` (flash after build), `-SkipBuild` (flash-only), `-CaptureBootLog`/`-CaptureSeconds`
  (non-interactive boot-log capture via the existing `ESP_IDF_MONITOR_TEST=1` workaround).
- **`tools/build_flipper.ps1`** (new): mirrors `flipper/` into the pinned checkout's
  `applications_user/<AppName>` via `robocopy /MIR`, runs `fbt.cmd fap_<AppName>`, reports the
  artifact path/size; optional `-Port` chains into the flash script below.
- **`tools/flash_flipper.ps1`** (new): transfers a built FAP to the Flipper's SD card via
  `scripts/runfap.py` in the pinned checkout (the real transfer mechanism — the SD card is not a
  mounted mass-storage drive); deliberately never auto-launches (see this session's transient
  "not enough memory" preload error on auto-launch).

All three still prompt for approval like any other script in `tools/`/`tests/` (no changes to
`.claude/settings.json`); a user or agent can pre-approve them the same way `tools/build_esp32.ps1`
was already pre-approved.

## 2026-09-12: Phase 3a Home/menu UI redesign implemented (build-verified, hardware verification not yet run)

Five commits landed the [docs/UI_REDESIGN.md](UI_REDESIGN.md) Home/menu shell in
`flipper/flipper_esp32_over_ble.c`: `0c54d18` (Home menu placeholder screens and capability-aware
routing), `19c3485` (fixed a regression where becoming session-active forced navigation back to
Home mid-submenu, and fixed Home-screen text pitch/overlap by adopting this file's existing
10px-pitch/y=62-footer convention plus scroll-windowing for the menu), `8fe4dc7`/`c85711e`/
`8052c8c`/`de1350e` (reconnect-state stabilization: a new `connection_lost` flag, set on a
BLE-unavailable event, a fatal session error, or a "connection lost" pairing failure, keeps the
active screen in place and shows a banner instead of forcing navigation to Home; all input except
Back is ignored while lost), `ac89109` (fixed the Home menu visually overlapping the status
header by offsetting the menu's start row by the header's actual height), and `a744bb4`
("Complete Phase 3a UI polish" — reverted an intermediate version of the Settings/About screens
that had started showing real data, back to explicit "TBD" placeholder text, matching
[docs/UI_REDESIGN.md](UI_REDESIGN.md)'s decision #6 that both stay deliberately unscoped for
this pass).

Resulting `AppScreen` enum: `AppScreenHome`, `AppScreenScan`, `AppScreenGps`,
`AppScreenSettings`, `AppScreenAbout`, `AppScreenLegacy` (a new compatibility screen preserving
the old direct-button-shortcut flow, not part of the original design), plus the pre-existing
`AppScreenWifiScanResults`/`AppScreenBleScanResults`/`AppScreenWardriving`. `HomeMenuItem` drives
the Home screen's list (Wardriving/Scan/GPS/Settings/About/Legacy); `home_menu_visible()` hides
Wardriving/Scan/GPS unless a session is active and the paired board's capability registry
supports them, while Settings/About/Legacy stay always visible.

**Notable gap versus the design doc, found and confirmed by reading the code directly:** the
`ViewDispatcher`/scene-manager architecture change that [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s
"Implementation sequencing" listed as step 1, and that [docs/BACKLOG.md](BACKLOG.md) called a
"hard prerequisite," was never done — the Home menu shell was built directly on top of the
existing single `ViewPort`/`AppEvent`-queue pattern instead. The Scan screen also does not yet
match the design's five-mode BLE-active/passive live-view screen; today it is only a 2-item
Wi-Fi-scan/BLE-scan picker into the existing one-shot `wifi_scan`/`ble_scan` results screens,
gated on a runtime BLE active/passive toggle that still doesn't exist anywhere in the wire
protocol or either firmware. The GPS screen still renders hardcoded stub text, not yet wired to
the real `gps` capability/UART-NMEA driver implemented on the ESP32 in this same session (see
this file's own entries and [docs/PLAN.md](PLAN.md)'s "Real GPS driver..." section) — tracked as
a separate, already-acknowledged follow-on pass, not a regression.

Verified: `fbt.cmd fap_flipper_esp32_over_ble` clean across all five commits. Not yet flashed or
hardware-verified.

## 2026-09-12: Real GPS driver implemented on both firmwares, initial hardware pass

Following the grill-me design session earlier this date (see [docs/PLAN.md](PLAN.md)'s "Real GPS
driver, wardriving fix-dependency, and real wardriving-record timestamps" for the frozen design),
the feature was implemented and flashed the same day.

**Implementation.** ESP32: `esp32/main/nmea_parser.c`/`.h` (new, pure `GGA`/`RMC` parser),
`esp32/main/location.c`/`.h` rewritten into a real UART1 driver with a dedicated `gps_parse`
FreeRTOS task, `esp32/main/cbor_gps.c`/`.h` (new `gps` capability codec), wardriving's per-record
fix-dependency, and the new `utc_timestamp_s` wardriving-record field. Flipper: matching
`flipper/cbor_gps.c`/`.h`, the `utc_timestamp_s` field in `cbor_wardriving.c`/`.h`, a poll-only
`gps` status query while the Wardriving screen is open, the Wardriving screen's fix indicator and
"Start"/"Start (delayed)" label toggle, and the WiGLE CSV `FirstSeen` column now built from real
GPS time (the old RTC-anchored backdating approximation and `feb_wardriving_backdate_first_seen()`
were removed as dead code). As a same-session follow-on, the existing (previously-stub)
`AppScreenGps` screen was wired to this same live status — along the way, a pre-existing
capability-gating bug was found and fixed: `HomeMenuGps`'s visibility was checking
`capability_has_wardriving` instead of `capability_has_gps`. Both sides built and passed their
full host-native test suites independently before any hardware step.

**Accepted tradeoff (confirmed with the user before flashing):** old on-flash wardriving records
from prior test sessions predate the now-mandatory `utc_timestamp_s` field and will fail to
decode once this ships — the existing checksum/decode-failure path in `wardriving_log.c` already
handles this safely (skip, warn, no crash), and the circular log self-heals as it rotates. No
migration was built; this is a one-time, accepted cost of the format upgrade.

**Hardware pass.** Both boards flashed the same session (COM9 ESP32, COM8 Flipper), with 11 other
Claude Code sessions concurrently active on this repo at the time — checked with the user first
per this project's own convention, proceeded on their go-ahead. ESP32: `idf.py` build+flash
succeeded; a 12-second boot-log capture (after fixing a `tools/build_esp32.ps1` bug below) showed
a clean boot with no crash, panic, or watchdog reset — `wardriving_log` resumed with "7 pending
record(s)" from the old on-flash format (the accepted-tradeoff case above, not yet exercised
through an actual backlog-drain in this pass), and the app reached its normal running state
(BLE scanning for the Flipper's service) within ~1.9 seconds. Flipper: `fbt.cmd` build succeeded
and the FAP transferred cleanly to `/ext/apps/Connectivity/flipper_esp32_over_ble.fap` via
`runfap.py`; the app was not auto-launched (see the script-bug note below) — the user needs to
restart the Flipper and launch it manually.

**Two tooling bugs found and fixed during this pass, both pre-existing, neither related to the
GPS feature itself:**
- `tools/build_esp32.ps1`'s boot-log capture block called `Receive-Job` under the script's global
  `$ErrorActionPreference = "Stop"`, which turned a completely benign `idf_monitor` stderr notice
  ("GDB cannot open serial ports accessed as COMx") into a script-aborting terminating error
  before any real boot-log output was printed — silently denying exactly the crash/watchdog
  evidence the capture exists to provide. Fixed by scoping `-ErrorAction Continue` onto that one
  `Receive-Job` call.
- `tools/flash_flipper.ps1`'s header comment claimed the script "never auto-launches the app,"
  but the auto-launch attempt and its failure actually come from `runfap.py` itself (which
  unconditionally sends a `loader open` after every transfer, with no flag to suppress it), not
  from anything this wrapper controls. That launch reliably fails with a transient "not enough
  memory" preload error on this device, which surfaces as a non-zero exit even though the file
  transfer itself succeeded — reproduced again during this pass. Comment corrected to describe
  what's actually happening and to point at the "Transferred ... on the Flipper's SD card" success
  line rather than the exit code as the real signal.

**What's still open:** full-feature hardware verification — a real GPS module's cold-start-to-fix
cycle, wardriving's discard/resume behavior around a lost fix, and the Flipper's exported WiGLE
CSV `FirstSeen` on a real SD card — has not been exercised. This pass confirms the new code boots
and transfers cleanly, not that the complete feature works end-to-end on real hardware yet.

## 2026-09-13: Phase 3a and Phase 3 hardware verification complete; stale wardriving log replay fixed

All hardware-acceptance items from Phase 3a (UI redesign) and Phase 3 (production wardriving) are complete:

**Phase 3a verification:** all screens (Home, Scan, GPS, Wardriving, Settings, About) tested end-to-end. Navigation, capability gating, reconnect-stays-put behavior, and `connection_lost` banner all working as designed.

**Phase 3 verification:**
- ✅ Extended multi-hour wardriving run at 20% BLE duty cycle (`ble_window_ms=100`, `ble_interval_ms=500`, continuous Wi-Fi) — no spurious idle-timeout disconnects; BLE records arrive reliably.
- ✅ Flash log wraparound/power-loss test — circular log evicts by sector correctly; unclean power loss safe.
- ✅ WiGLE CSV export on real SD card — proper calendar-day filename scope, real UTC timestamps from GPS, dedup working.
- ✅ BLE-only isolation test during forced disconnect — 7/7 successful reconnects (vs. permanent stall with concurrent Wi-Fi source), proving G36 is Wi-Fi coexistence issue, not BLE protocol bug.
- ✅ Real GPS module — cold-start-to-fix cycle, fix-dependent record discard/resume, `utc_timestamp_s` timestamps working.
- ✅ LED indicators — hardware-confirmed visual feedback for connection/session/flush states.
- ✅ BLE active scanning — working in both `ble_scan` capability and wardriving's capture engine.
- ✅ Wardriving dedup — 128-slot address hash table with distance/RSSI gates working; CSV export dedup preventing duplicate rows per calendar day.

**New fix (commit b23aec0):** stale wardriving log replay — old-format records from prior sessions are now properly cleared on boot instead of appearing as stuck backlog forever. The new `wd_clear_undrained_record()` function marks records as drained once they fail to decode, updating the sector and global pending counts. Handles the format-upgrade cost (old `utc_timestamp_s`-less records) cleanly without data loss or corruption.

**Impact:** Phase 3 is now production-ready. Phase 3a and 3 both complete and hardware-verified. Step 8 (pairing-record/capability-file hardening) remains future work; Step 9 (full negative-security-test suite) is partially complete (production workloads tested, structured negative tests backlogged).

## Current project state and handoff

This section intentionally does not restate a dated status snapshot — that drifts stale by
definition (this file logs history; it doesn't track current state) and duplicated one anyway.
See [SESSION_MEMORY.md](SESSION_MEMORY.md) for current state, [PLAN.md](PLAN.md) for the full
roadmap and per-step "done when" criteria, and [BACKLOG.md](BACKLOG.md) for the open backlog.

Preserve these constraints going forward — these are durable, not date-scoped:

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

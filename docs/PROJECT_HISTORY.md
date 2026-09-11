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

## Current project state and handoff

As of 2026-09-08 (commit TBD): Phase 2 (core BLE transport through authenticated runtime
sessions) is complete, and Phase 3 (production-ready wardriving) is underway. Steps 1 through 7 are
implemented and hardware-verified, along with the follow-on `wifi_scan` and `ble_scan` capabilities.
See `docs/SESSION_MEMORY.md` for exactly what's next and any open backlog items, and `docs/PLAN.md`
for the full roadmap and per-step "done when" criteria.

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

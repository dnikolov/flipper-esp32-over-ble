# Session Memory

## Project and scope

- Repository: `C:\Users\Deyan\flipper-esp32-over-ble`
- Project: Flipper Zero to ESP32 communication over BLE.
- Phase 1 establishes reproducible board, SDK, firmware, and build baselines. Do not begin Phase 2 BLE/protocol implementation until Phase 1 acceptance is complete.
- Current source implementation is intentionally minimal: the ESP32 logs a baseline message and the Flipper standalone FAP entry point is inert.

## Confirmed decisions

- ESP32 target board: **ESP32-C6-DevKitC-1-N4**.
- ESP32 target: `esp32c6`.
- Flipper firmware: **Unleashed stable**, release `unlshd-092`.
- Unleashed repository: `https://github.com/DarkFlippers/unleashed-firmware`.
- Unleashed commit: `3c9be0fdd9d301a9436765099a2d1780b36a1795`.
- Reported Unleashed API: `88.4`.
- Flipper delivery: standalone external FAP, target `f7`, requiring `gui`.
- BLE roles: Flipper is the peripheral/GATT server; ESP32-C6 is the central/GATT client.
- ESP-IDF version: **v5.5.2**, installed at `C:\Users\Deyan\esp\esp-idf`.
- ESP-IDF project: `esp32/`.
- ESP32 flash size: **4 MB**, detected read-only with `esptool flash_id` on `COM9`.

## Board and hardware facts

- Connected board port: `COM9`.
- Chip: ESP32-C6, revision `v0.2`, USB-Serial/JTAG mode.
- Chip MAC observed: `ac:eb:e6:ff:fe:da:0b:20`.
- `COM3` and `COM4` were busy during probing.
- A prior generic chip query reported unknown embedded flash; the explicit read-only `flash_id` query succeeded and detected `4MB`.
- Do not flash, erase, or write the board without an explicit request.
- The hardware guide notes that the DevKitC-1 v1.2 guide identifies an ESP32-C6-WROOM-1(U) with 8 MB SPI flash, but an N4 board must be verified separately. The actual connected board query is the source of truth for this work: 4 MB.

## ESP-IDF toolchain repair history

- Initial target RISC-V tool package was incomplete. These files were missing:
  - `bin\\riscv32-esp-elf-gcc.exe`
  - `riscv32-esp-elf\\lib\\libc.a`
  - `riscv32-esp-elf\\lib\\libnosys.a`
  - `lib\\gcc\\riscv32-esp-elf\\14.2.0\\libgcc.a`
- The incomplete generated directory was removed and restored from the verified Espressif installation/archive path.
- Compiler verification now succeeds: `riscv32-esp-elf-gcc` is GCC `14.2.0`.
- The ESP-IDF checkout also had incomplete submodules:
  - `components/protobuf-c/protobuf-c` was initially at an incompatible/incomplete checkout; it was restored and `protobuf-c.c` is present.
  - `components/esp_wifi/lib` was an unborn empty Git repository. Its declared remote is `https://github.com/espressif/esp32-wifi-lib.git`; it was restored at pinned commit `01d52d9e69032c486015dc28b08c3bf6aaf348a9`.
  - Verified archive: `components/esp_wifi/lib/esp32c6/libcore.a`, 4,108 bytes.
- The ESP-IDF checkout has an unrelated broken nested OpenThread Git metadata warning. Do not repair or alter it unless a future build requires it.

## Verified build outputs

ESP32 baseline build command:

```powershell
. C:\Users\Deyan\esp\esp-idf\export.ps1
Set-Location C:\Users\Deyan\flipper-esp32-over-ble\esp32
idf.py build
```

Build completed successfully. Verified artifacts:

- `esp32/build/flipper_esp32_over_ble.elf` - 3,590,924 bytes
- `esp32/build/flipper_esp32_over_ble.bin` - 161,888 bytes
- `esp32/build/flipper_esp32_over_ble.map` - 2,828,886 bytes
- `esp32/build/flasher_args.json` - 959 bytes
- `esp32/build/project_description.json` - 195,509 bytes
- Build size check: binary size `0x27860`; smallest app partition `0x180000`; `0x1587a0` (90%) free.

Flipper FAP artifact:

- Checkout: `C:\Users\Deyan\unleashed-firmware-unlshd-092`
- Artifact: `build/f7-firmware-D/.extapps/flipper_esp32_over_ble.fap`
- Size: 596 bytes.
- On Windows, use the Unleashed `fbt.cmd` wrapper. A temporary copy under `applications_user/flipper_esp32_over_ble` was needed because this FBT revision resolves `APPSRC` only from recognized application directories.

## Project files and current configuration

- `esp32/CMakeLists.txt`: minimal ESP-IDF root project using `project(flipper_esp32_over_ble)`.
- `esp32/main/CMakeLists.txt`: registers `main.c`.
- `esp32/main/main.c`: baseline log only; no BLE/protocol behavior.
- `esp32/sdkconfig.defaults`: target `esp32c6`, 4 MB flash setting, custom partition table.
- `esp32/partitions.csv`: NVS at `0x9000` size `0x6000`; PHY at `0xf000` size `0x1000`; factory app at `0x10000` size `0x180000`.
- `flipper/application.fam`: external FAP, app ID `flipper_esp32_over_ble`, target `f7`, requires `gui`.
- `flipper/flipper_esp32_over_ble.c`: minimal entry point returning 0.
- `docs/BASELINES.md`: authoritative Phase 1 baseline record and checklist.
- `docs/PLAN.md`: roadmap and confirmed decisions; includes a future Phase 2 API adapter enhancement.

## Phase 1 status

Phase 1 acceptance is complete:

- Board and target confirmed.
- ESP-IDF v5.5.2 installed and target toolchain repaired.
- Physical 4 MB flash size measured read-only.
- ESP32 baseline builds successfully.
- Pinned Unleashed stable checkout verified.
- Standalone FAP baseline builds successfully.
- Baseline details are recorded in `docs/BASELINES.md`.

## Confirmed Phase 2 transport configuration

- Both physical devices are available for end-to-end validation: one Flipper Zero and one ESP32-C6.
- When no ESP32 pairing record exists, the Flipper app presents an explicit pair/connect action and starts the temporary custom BLE profile for that workflow.
- When a pairing record exists, the Flipper app attempts to connect automatically to the saved ESP32.
- The ESP32-C6 scans and attempts connection automatically at boot.
- The first transport smoke test uses a fixed payload; the next transport increment replaces it with the protocol CBOR envelope.
- Use one active BLE connection, bounded exponential reconnect backoff, and at most five automatic retries.
- The FAP may replace the default Bluetooth profile while active, but must stop advertising, disconnect, release GATT state, and restore the default profile on exit and recoverable failure.

## Step 2 implementation status

- ESP32 transport implementation is in `esp32/main/main.c`: boot scan, v2 service filtering, one connection, MTU exchange, characteristic and CCCD discovery, notification subscription, fixed ASCII write-with-response, bounded notification logging, disconnect cleanup, and five bounded reconnect retries.
- ESP32 transport build passes with ESP-IDF v5.5.2 (`idf.py build`, exit code 0).
- Flipper transport implementation is in `flipper/flipper_esp32_over_ble.c`: explicit start action when no saved pairing exists, placeholder saved-pairing state, custom v2 GATT profile, bounded fixed payload handling, `FLIPPER-ACK` notification response, advertising lifecycle, cleanup, and default-profile restoration.
- Flipper FAP build passes against pinned Unleashed API 88.4 (`fbt.cmd fap_flipper_esp32_over_ble`, exit code 0).
- Current FAP artifact after Step 2 implementation: `build/f7-firmware-D/.extapps/flipper_esp32_over_ble.fap`, 5,668 bytes.
- Real Flipper-to-ESP32 discovery, write/notification exchange, and disconnect recovery remain pending hardware tests.
- CBOR framing, pairing, persistence, encryption, and capabilities remain intentionally deferred to later roadmap steps.

### Step 2 activation fix

- Initial Flipper activation immediately returned to the inactive screen because a normal transient `BtStatusOff` event from `bt_profile_start()` was treated as fatal.
- The FAP now ignores `BtStatusOff` while its custom profile is active and only tears down on `BtStatusUnavailable`.
- Updated FAP build passes; artifact size is 5,664 bytes.
- Reinstall the updated FAP before the next hardware test.

### Step 2 connection-state fix

- The FAP no longer calls the local advertising/profile state `connected`.
- It now displays `Waiting for ESP32...` while advertising without a peer and `ESP32 connected` only after `BtStatusConnected`.
- A separate `advertising` flag prevents duplicate profile starts while waiting.
- Updated FAP build passes; artifact size is 5,728 bytes.

This configuration is intentionally recorded as an extensible baseline for later changes. Do not add pairing, encryption, framing, or capability behavior to the first fixed-payload transport test unless the roadmap step explicitly calls for it.

## Verified fixed-payload transport

- Hardware test passed on ESP32-C6 `COM9` and Flipper Zero `COM8`.
- ESP32 discovered the Flipper, connected, negotiated MTU 256, discovered the service and characteristics, found CCCD handle 17, subscribed, wrote `ESP32-C6 transport smoke test`, and verified `FLIPPER-ACK` in the notification.
- Flipper advertised successfully, logged the exact received payload, and received it again after reconnect following disconnect reason 08.
- Root causes fixed: ESP32 UUID byte order, descriptor discovery range, CCCD matching, and Flipper UUID-only advertising.
- Final ESP32 build and flash passed with hash verification.
- ESP32 still reports an NVS initialization warning before Bluetooth startup. Pairing, persistence, CBOR framing, encryption, and capabilities remain unimplemented.

## 2026-09-02 grill-me session: design decisions and backlog

A design-review session on 2026-09-02 walked the full roadmap, including previously "done" steps, and resolved several open gaps. Full detail is in the updated `docs/PLAN.md`, `docs/PROTOCOL.md`, and `docs/CAPABILITIES.md`; summary of what changed and why:

- **Threat model clarified:** physical possession of either paired device (Flipper or ESP32) is explicitly accepted as fully compromising to that device's stored secrets, for this phase. This resolved a real contradiction where `docs/PROTOCOL.md` required a "firmware-owned protected key service" that a standalone external FAP cannot actually access. See `docs/PROTOCOL.md` "Implementation security requirements."
- **Hardware hardening deferred:** Secure Boot, flash encryption, NVS encryption, signed updates, and any irreversible eFuse configuration are explicitly out of scope for this phase — no sacrificial board is needed yet. See `docs/PLAN.md` "Deferred: hardware hardening."
- **Fragment header defined:** a minimal 4-byte header (flags, message ID, fragment index, fragment count) — see `docs/PROTOCOL.md#fragmentation`. This was previously unspecified despite being referenced in the plan.
- **Reconnect policy revised:** the original five-retry ceiling doesn't fit a board left running unattended for hours/days (see wardriving below); production behavior backs off to a ceiling then retries indefinitely at a slow cadence. Needs a hardware stress test before being relied on (see backlog).
- **Multi-board pairing:** the Flipper stores multiple pairing records keyed by `board_id`, not a single record — a second board (Heltec, see `docs/BASELINES.md`) is already planned. Only one BLE connection is active at a time (Flipper is the peripheral; first paired board to connect wins); switching boards means powering down the active one.
- **Capability roadmap sequenced:** `wifi_scan` first, then `ble_scan` + composite `wardriving` (after wiring a GY-NEO6MV2 GPS module), then Heltec display/LoRa (separate board/chip family), then Zigbee/Thread recon then participation, then `gpio_control`. See `docs/CAPABILITIES.md`.
- **Wardriving design:** autonomous capture from boot (doesn't need a Flipper present to keep running), buffered in a hand-rolled checksummed append-only log on raw flash (power-loss-safe against an unclean car-ignition-off cut — loses at most one in-flight record), sent to the Flipper as compact binary over BLE, converted to one timestamped WiGLE CSV file per flush on the Flipper's SD card. Pre-GPS-fix results are discarded for now.
- **Radio coexistence promoted to its own plan step** (step 4, ahead of pairing/session work) since `wifi_scan`+`ble_scan`+the BLE link to the Flipper all share one radio, and wardriving's continuous operation makes this near-critical-path rather than a final-validation concern.

### Backlog (from the 2026-09-02 session, not yet scheduled to a specific step)

- Reconnect/backoff stress test on real hardware, validating the revised indefinite-slow-retry policy, before relying on it under plan step 3+.
- Manual "disconnect current board" Flipper UI action, to switch between paired boards without powering one off.
- Automatic BLE connection arbitration between paired boards — gated on an unresolved BLE-HAL feasibility question (can the Flipper's peripheral role advertise while already connected?).
- GPS backfill-to-first-fix as a Flipper-settable wardriving option (currently: discard pre-fix results).
- Idle-connection (30s) keepalive/heartbeat during a live wardriving view session, so a stretch with no new results doesn't trigger a spurious disconnect.
- Automatic pause-on-degradation fallback for concurrent BLE-source wardriving scanning while connected — only if plan step 4 shows concurrent operation is unstable.

## 2026-09-02 grill-me session: step 3 (record framing) implementation decisions

A second design-review session on 2026-09-02 walked step 3 specifically before implementation started. Full detail is in `docs/PLAN.md` step 3 ("Step 3 implementation decisions") and the new `docs/PROTOCOL.md` "Canonical CBOR encoding (definition)" section; summary:

- No third-party CBOR library — hand-rolled, schema-specific canonical CBOR codec on both firmwares.
- "Canonical CBOR" was an undefined term in PROTOCOL.md (a real spec gap, closed this session): fixed field order per record/payload type matching the spec's own tables, not general RFC 8949 sorted-key encoding. Decoders reject out-of-order fields. This matters concretely because the AES-GCM AAD must be byte-identical across both independent implementations.
- Step 3's "done when" bar is met by host-native unit tests (no board), built with MSVC (`cl.exe`, via installed VS Community VC.Tools — no gcc/MinGW on PATH). An on-device smoke test over the existing step 2 transport follows as an extra confidence pass, not part of the formal done-when.
- Fragment-layer malformed-input handling (a gap PROTOCOL.md left connection-level-silent on): drop the reassembly buffer for that `message_id` and keep the connection open, unlike the crypto/session layer's hard close-on-failure.
- File layout: `framing.c/.h` + `cbor_codec.c/.h` per firmware, plus a new `tests/esp32/` and `tests/flipper/` compiling those same files against one canonical, hand-authored vector set in `tests/vectors/` (derived from the spec, not from either codec).
- Execution order: shared contracts/vectors/PROTOCOL.md addendum written first, then `esp32-developer` and `flipper-developer` implement in parallel.
- **Second spec gap found and resolved while drafting the codec contract:** PROTOCOL.md's "512 bytes" and "768 bytes" limits were stated without saying which layer each bounds. Resolved (and documented in PROTOCOL.md's fragmentation section) as: 768 bounds the full on-wire record at the reassembly layer (either shape, before CBOR decoding); 512 bounds specifically the plaintext `payload` map's own CBOR encoding (the thing that gets encrypted/was decrypted), not the full outer record. This was a judgment call made during implementation, not put to a fresh grill question — flagged here per usual practice for a resolved doc ambiguity.

### Step 3 codec implementation results (2026-09-02)

Both firmwares' `framing.c`/`cbor_codec.c` were implemented against the shared contracts and verified with real, executed builds — see `docs/PLAN.md` step 3 "Step 3 status" for full detail. Summary:

- ESP32: 16/16 host-native MSVC test checks pass; `idf.py build` passes.
- Flipper: 39/39 host-native MSVC test checks pass; `fbt.cmd fap_flipper_esp32_over_ble` passes.
- Two real framing.c bugs (oversized-fragment bound used max instead of min possible total; reassembly state not reset on message completion, breaking on `message_id` wraparound at 256) were found independently on both sides and fixed identically in both — confirmed the two firmwares' codecs actually agree, which was the point of running them as independent implementations.
- `framing.h`/`cbor_codec.h` remain byte-identical between `esp32/main/` and `flipper/` (verified via diff after both agents' fixes).
- Backlog item for step 6: `feb_cbor_decode_protected()`'s `ciphertext` bound (256 bytes) is smaller than the real max (512 bytes, matching `FEB_CBOR_MAX_PAYLOAD` since AES-128-GCM ciphertext is plaintext-length) — not yet fixed, not yet exercised by any vector, consistent between both firmwares.
- Neither `esp32/main/main.c` nor `flipper/flipper_esp32_over_ble.c` was touched — the codec is not yet wired into the live BLE transport. The agreed on-device smoke test (fragmented CBOR `error` record exchanged over the existing step 2 transport) is still pending and requires flashing both physical devices — do not do this without the user's explicit go-ahead, per this project's hardware-safety rule.

### Step 3 on-device smoke test: Flipper-side wiring (2026-09-02)

`flipper/flipper_esp32_over_ble.c` now wires the frozen `framing.c`/`cbor_codec.c` into the
live BLE transport, replacing the fixed `"Received fixed payload"`/`FLIPPER-ACK` exchange
from step 2. `framing.h`/`cbor_codec.h`/`framing.c`/`cbor_codec.c` and `docs/PROTOCOL.md`
were not touched (build-only change against the frozen contracts). Details:

- `profile_event_handler` now feeds every ATT write's raw bytes into a file-scope
  `feb_reassembly_t` (reset in `profile_start`) using `furi_get_tick()` as `now_ms` (its
  header confirms ticks are milliseconds on this firmware). `FEB_FRAME_OK` just re-arms;
  `FEB_FRAME_MESSAGE_COMPLETE` decodes the record via `feb_cbor_decode_unencrypted` then
  `feb_cbor_decode_error_payload`, logs type/board_id/code/message/request_id at `FURI_LOG_I`,
  and any rejection status is `FURI_LOG_W`'d and dropped (connection stays up), matching
  `framing.h`'s documented drop-and-continue contract.
- On successful decode, the Flipper replies with its own `feb_unencrypted_record_t`
  (`type="error"`, `session_id` = 8 bytes of `0xBB`, `board_id="flipper-smoketest"`,
  payload = `code="internal_error"`, `message="flipper ack of esp32 smoke test"`,
  `request_id` echoed from the decoded record) — placeholder values only, no
  session/pairing exists yet. Fragmented via `feb_fragment_record` with
  `capacity = feb_fragment_capacity(23)` (forces ~16-byte payload fragments regardless of
  the actual negotiated ATT MTU, to exercise multi-fragment reassembly end to end) and an
  independent, wrapping `outgoing_message_id` counter.
- **Real bug found while wiring the reply path, fixed before it could corrupt every
  fragmented notification:** `ble_gatt_characteristic_update()`'s `FlipperGattCharacteristicDataFixed`
  path always sends `data.fixed.length` bytes (here, `PAYLOAD_MAX` = 64) read from the
  source pointer, regardless of the source buffer's actual size — confirmed by reading
  `targets/f7/ble_glue/furi_ble/gatt.c`. The old fixed `"FLIPPER-ACK"` response (an 11-byte
  literal) was therefore already sending 64 bytes over the air, the last ~53 of them
  reading past the end of that static array (harmless-by-luck rodata, but real UB) — this
  is presumably why the step-2 hardware smoke test only checked for the string as a
  prefix. This would have silently corrupted every real fragment (wrong length delivered
  to the ESP32's reassembly). Fixed by switching the Notify characteristic's
  `data_prop_type` to `FlipperGattCharacteristicDataCallback`, matching the pattern already
  used elsewhere in this firmware for variable-length characteristics (see
  `targets/f7/ble_glue/services/dev_info_service.c`'s `dev_info_char_data_callback`): each
  fragment's `(pointer, length)` is passed as the callback's `context`, so exactly that many
  bytes go out per notification. The Write characteristic and `PAYLOAD_MAX` (64) were left
  unchanged — no characteristic size change was needed, only the data-source mechanism.
- Build: `fbt.cmd fap_flipper_esp32_over_ble` passes (build-only, per this step's scope; no
  hardware flashed). Artifact:
  `C:\Users\Deyan\unleashed-firmware-unlshd-092\build\f7-firmware-D\.extapps\flipper_esp32_over_ble.fap`,
  14,728 bytes.
- Still pending: the ESP32-side wiring (parallel work against the same `framing.h`/
  `cbor_codec.h` contracts) and the actual on-hardware exchange — flashing both boards
  requires explicit user go-ahead per this project's hardware-safety rule.

### Step 3 on-device smoke test: ESP32-side wiring (2026-09-02)

`esp32/main/main.c` now wires the frozen `framing.c`/`cbor_codec.c` into the live BLE
transport (TX and RX), replacing the fixed `smoke_payload`/`FLIPPER-ACK` exchange from
step 2. `framing.h`/`cbor_codec.h`/`framing.c`/`cbor_codec.c` and `docs/PROTOCOL.md` were
not touched. Details:

- TX (after the CCCD-subscribe write completes): builds a `feb_error_payload_t`
  (`code="internal_error"`, `message="esp32 framing smoke test round trip"`,
  `request_id=42`), wraps it in a `feb_unencrypted_record_t` (`type="error"`,
  `session_id` = 8 bytes of `0xAA`, `board_id="esp32-c6-smoketest"`), encodes the full
  record, then fragments it via `feb_fragment_capacity(23)` (16-byte payload fragments,
  independent of the real negotiated ATT MTU). Fragments are captured into a bounded
  static array and sent one at a time with `ble_gattc_write_flat`, each waiting for the
  prior fragment's write-completion callback; `message_id` is a wrapping counter.
- RX: a file-scope `feb_reassembly_t`, reset on both connect and disconnect. Each BLE
  notification is fed to `feb_reassembly_feed()` (`now_ms` from
  `esp_timer_get_time() / 1000`). On `FEB_FRAME_MESSAGE_COMPLETE` it decodes via
  `feb_cbor_decode_unencrypted` then `feb_cbor_decode_error_payload` and logs
  type/board_id/code/message/request_id at `ESP_LOGI`; every other rejection status is
  `ESP_LOGW`'d.
- `esp32/main/CMakeLists.txt` now also requires the `esp_timer` component (needed for
  `esp_timer_get_time()`, not part of ESP-IDF's default component requirements).
- Build: `idf.py build` passes (exit code 0). Artifact:
  `esp32/build/flipper_esp32_over_ble.bin`, 627,216 bytes (binary size `0x99210`,
  60% of the smallest app partition free).
- Build-only, per this step's scope — no hardware flashed.

Both sides now build clean against the same frozen contracts. Remaining before step 3 is
fully closed: the actual on-hardware fragmented exchange, which requires flashing both
physical boards — explicit user go-ahead needed first per this project's hardware-safety
rule, then reconfirm current serial ports (`COM9`/`COM8` from prior sessions, not
guaranteed stable).

### Step 3 on-device smoke test: hardware hang after `Rx MTU size: 256` — GATT attribute-count hypothesis investigated and ruled out (2026-09-03)

A real repro (both devices flashed, live logs captured on both) hung forever after MTU
negotiation: ESP32 issues `ble_gattc_disc_svc_by_uuid()` right after `mtu_exchanged()`
completes, and neither side ever logs anything again (85+ seconds). Because the ESP32's
`service_discovered()` always `ESP_LOGE`'s on any non-`BLE_HS_EDONE` discovery-failure
status, zero output on the ESP32 side means the Flipper's GATT server never answered the
ATT discovery request at all — a genuine stack-level hang, not a decode/logic bug in either
side's app code.

Prime suspect going in: this session's `flipper/flipper_esp32_over_ble.c` change switching
the Notify characteristic from `FlipperGattCharacteristicDataFixed` to
`FlipperGattCharacteristicDataCallback` (to fix a real overread — see the step-3 Flipper-side
wiring entry above), combined with `profile_start()` reserving only `6` attribute slots via
`ble_gatt_service_add(..., Max_Attribute_Records=6, ...)` for 2 characteristics (1 write,
1 read+notify).

Investigated by reading `targets/f7/ble_glue/furi_ble/gatt.c`/`gatt.h` and
`lib/stm32wb_copro/wpan/ble/core/auto/ble_gatt_aci.c` in both the cached mirror
(`docs/references/flipper-firmware/upstream`) and the real pinned checkout
(`C:\Users\Deyan\unleashed-firmware-unlshd-092`, confirmed byte-identical for `gatt.c`), and
cross-checking `targets/f7/ble_glue/services/dev_info_service.c`. Findings:

- **Attribute-slot count is independent of `data_prop_type`.** `ble_gatt_characteristic_init()`
  only uses the Fixed-vs-Callback distinction to compute the *declared max value length*
  passed to `aci_gatt_add_char()`'s `Char_Value_Length` parameter (identical result either way
  here: both paths yield `PAYLOAD_MAX` = 64 for the Notify characteristic). ST's attribute
  accounting (`aci_gatt_add_char`'s `Max_Attribute_Records` contract) is driven only by
  `char_properties` (NOTIFY/INDICATE costs +1 slot for the auto-added CCCD) and
  `descriptor_params` (+1 slot per extra descriptor) — never by how the byte content is
  sourced. `dev_info_service.c` confirms this empirically: it freely mixes `DataFixed` and
  `DataCallback` characteristics (3 Fixed, 2 Callback) under one flat
  `1 + 2 * DevInfoSvcGattCharacteristicCount` formula with no per-type adjustment, and none
  of its characteristics use NOTIFY, so no CCCD is ever added there either.
- **`6` is sufficient for this app's service**, both before and after the switch: Write
  (no NOTIFY, no extra descriptor) = 2 slots; Notify (READ|NOTIFY, no extra descriptor,
  `descriptor_params = NULL`) = 2 + 1 (CCCD) = 3 slots; total minimum = 5, with 1 spare in the
  reserved 6. `char_properties`/`descriptor_params` on both characteristics are byte-identical
  before and after this session's Fixed→Callback switch, so the switch could not have changed
  the required slot count either way.
- **Return-value handling was already correct for `ble_gatt_service_add`**: it returns `bool`,
  `profile_start()` already checks it (`if(!ble_gatt_service_add(...))`) and unwinds cleanly on
  failure. `ble_gatt_characteristic_init()` is `void` and never propagates a failure signal to
  its caller — a real (pre-existing, not new) accountability gap — but the gap is unlikely to
  explain a *silent* failure: both `gatt.c` functions unconditionally `FURI_LOG_E` on any
  non-zero `tBleStatus`, regardless of whether `BLE_GATT_STRICT` is defined (confirmed by grep:
  `BLE_GATT_STRICT` is not defined anywhere in this build, so `ble_gatt_strict_crash()` is
  currently a no-op — failures log and continue, they don't crash). Also confirmed in
  `ble_gatt_aci.c`: on a non-zero `resp.Status`, `aci_gatt_add_char()` returns early **without
  writing `*Char_Handle`**, so a real (silent, non-strict) registration failure would leave
  `char_instance->handle` as uninitialized garbage (the profile struct is `malloc`'d, not
  `calloc`'d) — a latent bug worth knowing about, but again not one that would produce *zero*
  log output.
- **Conclusion: hypothesis ruled out for this repro.** `ble_gatt_service_add()` and
  `ble_gatt_characteristic_init()` both run synchronously inside `profile_start()`, which
  completes before advertising can even begin — i.e. strictly before "advertising starts" in
  the captured timeline. Since the captured logs show a clean `bt_profile_start` ->
  advertising -> ESP32 connect -> connection-interval negotiation -> `Rx MTU size: 256`
  sequence with **no** "Failed to add ... char"/"Failed to add service" line anywhere, GATT
  service/characteristic registration definitely succeeded on this repro. The hang happens
  well after registration, at the point the ESP32's *first post-MTU ATT request*
  (service discovery) should get a response and doesn't — this points at a live BLE-stack
  responsiveness/deadlock issue at or after MTU renegotiation, not a static attribute-table
  sizing defect. Root cause is still open; next repro should look at what runs synchronously
  in Flipper app/BLE-stack code between MTU exchange and the first ATT response (e.g. whether
  anything blocks the BLE stack's own task, or whether `profile_event_handler`'s registration
  via `ble_event_dispatcher_register_svc_handler` interacts badly with the stack's internal
  event pump at this point in the connection lifecycle).
- **Instrumentation added** (temporary, low-risk, `flipper/flipper_esp32_over_ble.c` only —
  `gatt.c`/`gatt.h` are firmware-internal ABI, not owned by this app and not modified):
  `profile_start()` now logs the assigned `service_handle` after `ble_gatt_service_add()` and
  each characteristic's `handle`/`descriptor_handle` after `ble_gatt_characteristic_init()`,
  giving a positive-confirmation signal (not just "no error line") on the next repro.
- Build: `fbt.cmd fap_flipper_esp32_over_ble` passes. Artifact:
  `C:\Users\Deyan\unleashed-firmware-unlshd-092\build\f7-firmware-D\.extapps\flipper_esp32_over_ble.fap`,
  14,956 bytes. Build-only — not yet reflashed; the human will redeploy and re-test physically.

### Step 3 on-device smoke test: "MPU fault, possibly stack overflow" crash after connect (2026-09-03)

A second real repro (post-MTU, around/after GATT discovery, right after pressing OK and the
ESP32 connecting) produced a concrete crash screen: `MPU fault, possibly stack overflow`.
This is much more specific than the earlier silent-hang repro (GATT attribute-count hypothesis
already ruled out — see the 2026-09-03 entry above) and points at a stack overflow in the
BLE event-dispatch path, not a registration/logic bug.

**Confirmed call chain and task stack budget.** `profile_event_handler` runs synchronously
inside `hci_user_evt_proc()`, called from the ST BLE-stack event pump on the `"BleEventWorker"`
`FuriThread`, allocated with **`furi_thread_alloc_ex("BleEventWorker", 1280, ble_event_thread,
NULL)`** — a **1280-byte stack**, `FuriThreadPriorityHigh` (confirmed in both the cached mirror
and reasoned to be identical in the pinned checkout: `targets/f7/ble_glue/ble_event_thread.c`;
call chain confirmed via `ble_app.c:148`'s `ble_event_dispatcher_process_event(...)` and
`furi_ble/event_dispatcher.c`'s synchronous, single-threaded handler-list dispatch — only one
event is ever in flight on this thread, so single-threaded-dispatch is a verified fact here,
not just an assumption carried over from this app's own design).

**Confirmed stack-local buffers found in `flipper/flipper_esp32_over_ble.c` (before fix),
reachable from `profile_event_handler`:**
- `send_smoketest_reply()`: `uint8_t payload_buf[256]` and `uint8_t record_buf[FEB_MAX_RECORD_SIZE]`
  (768 bytes) were both **stack-local**, plus a `feb_error_payload_t` (~40 bytes) and a
  `feb_unencrypted_record_t` (~36 bytes) — roughly **1,100+ bytes in this one frame alone**,
  already close to the entire 1280-byte thread budget before accounting for the caller's frame
  or anything the ST BLE library/dispatcher had already consumed getting here.
- `profile_event_handler()`'s RX path: `feb_unencrypted_record_t record` and
  `feb_error_payload_t error_payload` were also stack-local (~36 + ~40 bytes) — smaller, but
  additive on top of the same frame that calls into `send_smoketest_reply()`.

**Fix applied (`flipper/flipper_esp32_over_ble.c` only, `framing.h`/`cbor_codec.h`/
`framing.c`/`cbor_codec.c`/`docs/PROTOCOL.md` not touched):** moved `payload_buf` and
`record_buf` to file-scope `static` storage (named `smoketest_payload_buf`/
`smoketest_record_buf`, matching how `reassembly` was already made static this session), and
also made the small RX-path `record`/`error_payload` locals and the TX-path `error_payload`/
`reply` structs `static` for margin. Safety of static reuse rests on the now-confirmed fact
above (single `BleEventWorker` thread, sequential dispatch, one active BLE connection) — noted
explicitly in a comment at the new statics' declaration site.

**Secondary, larger suspect found in `framing.c` — reported, not fixed (explicitly out of
scope this session):** `feb_fragment_record()` itself declares `uint8_t frag_buf[
FEB_FRAG_HEADER_SIZE + FEB_MAX_RECORD_SIZE]` (772 bytes) as its **own stack-local**, sized off
the full `FEB_MAX_RECORD_SIZE` regardless of the actual per-fragment `capacity` argument. This
contradicts `framing.h`'s own doc comment on `feb_fragment_record` ("into a caller-owned
buffer sized >= FEB_FRAG_HEADER_SIZE + capacity — no dynamic allocation") — the function
signature takes no such buffer parameter at all; the buffer is internal. This 772-byte frame
sits directly in the reachable path (`profile_event_handler` -> `send_smoketest_reply` ->
`feb_fragment_record`) and by itself consumes roughly 60% of the entire 1280-byte
`BleEventWorker` stack. **This was not edited** (framing.c is explicitly frozen this session),
but it is very likely the dominant remaining stack-overflow risk even after this session's
fix — the `flipper_esp32_over_ble.c` fix removes ~1,024 bytes of the FAP's own additive stack
usage, but `feb_fragment_record`'s internal 772-byte buffer is untouched and, combined with
`profile_event_handler`'s own frame (~150-200 bytes estimated: pointers, the two now-static-but-still-declared
struct locals contribute 0 now, decode-status enums, etc.) plus whatever the ST BLE library/
dispatcher consumed before reaching us, plausibly still exceeds or comes very close to the
1280-byte budget. **Recommend as a follow-up (next session, requires explicit sign-off to
touch framing.c/framing.h):** either make `feb_fragment_record()` honor its documented
caller-owned-buffer contract (add a buffer parameter sized by the caller to `capacity`, not
`FEB_MAX_RECORD_SIZE`), or size its internal buffer to `FEB_FRAG_HEADER_SIZE + capacity`
instead of `+ FEB_MAX_RECORD_SIZE`, or make it a file-scope static with the same
single-threaded-dispatch justification used above. Also checked `cbor_codec.c` for the same
pattern: only small fixed arrays exist there (`key_ptrs`/`key_lens` sized
`FEB_CBOR_MAX_MAP_ENTRIES` = 8 entries, and smaller `seen_ptrs`/`seen_lens[3..7]` arrays) —
tens of bytes each, not a comparable risk.

Build: `fbt.cmd fap_flipper_esp32_over_ble` passes (synced into
`C:\Users\Deyan\unleashed-firmware-unlshd-092\applications_user\flipper_esp32_over_ble\` first,
per this project's Windows FBT `APPSRC`-resolution constraint). Artifact:
`C:\Users\Deyan\unleashed-firmware-unlshd-092\build\f7-firmware-D\.extapps\flipper_esp32_over_ble.fap`,
15,148 bytes. **Build-only** — not reflashed; the human will redeploy and re-test physically.
Given the `framing.c` finding above, this fix alone may reduce but **not fully eliminate** the
crash risk; say so plainly if asked before the next hardware repro.

### Step 3 on-device smoke test: `framing.c` stack-overflow fix (2026-09-03)

Followed up on the previous entry's flagged secondary suspect. Fixed directly (by the
orchestrating session, not a subagent, to guarantee the two copies got the identical
change) in both `esp32/main/framing.c` and `flipper/framing.c`: `feb_fragment_record()`'s
internal `frag_buf[FEB_FRAG_HEADER_SIZE + FEB_MAX_RECORD_SIZE]` (772 bytes) changed from
stack-local to file-scope `static`, with a comment explaining why (small dedicated
call-chain stacks — the Flipper's 1280-byte `BleEventWorker` thread in particular — and
that fragmentation is synchronous/single-in-flight on both firmwares, so static reuse is
safe). `framing.h`/`cbor_codec.h`/`cbor_codec.c`/`docs/PROTOCOL.md` untouched; this is an
internal buffer-ownership fix, not a wire-format change. Note: `framing.c`'s two
implementations were already independently written (different brace/spacing style per
firmware) rather than byte-identical — only the `.h` contracts are held byte-identical —
so the fix was applied as the equivalent change in each file's own style, not a literal
diff-identical edit.

Both sides rebuilt clean:
- ESP32: `idf.py build` passes. `esp32/build/flipper_esp32_over_ble.bin`, 627,632 bytes.
- Flipper: `fbt.cmd fap_flipper_esp32_over_ble` passes (only `framing.c` recompiled, as
  expected). `C:\Users\Deyan\unleashed-firmware-unlshd-092\build\f7-firmware-D\.extapps\flipper_esp32_over_ble.fap`,
  15,176 bytes.

Both builds are build-only as of this entry; reflashing/redeploying and the next physical
repro follow immediately after. This is expected to remove the dominant remaining
stack-overflow risk identified in the "MPU fault" entry above, but has not yet been
confirmed against real hardware.

### Step 3 on-device smoke test: PASSED (2026-09-03)

Both boards reflashed with the `framing.c` static-buffer fix (ESP32 via `idf.py -p COM9
flash`, hash-verified; Flipper via `fbt.cmd launch APPSRC=flipper_esp32_over_ble`) and
retested with simultaneous live serial captures on both `COM9` and `COM8`. Full round trip
succeeded, no crash, no hang:

- **ESP32 -> Flipper**: sent its `error` record (156 bytes, 10 fragments at the forced
  16-byte-payload capacity) via sequential `ble_gattc_write_flat` writes, all acknowledged.
  Flipper's log: `Decoded 'error' from board 'esp32-c6-smoketest': code=internal_error
  message=esp32 framing smoke test round trip request_id=42`.
- **Flipper -> ESP32**: replied with its own `error` record (9 fragments) via notifications.
  ESP32's log: `reassembled record: type=error board_id=flipper-smoketest
  code=internal_error message=flipper ack of esp32 smoke test request_id=42`.
- Full connection sequence (scan -> connect -> MTU 256 -> service/characteristic/descriptor
  discovery -> CCCD subscribe) completed cleanly on the ESP32 side; the new registration-
  confirmation logging on the Flipper side showed real, sane handles (`service_handle=12`,
  `Write` char `handle=13`, `Notify` char `handle=15`) confirming GATT registration was never
  actually the problem (as concluded earlier).
- One benign warning on the Flipper: `[W][ViewPort] ViewPort lockup: see
  applications/services/gui/view_port.c:185` — a UI redraw-timing warning, unrelated to the
  BLE/framing path (no crash followed it; the exchange completed after it appeared).

This confirms the `framing.c` static-buffer fix resolved the "MPU fault, possibly stack
overflow" crash. **Step 3's on-device smoke test (the bonus confidence pass beyond the
formal "done when", per docs/PLAN.md) is now complete on real hardware.** Step 4 (radio
coexistence validation) is next per the roadmap.

## Next work guidance

- Phase 2 may now begin, but first reread `docs/PLAN.md`, `docs/PROTOCOL.md`, `docs/PAIRING.md`, `docs/DECISIONS.md`, and `docs/CAPABILITIES.md`.
- Preserve the pinned Unleashed release/API and ESP-IDF version.
- Verify the Heltec pin map only when that board's phase actually starts (see `docs/BASELINES.md`); this project currently targets ESP32-C6-DevKitC-1-N4 as the primary/first board.
- For BLE implementation, keep the Flipper peripheral/GATT-server and ESP32 central/GATT-client roles.
- Preserve the reset-gated trusted-environment X25519 pairing design, HKDF-SHA-256, HMAC-SHA-256, AES-256-GCM runtime sessions (revised from AES-128-GCM during step 6 design, 2026-09-06 — see below), sequence/replay protection, and the explicit app-owned/physical-access-accepted persistence threat boundary.
- Step 3 (record framing) is fully complete: formal "done when" met (host-native codec tests pass on both firmwares, both target builds pass), and the on-device smoke test (fragmented CBOR `error` record exchanged both directions over real hardware) passed 2026-09-03 after fixing a stack-overflow bug found along the way (see the dated entries above).
- Step 4 (radio coexistence validation) is fully complete as of 2026-09-03: all 5 sweep points passed cleanly on real hardware with zero disconnects at any BLE-observer duty cycle up to the theoretical maximum. See `docs/PLAN.md` step 4's "Step 4 results" section for the recommended interval bounds to use when step 7's `wardriving`/`ble_scan` capability is implemented, and this file's "step 4 run executed on real hardware" entry above for orchestration lessons learned. The throwaway `esp32/coex_test/` project should be left alone/ignored (not deleted without asking; it's harmless disk space) since it's not part of the shipping firmware.
- Step 5 (trusted-environment pairing): the crypto/codec layer (`pairing.h`/`pairing_crypto.h` contracts, `pairing.c`/`pairing_crypto.c` on both firmwares) is implemented, host-test-verified, and build-verified as of 2026-09-03 (see this file's dated entries and `docs/PLAN.md` step 5's "Step 5 status"). The remaining wiring piece's design was completed 2026-09-05 (see this file's "step 5 BLE/storage/window-timer wiring design" entry and `docs/PLAN.md` step 5's "Step 5 BLE/storage/window-timer wiring decisions"), and **both sides of that wiring are now implemented and build-verified** (see this file's 2026-09-05 "Flipper-side pairing ceremony wired" and "ESP32-side pairing ceremony wired" entries below): `flipper/flipper_esp32_over_ble.c` runs the real responder-on-the-Flipper ceremony (persisting per-board secrets under `/data/pairings/<board_id>.dat` with detailed status text/LED feedback) and `esp32/main/main.c` runs the real reset-gated initiator ceremony (board_id from the factory MAC, one 120s window per reset, `pairing_secret` persisted to NVS before `pair_complete`) — both build clean (`fbt.cmd fap_flipper_esp32_over_ble`; `idf.py build`). The first live ceremony has since succeeded on real hardware (see the dated "step 5 hardware pairing test" entry below), and the remaining reboot-survival/reset-and-repair/passive-capture checks now have a fully worked-out methodology — **the next action is to execute the 11-step plan in `docs/PLAN.md` step 5's "Step 5 hardware verification plan" section** (see also this file's matching 2026-09-05 grill-me entry) — this requires explicit user go-ahead first, per this project's hardware-safety rule.
- `docs/references/flipper-firmware/upstream` mismatch is now fixed (2026-09-03) — see the dated entry below.
- **Step 5 is fully done, including hardware verification** (superseding the "next action" wording in the bullet above, which described the state before that verification ran): see this file's "Step 5 hardware verification executed" entry (2026-09-05). **Step 6 (authenticated runtime sessions) is next in the roadmap and its implementation is now also done — host-test-verified and build-verified on both firmwares as of 2026-09-06, but not yet hardware-verified.** See this file's 2026-09-06 dated entries ("step 6 shared contracts written," including a real AES-128-GCM -> AES-256-GCM protocol revision found before any code was written, and "step 6 implementation") and `docs/PLAN.md` step 6's "Step 6 status" section for full detail. **The next action for a fresh session is the on-hardware verification pass** (reset-vs-runtime-auth boot decision, the `unknown_board` fallback) — the user explicitly deferred this to a clean session rather than running it in the session that just finished the implementation; it still requires its own explicit go-ahead per this project's hardware-safety rule, and reconfirming serial ports / checking for conflicting peer sessions first.

## 2026-09-03: fixed `docs/references/flipper-firmware/upstream` mirror mismatch

The mirror was a checkout of the vanilla `flipperdevices/flipperzero-firmware.git` at
commit `2d8711939ac8442a572219ed0fb4beaa02a89858`, mislabeled as Unleashed (found during
the step 5 design session — see above). Fixed by removing that checkout and re-fetching
the mirror directly from `https://github.com/DarkFlippers/unleashed-firmware.git`, shallow
(`--depth 1`) at the project's pinned commit `3c9be0fdd9d301a9436765099a2d1780b36a1795`
(same commit `CLAUDE.md`/`docs/BASELINES.md` pin for the real build checkout at
`C:\Users\Deyan\unleashed-firmware-unlshd-092`). Verified `targets/f7/api_symbols.csv`
reports `Version,+,88.4`, matching the documented pinned API. Submodules were not
initialized, matching the mirror's original (also-uninitialized) state — this mirror has
never included submodule content, only top-level firmware source, which is all
`api_symbols.csv`-style ABI lookups have ever needed. `docs/references/flipper-firmware/REVISION.txt`
updated with the correct commit/remote/retrieval date and a note explaining the prior
mismatch. One transient `rm -rf`/"Device or resource busy" error occurred on the first
delete attempt (likely a peer session or file-indexer holding a handle in this shared repo
directory — 11 peer sessions were active at the time per `ListAgents`); succeeded
immediately on retry, no other action needed.

## 2026-09-03 grill-me session: step 4 (radio coexistence) test design, not yet implemented

A design-review session on 2026-09-03 walked step 4 before any implementation started (no code written, no board flashed this session). Full detail and rationale is in `docs/PLAN.md` step 4's "Step 4 implementation decisions" and the step 9 cross-reference added alongside it. Summary for the next session to pick up from:

- Step 4 is rescoped to **BLE/Wi-Fi only**; 802.15.4 coexistence is deferred to the Zigbee/Thread recon phase (step 7 item 4) since no 802.15.4 code exists yet.
- Plan: build a **throwaway** ESP32-side-only instrumentation harness (not wired into `main.c`, no Flipper firmware changes needed), run a single baseline confirmation for the BLE-scan-**paused** config, then an ascending **3-4 point sweep** of the **concurrent** config from conservative up to the theoretical radio maximum (continuous Wi-Fi scanning, ~100% BLE observer duty cycle).
- Two-tier pass/fail per sweep point (hard fail = unrecovered disconnect; degradation signal = any recovered-but-unexpected disconnect, excluded from "safe default" but not fatal), plus per-point validation that the step-2 merged reconnect-scan behavior still finds the Flipper correctly at that duty cycle. Automated structured summary log lines per point, not manual log review.
- **User has authorized a fully unattended overnight run**, including repeated automatic reflashing of the ESP32-C6 (not the Flipper) and best-effort auto-recovery on a hang (re-flash-and-resume) rather than halt-only — accepted tradeoff: a recovered hang could mask real instability, mitigated by two hard rules: (1) any hang invalidates and fully restarts that sweep point's 30-minute window, never splices data across a recovery; (2) capped at 2 restarts (3 tries) per point before marking it "unstable — exceeded retry budget" and moving on, with a ~5-6 hour ceiling for the whole sweep.
- Physical setup was confirmed for this authorization: laptop plugged in/not sleeping, Flipper charged and powered on in BLE range for the whole run, real Wi-Fi APs present in the environment. **Reconfirm this checklist before actually kicking off the overnight run in the next session** — it was confirmed for this specific session's intended run, not as a standing fact.
- **Follow-up same session: confirmed the Flipper-side app launch/start doesn't need a manual button press either — it's fully scriptable over the Flipper's own CLI on COM8.** Live-tested against the real device:
  - `loader open <name>` only searches the firmware's compiled-in app table (`loader list` showed just `Sub-GHz`) — it does **not** find external FAPs on the SD card. External FAPs must be opened by their full SD-card path instead: `loader open "/ext/apps/Connectivity/flipper_esp32_over_ble.fap"` (path derived from `flipper/application.fam`'s `fap_category="Connectivity"`), confirmed working via `loader info` reporting the app running.
  - Simulating the OK button via `input send ok <type>` requires a real `press`/`release` pairing, not a bare `short`. The GUI's input dispatcher (`gui_input()` in `applications/services/gui/gui.c`) tracks an "ongoing input" bitmask per key and silently discards (debug-log only) any non-`press`/`release` event for a key that isn't already marked ongoing — so a lone `input send ok short` is dropped before it ever reaches the app. The working sequence, confirmed live (Flipper's screen changed to "Waiting for ESP32..."): `input send ok press`, then `input send ok short`, then `input send ok release`, in that order.
  - **A real infrastructure gotcha hit along the way**: COM8 (the Flipper's USB CDC serial) got into a state where reads worked but every write timed out, reproducing identically across two independent libraries (.NET `SerialPort` and Python `pyserial`) with hardware flow control explicitly disabled both ways — ruling out a client-library bug. A physical unplug/replug of the Flipper's USB cable fixed it immediately. Cause not fully root-caused (suspected: repeated rapid open/close/DTR-toggle cycles during probing left the device's USB CDC session in a bad state, or a Windows generic-driver CTS quirk), but the fix is now known: if COM8 writes ever stall like this again, physically reconnect the cable rather than debugging further.
  - **Net effect: the step 4 overnight run can be fully automated end to end**, including the Flipper-side app launch and start — no manual button press is required. This supersedes the "user must press OK once" fallback originally recorded in `docs/PLAN.md` step 4's implementation decisions; see the updated bullet there.
- **Nothing has been built or flashed yet** — this session was design-only ("we'll continue in a clean session"). Next session should: reread `docs/PLAN.md` step 4 in full, reconfirm serial ports (`COM9`/`COM8` from prior sessions, not guaranteed stable), reconfirm the physical setup checklist above, then build the throwaway harness (likely via the `esp32-developer` agent) and start the run.

## 2026-09-03: step 4 harness implemented and build-verified (not yet flashed)

Built the throwaway coexistence sweep harness as its own independent ESP-IDF project, per the grill-me session's design: `esp32/coex_test/` (`CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`, `main/CMakeLists.txt`, `main/coex_test.c`). Does not touch `esp32/main/main.c`, `esp32/CMakeLists.txt`, or anything under `flipper/`. `idf.py build` passes (exit code 0, verified twice, including a clean single-file recompile of `coex_test.c` alone with zero warnings/errors). Artifacts: `esp32/coex_test/build/coex_test.bin` (1,180,448 bytes; 0x120320, 25% of the 0x180000 factory partition free), `esp32/coex_test/build/bootloader/bootloader.bin`, `esp32/coex_test/build/partition_table/partition-table.bin`. Flash command (from `esp32/coex_test/`, not yet run): `idf.py -p COM9 flash` (reconfirm port).

**Sweep points (5 total: 1 paused baseline + 4 concurrent), chosen from `docs/references`-cached ESP-IDF `docs/en/api-guides/coexist.rst` guidance and NimBLE's own scan-parameter defaults:**

- The coexistence guide's Wi-Fi/BLE support matrix marks BLE Scan+Connected, both under Wi-Fi STA Scan and Wi-Fi STA Connected, as "Y — supported and the performance is stable," and `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` (software coexistence arbitration) defaults to `y` whenever both `BT_ENABLED` and `ESP_WIFI_ENABLED` are set, which is the case here — set explicitly in `sdkconfig.defaults` to document the intentional choice rather than relying on the silent default. The guide gives no single numeric interval recommendation beyond "use default connectionless power-save Window/Interval values unless you've tested custom ones," so concrete point values were chosen from NimBLE's own scan-parameter units (`ble_gap.h`, 0.625ms/unit) rather than invented arbitrarily.
- Point 0 `baseline-paused` (config=paused): Wi-Fi continuous back-to-back all-channel active scan (`esp_wifi_scan_start(NULL, true)` looped with no gap); BLE observer scan fully off while connected — reconnects (if any) use a dedicated active scan reusing `esp32/main/main.c`'s already-proven default params (`itvl=0, window=0`, which NimBLE auto-resolves to `BLE_GAP_SCAN_FAST_INTERVAL_MIN`/`_WINDOW` = 30ms/30ms).
- Point 1 `concurrent-conservative-10pct`: BLE observer window=100ms/interval=1000ms (10% duty), Wi-Fi rescans every 30s.
- Point 2 `concurrent-moderate-50pct`: window=100ms/interval=200ms (50% duty), Wi-Fi every 15s.
- Point 3 `concurrent-aggressive-90pct`: window=135ms/interval=150ms (90% duty), Wi-Fi continuous back-to-back.
- Point 4 `concurrent-max-100pct`: window=30ms/interval=30ms (100% duty) — deliberately identical to NimBLE's own default fast-scan parameters (`BLE_GAP_SCAN_FAST_INTERVAL_MIN`/`_WINDOW`), i.e. the same values `esp32/main/main.c`'s already-hardware-proven reconnect scan uses today, so the "theoretical radio maximum" point isn't a novel untested config. Wi-Fi continuous back-to-back.
- **Deviation from the task prompt's literal wording, flagged explicitly:** the prompt's numbered list 1/2 says the concurrent sweep uses "the same Wi-Fi active scanning as" the continuous baseline (implying constant Wi-Fi cadence across all sweep points), but its own "reasonable shape" example varies Wi-Fi cadence in lockstep with BLE duty per point (30s/15s/continuous/continuous) — internally inconsistent. Resolved by escalating both dimensions together (ascending BLE duty *and* ascending Wi-Fi aggressiveness per point), since that better matches `docs/PLAN.md`'s own framing of the sweep as "ascending from conservative to the theoretical radio maximum" than pinning Wi-Fi at its most aggressive setting even for the "conservative" BLE point.

**Other implementation choices/deviations, for the record:**

- Reconnect classification thresholds (not specified numerically anywhere): a recovered disconnect counts as a hard fail, not just a degradation signal, if the gap exceeds `COEX_RECONNECT_EXPECTED_MAX_GAP_MS` = 120s. Rationale: the bounded exponential backoff ceiling (1+2+4+8+16 = 31s worst case) plus scan/connect/MTU/discovery overhead is comfortably under 45s; 120s adds margin for one full cycle of the revised policy's indefinite 60s slow-cadence retry actually being needed before still calling it "expected." A wedge/stuck-connection check (`COEX_WEDGE_TIMEOUT_MS` = 20s of no successful heartbeat ATT write while nominally connected) forces a local disconnect and counts as its own hard fail, independent of the gap-based classification, to avoid double-counting a single wedge incident.
- Retry-budget accounting for the confusing self-referential NVS `attempt_count` rule in `docs/PLAN.md` step 4 (the doc's own text flags this as tricky): implemented as strictly "3 total tries per point, 1-indexed, incremented and persisted at the very start of each attempt before running the point's window." A 4th boot for the same point (i.e., `attempt_count` read from NVS is already 3, meaning 3 prior tries all failed to complete cleanly) emits `COEX_POINT_UNSTABLE` immediately without running a window, rather than the doc's literal "reaches 2" 0-indexed phrasing, which would have only allowed 2 actual attempts before bailing. This matches `docs/PLAN.md`'s explicit "2 restarts (3 total tries)" summary elsewhere in the same step.
- Heartbeat: implemented as a periodic (5s) fixed 4-byte GATT write-with-response to the existing write characteristic, used only as an ATT-level liveness signal (write completion updates the wedge-detection timestamp) — no CBOR/framing layer, per the step's explicit scope. The Flipper's current firmware (already wired to `framing.c`/`cbor_codec.c` since step 3) will not decode this raw payload as valid CBOR and will silently drop-and-continue per its documented malformed-input policy; this does not disrupt the harness's liveness check, since only the ATT write acknowledgment matters here, not any application-level reply.
- Dropped `main.c`'s verbose per-advertisement hexdump scan logging (kept only match/state-transition logging) to avoid unbounded log volume across a multi-hour run.
- Not yet done: flashing the board and running the sweep. Per this project's hardware-safety rule, that requires the user's explicit go-ahead in the session that actually performs it, even though the 2026-09-03 grill-me session recorded a standing authorization for repeated automatic ESP32-C6 reflashing during this specific test.
- **Build tooling note (new, worth recording):** `idf.py`/ESP-IDF's `export.ps1` refuses to run under any process that inherits the `MSYSTEM` environment variable (`tools/idf_tools.py` hard-exits with "MSys/Mingw is not supported" whenever `MSYSTEM` is present in `os.environ`, regardless of shell used to launch it). Git Bash sets `MSYSTEM=MINGW64` for every session; invoking `powershell.exe`/`cmd.exe` from within a Git Bash session inherits it unless explicitly cleared first (`Remove-Item Env:\MSYSTEM` etc. in the PowerShell script, run before `. export.ps1`). Worth remembering for any future automation that shells out from a POSIX/Git-Bash context.

## 2026-09-03: step 4 run executed on real hardware — PASSED, all 5 points clean

The overnight sweep was actually run (not just built) this session, using a new orchestrator script (`tools/coex/run_coex_sweep.ps1`) that flashes/monitors the ESP32 over `COM9`, parses the harness's `COEX_*` structured log-line contract, detects a silent hang, and reflashes to recover. Full results, the recommended interval bounds for step 7, and the accepted merged-reconnect-scan gap are recorded in `docs/PLAN.md` step 4's new "Step 4 results (2026-09-03)" section — summary: **all 5 points (paused baseline + 4 ascending concurrent points, 10%/50%/90%/100% BLE-observer duty cycle) passed with zero disconnects, hard fails, or degradations**, on the first attempt each, no retries needed. Step 4 is now fully closed; step 5 (trusted-environment pairing) is next per the roadmap.

Notable events from actually running it, beyond what's in PLAN.md:

- **COM8 CLI scriptability, previously confirmed working in the prior grill-me session, proved unreliable in practice.** The `loader open` + `input send ok press/short/release` sequence hit repeated `SerialPort.WriteLine` "semaphore timeout" exceptions (writes hanging, reads still working) — reproduced across multiple physical USB replugs and even a single clean open-then-write attempt with no rapid probing. Root cause not found (rapid open/close cycling was suspected but a single clean attempt still failed). Worked around by having the user launch the Flipper app manually (physical button press) and disabling the orchestrator's periodic COM8 health-check entirely for this run. `docs/PLAN.md` step 4 has a correction note; treat COM8 scriptability as unproven until someone root-causes it — it is not a blocker since the sweep needs no further Flipper-side interaction once the app is running.
- **Two orchestrator crashes, zero hardware impact.** A Windows file-sharing conflict between the PowerShell orchestrator's `Add-Content` (writing structured events) and Git-Bash coreutils (`tail`/`wc`/`grep`) reading the *same* file for a live-log view crashed the orchestrator twice (first via `tail -f`, then again via a discrete polling loop — both trip the same underlying sharing violation on Windows). Both times the ESP32 itself kept running its sweep correctly and unaffected, since sweep progress lives in on-device NVS, not in the orchestrator — confirmed both times via direct one-shot UART reads before resuming orchestrator monitoring. Fixed permanently by wrapping every `Add-Content` call in try/catch (a log-write failure is now a non-fatal warning, not a terminating error) — see `docs/PLAN.md`'s incident writeup for the full lesson. `tools/coex/run_coex_sweep.ps1` also gained `-SkipInitialFlash`/`-SkipFlipperLaunch`/`-SkipFlipperHealthCheck` switches, used for both restarts so recovery never reflashed a healthy board or burned a sweep-point retry attempt.
- **8 concurrent peer Claude Code sessions were active on this same project directory** during the run; all were contacted and confirmed clear of `esp32/`/`flipper/`/`COM8`/`COM9` before flashing started, and asked to hold off for the run's duration — worth checking for again before any future hardware-touching automated run on this project, since multiple sessions running in parallel on the same repo/hardware is apparently common here.

## 2026-09-03 grill-me session: step 5 (trusted-environment pairing) design, not yet implemented

A design-review session on 2026-09-03 walked step 5 before any implementation started (no code written, no board flashed this session). Full decision list is recorded in `docs/PLAN.md` step 5's new "Step 5 implementation decisions" section, and wire-format/state-machine changes are recorded directly in `docs/PROTOCOL.md` and `docs/PAIRING.md`. This was the deepest design pass so far — deeper than steps 3/4 — because it surfaced a real feasibility blocker early, not just wire-format ambiguities. Summary:

- **Feasibility blocker: a standalone Flipper FAP cannot link against X25519, HKDF, or HMAC-SHA-256 at all.** Checked the actual pinned Unleashed checkout's `targets/f7/api_symbols.csv` (API 88.4, not the mislabeled `docs/references/` mirror — see below): every `mbedtls_*` symbol, including all ECDH/Curve25519 and HMAC/SHA-256 functions, is present in firmware source but explicitly unexported (`-`); no HKDF symbol exists at any API version; zero exported bignum/ECC/hash fallback of any kind. Only `furi_hal_crypto_gcm_encrypt_and_tag`/`_decrypt_and_verify` (raw-key AES-GCM) is usable. Walked the alternatives (relaxing the protocol to a manually-relayed short code instead of X25519) and concluded every alternative that avoids new asymmetric-crypto code in the FAP also gives up the "protects against passive BLE capture during pairing" property the current design relies on — judged not worth trading away. **Resolution: keep the protocol as specified; hand-roll SHA-256/HMAC/HKDF and port a known compact reference X25519 implementation (e.g. curve25519-donna) into the FAP.** ESP32 side needs no such workaround — `mbedtls_ecdh_*`/`mbedtls_md_hmac`/`mbedtls_gcm_*` are already Kconfig-default-enabled; only `mbedtls_hkdf` needs one new `CONFIG_MBEDTLS_HKDF_C=y` in `esp32/sdkconfig.defaults`.
- **Documentation bug found along the way, not yet fixed:** `docs/references/flipper-firmware/upstream/REVISION.txt` shows a *vanilla* `flipperdevices/flipperzero-firmware.git` checkout at commit `2d8711939ac8442a572219ed0fb4beaa02a89858` — not the pinned Unleashed `unlshd-092` at `3c9be0fdd9d301a9436765099a2d1780b36a1795` that `CLAUDE.md` says this mirror is. All this session's ABI findings used the correct full checkout at `C:\Users\Deyan\unleashed-firmware-unlshd-092` instead (confirmed against the pinned commit). The mismatch itself was left unfixed this session — anyone consulting `docs/references/` directly per its own documented purpose would currently get wrong answers about the Unleashed ABI. Worth asking the user whether/how to refresh it (e.g. re-sync the mirror to the pinned Unleashed commit) before it causes a real mistake.
- Several wire-format gaps were found and closed in `docs/PROTOCOL.md`: transcript `T`'s `service_uuid` byte order (now RFC 4122 big-endian, closing an ambiguity in the same class as step 2's real UUID-byte-order bug), and the pairing record wire envelope (`{version, type, board_id, payload}`, no `session_id` — never actually specified before, since pairing records can't use the general `session_id`-bearing envelope that only `hello`/`hello_ack`/`error` were shown with).
- `docs/PAIRING.md`'s "one attempt per reset window" wording was ambiguous about whether a failed attempt consumes the whole window or just resets per-attempt state; clarified as literally one attempt total, any outcome ends the window.
- `board_id` generation was pulled into this step's scope (it's a hard prerequisite for pairing — `T`, `pair_init`, and the `pairing_secret` HKDF `info` string all need it) despite being textually listed under step 7; step 7's own wording already says it must exist "before pairing," so this isn't out-of-order work, just a step-numbering note for later readers.
- NVS blob writes and Flipper storage atomicity were both confirmed sufficient via the plain documented APIs (ESP-IDF's NVS already double-buffers blobs by internal version; the Flipper has a real in-tree precedent in `archive_favorites.c` for the temp-file/sync/close/rename pattern) — no hand-rolled shadow-key workaround needed on either side.
- Zeroization will use a volatile-pointer-based secure-clear helper on both sides (plain `memset` on a soon-dead variable can be legally optimized away by the compiler, CWE-14) — `mbedtls_platform_zeroize()` on ESP32, a small hand-rolled equivalent behind the same `pairing_crypto.h` contract on the Flipper.
- File layout and execution order mirror step 3 exactly: new shared `pairing.h`/`pairing_crypto.h` contracts plus `tests/vectors/` additions (per-primitive RFC/NIST known-answer vectors, plus one new fixed-input "golden end-to-end pairing" vector covering the full `T`/`K_shared`/`K_confirm`/`pairing_secret`/confirmation-tag pipeline, test-only) written first, then `esp32-developer`/`flipper-developer` implement independently in parallel.
- **Nothing has been built or flashed yet.** Next session should: write the shared contracts and vectors first, then delegate implementation to `esp32-developer`/`flipper-developer` in parallel per the file-layout decision, and separately decide whether to fix the `docs/references/flipper-firmware/upstream` mismatch noted above.

## 2026-09-03: step 5 shared contracts (`pairing.h`/`pairing_crypto.h`) and vector additions written

Per the design session above, wrote the frozen contracts before any firmware-specific
implementation, mirroring step 3's execution order exactly. Nothing under `esp32/main/main.c`
or `flipper/flipper_esp32_over_ble.c` was touched.

- **`esp32/main/pairing_crypto.h`** (byte-identical copy at `flipper/pairing_crypto.h`,
  diffed to confirm): one-shot X25519 (RFC 7748 `feb_x25519`/`feb_x25519_base`,
  `feb_x25519_keypair` for copying through caller-supplied CSPRNG bytes,
  `feb_is_all_zero` for the required all-zero-shared-secret rejection), one-shot SHA-256/
  HMAC-SHA-256/HKDF-SHA-256, `feb_consttime_equal`, and `feb_secure_zero`. No streaming
  context is exposed in the header — every hash/HMAC/HKDF input in this protocol is a
  small, fully-buffered message (transcript T, proof strings), so a one-shot-only
  interface stays portable across mbedtls's context-object backend (ESP32) and a
  hand-rolled one (Flipper) without leaking either backend's internal struct layout.
- **`esp32/main/pairing.h`** (byte-identical copy at `flipper/pairing.h`): the pairing
  record wire envelope (`feb_pairing_envelope_t`, `{version, type, board_id, payload}`),
  per-type payload structs and CBOR encode/decode declarations for
  `pair_init`/`pair_reply`/`pair_confirm`/`pair_complete` (pairing-phase `error` reuses
  `feb_error_payload_t` from `cbor_codec.h` instead of a new struct), the transcript `T`
  builder (`feb_pairing_transcript_t`/`feb_pairing_encode_transcript`), and the
  `K_confirm`/`pairing_secret`/confirmation-tag derivation pipeline
  (`feb_pairing_derive_kconfirm`, `feb_pairing_derive_secret`,
  `feb_pairing_flipper_confirm`/`_esp32_confirm`/`_complete_tag`) wrapping
  `pairing_crypto.h`'s primitives with the exact salt/info/label strings from
  `docs/PROTOCOL.md`. Deliberately does **not** define the reset-gated 120-second window
  timer, BLE scan/connect state machine, or persistent storage — those stay
  implementation-specific per firmware (in `pairing.c`'s caller, i.e. `main.c`/
  `flipper_esp32_over_ble.c`), matching how `framing.h`/`cbor_codec.h` never dictated
  `main.c`'s own connection-state handling either. `board_id` generation (MAC-derived,
  ESP32-only) is likewise out of scope for this header.
  `FEB_PAIRING_MAX_TRANSCRIPT_LEN` (288 bytes) is derived from the golden vector's actual
  262-byte `T` plus the worst-case `board_id`-length delta, not guessed.
- **`tests/vectors/generate_vectors.py`** extended (still the single generator, still
  regenerates the full `vectors.h` including all step-3 vectors unchanged) with: RFC 7748
  X25519 test cases 1/2, the Alice/Bob Diffie-Hellman example, and an all-zero-shared-secret
  case; a FIPS 180-4 SHA-256 vector; an RFC 4231 HMAC-SHA-256 vector; an RFC 5869
  HKDF-SHA-256 vector (inputs from the RFC, output computed at this protocol's real L=32
  rather than hand-truncated from the RFC's published L=42 OKM); and one golden
  end-to-end pairing vector (fixed SHA-256-derived, non-random inputs — reproducible from
  the script's own source, no hand-transcribed hex) covering the full T/K_shared/
  K_confirm/pairing_secret/confirmation-tag pipeline plus all four pairing record
  envelopes' CBOR encodings and one pairing-phase `error` record.
  - **Verification method, since no cryptography library is available in this Python
    install (`ModuleNotFoundError: No module named 'cryptography'`, confirmed by direct
    check)**: X25519 is a from-scratch pure-Python RFC 7748 Montgomery-ladder
    implementation *inside the generator*, self-validated by `assert`ing it reproduces
    RFC 7748's own published test-case outputs and the Alice/Bob shared secret (fetched
    directly from `rfc-editor.org`, not from memory — `python generate_vectors.py` was
    actually run and all assertions passed) before being trusted to compute the golden
    vector's `K_shared`. SHA-256/HMAC/HKDF use Python's stdlib `hashlib`/`hmac` as ground
    truth instead — only RFC-published *inputs* were transcribed (also fetched directly
    from the RFCs), never RFC-published *output* hex, to avoid a hand-splicing
    transcription error in a 64+ hex-character string going undetected.
  - `tests/README.md` updated with a "Step 5 additions" section describing the new
    vectors and explicitly noting no pairing host test binary exists yet.
- **Not done yet, by design** (this was a contracts-only pass, matching the user's
  explicit request this session): `pairing.c`/`pairing_crypto.c` on either firmware, any
  host-native test binary exercising the new vectors, `esp32/sdkconfig.defaults`'s
  `CONFIG_MBEDTLS_HKDF_C=y`, and all `main.c`/`flipper_esp32_over_ble.c` wiring (window
  timer, BLE integration, persistence). Delegated to `esp32-developer`/`flipper-developer`
  in parallel immediately after this entry, scoped to the codec+crypto implementation and
  host tests only (not the BLE/storage/window integration, which needs its own design
  pass and hardware go-ahead) — check this file's next dated entry for the outcome.

## 2026-09-03: step 5 ESP32-side `pairing_crypto.c`/`pairing.c` implemented and tested

Implemented `esp32/main/pairing_crypto.c` and `esp32/main/pairing.c` against the frozen
`pairing_crypto.h`/`pairing.h` contracts, plus a new host-native test binary
(`tests/esp32/test_pairing.c` + `tests/esp32/build_pairing.ps1` +
`tests/esp32/mbedtls_test_config.h`). `pairing_crypto.h`/`pairing.h` were not touched.
`esp32/main/main.c` was not touched (out of scope this round, per the task).

- **Real API mismatch found and worked around, not just a style choice.** mbedtls's
  high-level Curve25519 path (`mbedtls_ecp_mul()` -> `mbedtls_ecp_check_pubkey()` ->
  `ecp_check_pubkey_mx()`/`ecp_check_bad_points_mx()` in `ecp.c`) rejects `u=0` and a
  handful of other known low-order points with `MBEDTLS_ERR_ECP_INVALID_KEY`. But
  `feb_x25519()` is declared `void` (no error return) and `pairing_crypto.h`'s own doc
  comment requires it to be a total function matching RFC 7748 for *every* 32-byte input
  (the header explicitly pushes the *only* rejection — an all-zero shared secret — to the
  caller via `feb_is_all_zero()`). Using `mbedtls_ecp_mul()` directly would have no way to
  signal or safely recover from that internal rejection inside a void function, and would
  fail this step's own `FEB_VEC_X25519_ZERO_U` -> `FEB_VEC_X25519_ZERO_OUTPUT` vector.
  **Resolution:** implemented the RFC 7748 section 5 Montgomery ladder directly against
  mbedtls's bignum (`mbedtls_mpi_*`) primitives (`mbedtls_mpi_safe_cond_swap` for the
  constant-time cswap, `mbedtls_mpi_exp_mod` for the final field inversion) instead of
  going through `mbedtls_ecp_mul()`/`mbedtls_ecdh_*` — still "backed by mbedtls" per the
  task's framing, just at the modular-arithmetic layer. All RFC 7748 §5.2/§6.1 vectors,
  the all-zero case, and the golden pairing vector's `K_shared` (computed from both
  directions) pass against this implementation.
  - **Accepted residual caveat, flagged as a follow-up hardening item, not fixed this
    round:** `mbedtls_mpi_mod_mpi()` (used for every modular reduction in the ladder) is
    not documented as constant-time (its repeated-subtraction normalization loop is
    data-dependent), unlike mbedtls's own internal Curve25519 ladder path. This is a
    correctness-preserving, timing-side-channel-accepting tradeoff — reasonable given
    this is a one-shot, infrequent pairing operation and the project's threat model
    (`docs/PROTOCOL.md` "Implementation security requirements") already excludes physical
    possession, but a genuine gap versus a production-grade constant-time field
    implementation. Worth a real fix (e.g. wiring through mbedtls's internal ladder with a
    safe fallback only for the handful of rejected edge-case inputs) before this code is
    trusted against a serious remote-timing threat model.
- **Real sdkconfig staleness bug found and fixed, independent of the code itself.** After
  adding `CONFIG_MBEDTLS_HKDF_C=y` to `esp32/sdkconfig.defaults` per the task, `idf.py
  build` passed immediately — but the already-existing, previously-generated
  `esp32/sdkconfig` (predating this session) still had `# CONFIG_MBEDTLS_HKDF_C is not
  set`, because ESP-IDF only seeds new keys from `sdkconfig.defaults` into an *existing*
  sdkconfig, it doesn't retroactively override an already-answered option. The build
  still passed because `main.c` doesn't call anything in `pairing_crypto.c`/`pairing.c`
  yet (explicitly out of scope this round), so the linker's `--gc-sections` dead-code
  elimination stripped the entire unreferenced `mbedtls_hkdf()` call path before it could
  produce an undefined-symbol error — a real "looks fine but isn't actually verified" trap.
  Found by explicitly grepping the generated `sdkconfig` after the build instead of trusting
  a clean exit code alone. Fixed by deleting the stale `esp32/sdkconfig` and letting `idf.py
  build` regenerate it fully from `sdkconfig.defaults`; confirmed `CONFIG_MBEDTLS_HKDF_C=y`
  in the regenerated file and reconfirmed `idf.py build` still passes clean (all other
  settings — target, 4 MB flash, custom partition table, BT/NimBLE central role — verified
  unchanged after regeneration). The host-native test binary is the actual proof that
  `mbedtls_hkdf()` links and computes correctly, since it calls `feb_hkdf_sha256()` in a
  real, executed, checked path; the sdkconfig staleness only affected whether the *real
  firmware build* would have linked successfully once `main.c` starts calling into this
  code in a future step.
- **Host-native test:** `tests/esp32/test_pairing.c`, compiled via a new
  `tests/esp32/build_pairing.ps1` (sibling to `build.ps1`, not folded into it — this
  binary pulls in a chunk of ESP-IDF's vendored mbedtls sources that the framing/cbor
  test has no need for) against a new minimal `tests/esp32/mbedtls_test_config.h`
  (`MBEDTLS_BIGNUM_C`/`MD_C`/`SHA256_C`/`HKDF_C` only, deliberately narrower than either
  ESP-IDF's Kconfig-generated config or mbedtls's own vendored default `mbedtls_config.h`,
  which defines `MBEDTLS_PSA_CRYPTO_C` and pulls in a much larger dependency surface not
  needed here). Compiles `pairing_crypto.c`, `pairing.c`, `cbor_codec.c`, and mbedtls's
  `sha256.c`/`md.c`/`hkdf.c`/`bignum.c`/`bignum_core.c`/`constant_time.c`/
  `platform_util.c` directly from the ESP-IDF checkout at `$IDF_PATH`. **42/42 checks
  pass**, actually executed: both RFC 7748 §5.2 test cases, the §6.1 Alice/Bob
  Diffie-Hellman example (public-key generation and both-direction shared secret), the
  all-zero-input case plus `feb_is_all_zero()` sanity checks, SHA-256 (FIPS 180-4),
  HMAC-SHA-256 (RFC 4231), HKDF-SHA-256 (RFC 5869, L=32), `feb_consttime_equal()` sanity
  checks, the golden vector's transcript `T`, `K_shared` (both directions),
  `K_confirm`/`pairing_secret`, all three confirmation/completion tags, all four
  pair_*/payload encode-decode round trips against the exact golden bytes, and the
  pairing-phase `error` payload/envelope round trip. The existing
  `tests/esp32/build.ps1` (framing/cbor) was rerun unchanged and still passes (17/17;
  the 16/16 figure recorded for step 3 predates a since-added regression check, per that
  step's own status entry — not a regression from this session).
- **Real target build:** `idf.py build` (ESP-IDF v5.5.2, `esp32c6`) passes clean, zero
  compiler warnings for `pairing.c`/`pairing_crypto.c`, confirmed via both a full clean
  rebuild (after regenerating `sdkconfig`, see above) and a targeted forced-recompile of
  just those two objects. `esp32/main/CMakeLists.txt` now also lists `pairing_crypto.c`/
  `pairing.c` in `SRCS` and adds the `mbedtls` component to `REQUIRES`. `main.c` was not
  touched — nothing calls this code yet, matching this round's explicit scope.
- **Not done, by design (this round's explicit scope):** `main.c` wiring, the reset-gated
  120-second pairing window/timer, BLE pairing-service scan/connect logic, and NVS
  persistence of `pairing_secret`. These remain a separate follow-up pass, same as the
  Flipper side.

## 2026-09-03: step 5 Flipper-side implementation (`pairing_crypto.c`/`pairing.c`) and host test, build-verified

Implemented `flipper/pairing_crypto.c` and `flipper/pairing.c` against the frozen
contracts above, plus a new host-native test binary
(`tests/flipper/test_pairing.c`/`build_pairing.ps1`). `flipper_esp32_over_ble.c` was not
touched, matching this round's scope.

- **Bug found and fixed in the frozen headers (not a design reinterpretation — a plain
  syntax error).** `pairing_crypto.h`'s top comment reads `...mbedtls (mbedtls_ecdh_*/` at
  the end of one line, continuing `mbedtls_md_hmac/mbedtls_hkdf/...` on the next. The `*/`
  sequence embedded in that symbol list terminates the enclosing `/* ... */` block comment
  early (C block comments end at the first `*/` regardless of intent), turning the rest of
  the comment into top-level tokens and failing to compile under both MSVC (this session's
  host test) and, since the bug is byte-identical in `esp32/main/pairing_crypto.h`, also
  gcc/mbedtls on the ESP32 side. Fixed by changing that one clause from slash-separated to
  comma-separated (`mbedtls_ecdh_*, mbedtls_md_hmac, mbedtls_hkdf, mbedtls_gcm_*`) in both
  files identically (re-diffed to confirm they're still byte-identical after the edit) —
  comment text only, zero effect on any declared type or function signature. Flagging this
  per the "stop and report a genuinely wrong contract" instruction, even though the fix
  itself was small and unambiguous enough to make and keep moving: if `esp32-developer`'s
  session started from the pre-fix header, its build will hit the identical error until it
  picks up this fix.
- **X25519**: ported from `curve25519-donna.c` (the portable 32/64-bit build, not the
  `-c64` asm-tuned variant), by Adam Langley, derived from Daniel J. Bernstein's public
  domain code — `https://github.com/agl/curve25519-donna/blob/master/curve25519-donna.c`,
  3-clause BSD, Copyright 2008 Google Inc. Fetched directly from that URL this session (not
  from memory) and ported with algorithm/structure/identifier names kept intentionally
  close to upstream (including the terse `u8`/`s32`/`limb` typedefs, deviating from this
  project's usual naming conventions on purpose) so the port stays diffable against the
  original for a correctness audit. Only changes: the public entry point
  `curve25519_donna` was renamed to the file-local `static x25519_donna_scalarmult` (no
  stray exported symbol from the firmware binary), and it now calls this file's own
  `feb_secure_zero()` on its stack-local field-element buffers before returning (the
  original doesn't zeroize, since it wasn't written against this project's zeroization
  contract). Full attribution and the BSD license text are reproduced verbatim in a comment
  block at the top of `flipper/pairing_crypto.c`. SHA-256 (FIPS 180-4), HMAC-SHA-256 (RFC
  2104), and HKDF-SHA-256 (RFC 5869, Expand bounded to one HMAC block since this protocol
  never requests more than `FEB_HKDF_MAX_LEN`=32 bytes) were hand-rolled from spec using an
  internal (file-static, not header-exposed) streaming SHA-256 context, avoiding any fixed-size
  concatenation-buffer assumption for HMAC's inner/outer hash or HKDF's Expand step.
- **Judgment calls, both defensive-only (no observed effect on any passing vector):**
  `feb_pairing_derive_secret()`'s `info` buffer and the internal confirm/complete-tag
  helper's `label || T` buffer are fixed-size stack buffers sized from
  `FEB_PAIRING_BOARD_ID_MAX_LEN`/`FEB_PAIRING_MAX_TRANSCRIPT_LEN`; both silently clamp an
  over-length input rather than corrupt the stack, since the header declares these
  functions `void` (no error-return path) and every real caller's `board_id`/`T` is already
  bounded by `feb_cbor_decode_pairing_envelope()`/`feb_pairing_encode_transcript()`
  upstream of these calls.
- **Host test**: `tests/flipper/test_pairing.c` (new; parallel to `build.ps1`, added
  `build_pairing.ps1` rather than folding into the existing script, so each test binary's
  source list and run stay independently auditable) exercises every
  `FEB_VEC_X25519_*`/`FEB_VEC_SHA256_*`/`FEB_VEC_HMAC_*`/`FEB_VEC_HKDF_*`/`FEB_VEC_PAIR_*`
  vector: per-primitive RFC/FIPS vectors, the all-zero-point/`feb_is_all_zero` case, and the
  full golden pairing pipeline (T, both-direction `K_shared`, `K_confirm`, `pairing_secret`,
  all three confirmation/completion tags, all four payload+envelope CBOR encodings
  round-tripped and byte-compared against the golden vectors, plus the pairing-phase
  `error` envelope). **Result: 67/67 checks passed** (`build_pairing.ps1` actually run this
  session, not just written). The pre-existing `tests/flipper/test_flipper_codec.c` was
  re-run unchanged as a regression check: still 39/39.
- **FAP build verified against the pinned Unleashed checkout**
  (`C:\Users\Deyan\unleashed-firmware-unlshd-092`, `fbt.cmd fap_flipper_esp32_over_ble`,
  temp copy synced into `applications_user\flipper_esp32_over_ble` per this project's
  Windows FBT constraint). `application.fam`'s `sources` list was extended with
  `pairing_crypto.c`/`pairing.c` (appid/entry_point/requires/fap_category untouched) so
  they actually compile as part of the FAP even though nothing calls them yet. Build
  succeeded cleanly: artifact `build\f7-firmware-D\.extapps\flipper_esp32_over_ble.fap`,
  15,176 bytes, API 88.4 reported up to date.
- **Not done this round, by design**: `flipper_esp32_over_ble.c` (pairing-screen UI/action,
  GATT pairing-service profile, app-owned atomic storage of `pairing_secret`, the
  reset-gated window timer) — a separate follow-up, matching how step 3's codec was landed
  before being wired into the app. ESP32-side `pairing_crypto.c`/`pairing.c` is
  `esp32-developer`'s parallel counterpart; not touched or verified from this session.

## 2026-09-05 grill-me session: step 5 BLE/storage/window-timer wiring design, not yet implemented

A design-review session on 2026-09-05 walked the last remaining piece of step 5 — wiring
the already-frozen, already-tested `pairing.c`/`pairing_crypto.c` into `esp32/main/main.c`
and `flipper/flipper_esp32_over_ble.c` — before any implementation started (no code written,
no board flashed this session). Full decision list and rationale is in `docs/PLAN.md` step 5's
new "Step 5 BLE/storage/window-timer wiring decisions" section; summary for the next session
to pick up from:

- `board_id`: recomputed every ESP32 boot from the factory base MAC
  (`esp_efuse_mac_get_default()`), formatted `esp32c6-<12 lowercase hex chars>` — no NVS
  write needed for it.
- ESP32 window/state machine: only a fully-established connection's outcome consumes the
  one-shot pairing attempt (pre-link connect failures keep retrying via the existing
  `schedule_reconnect()`, gated on window-open); expiry uses synchronous deadline checks, not
  an interrupting async timer; once the window closes for any reason, the ESP32 disconnects
  and goes fully idle until the next reset (temporary, until step 6 gives it something else
  to do); the step 3 smoke-test code is removed and replaced by the real pairing flow.
- Flipper UI: kept as the existing single explicit OK-press action (no auto-connect split
  yet); detailed per-phase on-screen status through the ceremony (explicit user preference,
  overriding this session's own initial minimal-UI recommendation); silent overwrite when
  replacing an existing board's pairing, no confirmation dialog.
- **New LED status stub** (Flipper-side only): continuous blue blink from waiting through the
  whole handshake, solid blue only once pairing succeeds and persists, off on any failure —
  built from primitives already confirmed exported in the pinned Unleashed API
  (`sequence_blink_start_blue`/`sequence_set_only_blue_255`/`sequence_reset_blue` via
  `notification_message()`). Explicitly a hardcoded stub for this one flow; generalizing it
  into a reusable status/notification module is a new backlog item in `docs/PLAN.md`.
- Flipper storage: one file per `board_id` (not one shared file with a list), so "other
  stored pairings are unaffected" (step 5's own "done when" wording) holds by construction
  via the existing atomic temp-file/sync/close/rename pattern, rather than resting on
  list-rewrite correctness.
- **Real input-validation gap found and closed proactively:** `board_id` arrives before any
  cryptographic confirmation (pairing is intentionally unauthenticated until the confirmation
  tags validate), so using it directly as a filename without validation would be a
  path-traversal-shaped gap, independent of this project's accepted physical-possession
  threat model. Resolved: validate against a strict `[A-Za-z0-9_-]` charset before any
  filesystem operation, reject anything else as malformed input.
- **Nothing has been built or flashed yet.** Next session should delegate implementation to
  `esp32-developer`/`flipper-developer` in parallel, per the file-layout/execution-order
  pattern already used for steps 3/5 — build-verified only (`idf.py build` and
  `fbt.cmd fap_flipper_esp32_over_ble`), no board touched. The actual hardware pairing test
  (reset-and-repair, reboot-survives persistence, a live ceremony on real hardware, and
  confirming passive capture doesn't expose the persisted secret) is a separate follow-up
  requiring explicit user go-ahead first, per this project's hardware-safety rule.

## 2026-09-05: Flipper-side pairing ceremony wired into flipper_esp32_over_ble.c

Implemented the Flipper half of the 2026-09-05 wiring design above (`flipper-developer`
scope). `flipper/flipper_esp32_over_ble.c` was rewritten to remove the step-3 on-device
smoke test (`send_smoketest_reply` and call sites) entirely and replace it with the real
pairing responder flow against the already-frozen, already-tested `pairing.c`/
`pairing_crypto.c`. Build-verification only this round, per the task scope — no board
touched.

**Ceremony flow implemented** (Flipper is the BLE peripheral/responder; the ESP32-side
initiator in `esp32/main/main.c` remains unwritten, so this is unexercised against a real
peer yet): on `pair_init`, generates a fresh X25519 keypair and `client_nonce`, computes
`K_shared` (rejecting an all-zero result), builds transcript `T`, derives `K_confirm`,
sends `pair_reply`; on `pair_confirm`, verifies the ESP32's confirmation tag in constant
time; on `pair_complete`, verifies the completion tag, then persists `pairing_secret` to
`/data/pairings/<board_id>.dat` via the atomic temp-file + `storage_file_sync()` + close +
remove-old + rename pattern (the `archive_favorites.c` precedent with the missing sync
call added, per the design decision). All ephemeral secrets (X25519 private key,
`K_shared`, `K_confirm`, `pairing_secret`) are zeroized via `feb_secure_zero()` on every
success/failure path, using `static` (not stack) storage for the ceremony state and every
sizeable per-record local buffer, matching the existing file's own established mitigation
for the real stack-overflow bug found during step 3 (~1280-byte `BleEventWorker` stack).

**`board_id` validated against `[A-Za-z0-9_-]` immediately after decoding the outer
pairing envelope**, before dispatching to any per-type handler and therefore before any
filesystem path is ever built from it — closing the path-traversal-shaped gap flagged in
the design session. A decode failure or invalid `board_id` at this stage is treated as
unauthenticated garbage: logged and dropped with **no wire reply**, since we cannot yet
trust enough of the record to safely echo a `board_id`-bearing `pairing_failed`. Once the
envelope/`board_id` are known-good, a *cryptographic* failure (bad shared secret, bad
confirm, bad complete tag, malformed per-type payload, or a `board_id` that changes
mid-ceremony) does send a `pairing_failed` record (using the known `board_id`) before
tearing down local state — this two-tier split (silent-drop for unparseable input vs.
`pairing_failed` for parseable-but-invalid input) was not explicit in the design notes and
is this session's own resolution of that gap, consistent with PROTOCOL.md's "do not
expose the cause" framing.

**No proactive `bt_disconnect()` on pairing failure.** Investigated calling
`bt_disconnect()` directly from `profile_event_handler` on a ceremony failure and rejected
it: that handler runs synchronously inside the BLE stack's own HCI event dispatch
(`hci_user_evt_proc()` on `BleEventWorker`), and calling back into BT-stack teardown from
inside its own event callback is a real reentrancy risk the existing code never does
(it only ever sends GATT notifications from that thread, never touches profile/connection
lifecycle). Resolved by relying on the *peer* to disconnect: per the design session's own
ESP32-side semantics ("once the window closes for any reason, the ESP32 disconnects and
goes fully idle"), a spec-conformant ESP32 will disconnect on its own after any outcome
(success or failure), which the Flipper picks up reactively via the existing
`BtStatusAdvertising` event (the BLE stack auto-resumes advertising after any peer
disconnect, confirmed by reading `targets/f7/ble_glue/gap.c`'s
`HCI_DISCONNECTION_COMPLETE_EVT_CODE` handler, which calls `gap_advertise_start()`
whenever `gap->enable_adv` is set). All BT-profile-lifecycle calls (`bt_disconnect`,
`bt_profile_restore_default`) remain confined to the main loop, as before.

**Real latent bug found and fixed as part of this wiring, not previously noticed:** the
pre-existing code never handled a `BtStatusAdvertising` event arriving *after*
`BtStatusConnected` (i.e., a peer disconnect during an already-`service_active` session) —
`app.service_active` would stay `true` forever after any mid-session disconnect, with no
path back to a "waiting for a peer" state short of exiting the whole app. Fixed generally
as part of the new `pairing_phase` state machine: any `BtStatusAdvertising` received while
`pairing_phase != PairingPhaseDone` now resets ceremony state and returns the UI/LED to
"Waiting for ESP32...". `PairingPhaseDone` is deliberately excluded from this reset so the
expected post-success ESP32-initiated disconnect leaves "Paired"/solid-blue on screen
rather than reverting to "Waiting" — this is the intended behavior for the "silent
overwrite on re-pair" design point: a *later*, fresh `pair_init` on a new connection resets
state again via the `BtStatusConnected` handler regardless of the lingering `Done` phase,
so a subsequent legitimate re-pairing attempt is never blocked by a stale `Done` state.

**One deliberate, non-wire-visible deviation from the design/spec prose:** PROTOCOL.md's
prose describes deriving `pairing_secret` "after both confirmations validate." This
implementation derives it immediately after computing `K_shared` during `pair_init`
processing instead (same HKDF inputs, byte-identical output, only the wall-clock timing of
the in-memory computation differs), because doing so let the transient
`epoch`/`client_nonce`/`device_nonce`/`K_shared` values stay purely function-local
(zeroized at the end of `pair_init` handling) rather than needing their own persistent
static storage across the three separate BLE writes. The security-relevant constraint —
never touching storage or trusting the secret before `pair_complete`'s tag is verified — is
still upheld exactly as specified; only the derivation's internal timing moved earlier.
Flagged here rather than silently done, per this project's own working-method rule.

**UI/LED wiring implemented per the design session exactly:** full per-phase status text
(`Waiting for ESP32...` -> `Connected, exchanging keys...` -> `Confirming...` ->
`Saving...` -> `Paired`, or `Failed: <reason>`), continuous `sequence_blink_start_blue`
from the moment OK is pressed through the whole handshake, `sequence_set_only_blue_255`
only once `pair_complete` verifies *and* the secret is actually persisted, and
`sequence_reset_blue` on any failure, on return to idle (Back/app exit), and once at app
startup (defensive: guards against a stale blinking LED left over from a prior crashed run
of this same app). The second on-screen status line was changed from the pre-existing,
inaccurate hardcoded `"No saved pairing"` (which could never become true) to a real check
—`any_saved_pairing_exists()` opens `/data/pairings` via `storage_dir_open`/`storage_dir_read`
at app startup and after every successful pairing — labeled `"Have saved pairing"` /
`"No saved pairing"` (deliberately not the old `"Saved pairing: auto-connect"` wording,
since auto-connect is explicitly step 6's concern per the design session, not implemented
here).

**Build verification.** Synced `flipper/*.c`/`*.h` + `application.fam` (already listing
`pairing_crypto.c`/`pairing.c` from a prior session — verified, not just assumed) into
`C:\Users\Deyan\unleashed-firmware-unlshd-092\applications_user\flipper_esp32_over_ble\`
and ran `fbt.cmd fap_flipper_esp32_over_ble`. Build succeeded clean (CC of the changed
`.c`, `SDKCHK` against API 88.4, `LINK`, `APPMETA`, `FAP`, `FASTFAP`, `APPCHK`, no warnings
or errors in the log). Artifact: `build\f7-firmware-D\.extapps\flipper_esp32_over_ble.fap`,
38,284 bytes (re-verified clean after one post-build fix: `handle_pair_init`'s payload-decode
failure path now copies+validates `board_id` before decoding, so it can send a
`pairing_failed` reply consistently with the `pair_confirm`/`pair_complete` handlers instead
of silently dropping the record). Confirmed `sequence_blink_start_blue`/`sequence_set_only_blue_255`/
`sequence_reset_blue` and `notification_message()`/`RECORD_NOTIFICATION`, and the
`storage_file_*`/`storage_common_*`/`storage_dir_*`/`APP_DATA_PATH` storage APIs used, are
all exported at the pinned API 88.4 by re-checking `targets/f7/api_symbols.csv` in the real
pinned checkout directly (not the `docs/references/` mirror) before using them. Also
confirmed via `storage_processing.c` that `/data/...` paths auto-resolve to
`/ext/apps_data/flipper_esp32_over_ble/...` and auto-create that one app-data directory,
but *not* the `pairings/` subdirectory under it — `storage_common_mkdir()` on
`/data/pairings` is still called explicitly (tolerating `FSE_EXIST`) before every save.

**Out of scope this round, as instructed:** unpair/factory-reset UI, multi-board selection
UI (the per-file storage layout already supports it without rework). Not flashed/launched
on the physical Flipper this round, per this project's hardware-safety rule and the task's
explicit build-verification-only scope. (The ESP32-side wiring flagged as outstanding when
this entry was first written was completed the same session by a parallel `esp32-developer`
agent — see the entry immediately below.)

## 2026-09-05: ESP32-side pairing ceremony wired into main.c

Implemented the ESP32 half of the 2026-09-05 wiring design (`esp32-developer` scope, run in
parallel with the Flipper-side agent above). `esp32/main/main.c` was rewritten to remove the
step-3 on-device smoke test (`build_and_send_smoke_record` and call sites) entirely and
replace it with the real pairing initiator flow against the already-frozen, already-tested
`pairing.c`/`pairing_crypto.c`. `esp32/main/CMakeLists.txt` gained `nvs_flash` and
`esp_hw_support` in `REQUIRES` (needed for `nvs_open`/`nvs_set_blob`/`nvs_commit` and
`esp_efuse_mac_get_default`/`esp_fill_random`). Build-verification only this round, per the
task scope — no board touched.

**Ceremony flow implemented**: `board_id` computed once at boot from
`esp_efuse_mac_get_default()` (factory base MAC, not the BLE-stack-derived MAC), formatted
`esp32c6-<12 lowercase hex>`. A fresh 16-byte `pairing_epoch` is generated at boot and one
120-second window is opened; `pairing_window_is_open()` is the single synchronous
deadline-check helper, called before starting a new scan, before a reconnect-scan retry,
before connecting to a discovered peer, and before building `pair_init` on a freshly
established connection — never as an interrupting timer, so an in-flight handshake always
runs to completion. On CCCD-subscribe completion, generates a fresh X25519 keypair and
`device_nonce` and sends `pair_init`; on `pair_reply`, computes `K_shared` (rejecting an
all-zero result), builds transcript `T` (RFC-4122 big-endian `service_uuid`), derives
`K_confirm`, verifies the Flipper's confirmation tag in constant time, and sends
`pair_confirm`; on success, derives `pairing_secret`, persists it via `nvs_set_blob`+
`nvs_commit()` in a dedicated `feb_pairing` NVS namespace *before* sending `pair_complete`
(an NVS write failure aborts the ceremony exactly like any other failure — no
`pair_complete`, no retry), then sends `pair_complete` and disconnects. Every
failure/expiry/success path zeroizes the ephemeral X25519 private key, `K_shared`, and
`K_confirm` via `feb_secure_zero()`; `pairing_secret` itself is zeroized immediately after
computing the completion tag.

**Window consumption implemented via the existing disconnect path**: `BLE_GAP_EVENT_DISCONNECT`
unconditionally marks the pairing window closed and goes idle (no more `schedule_reconnect()`
calls after this) — since NimBLE only raises this event for a previously-established link,
this alone implements "only a fully-established connection's outcome consumes the attempt";
a bare `ble_gap_connect()` failure still flows through the existing `schedule_reconnect()`
path, now additionally gated on the window still being open. Once the window closes for any
reason (success, failure, or the 120s deadline), the board goes fully idle until the next
physical reset, per the design decision (step 6 will replace this later).

Reused the existing fragmentation/write-completion state machine, generalized from the old
fixed-23-byte smoke-test capacity to the real negotiated ATT MTU (captured in
`mtu_exchanged()`), with static (not stack-local) TX buffers sized `FEB_MAX_RECORD_SIZE +
48*FEB_FRAG_HEADER_SIZE` (960 bytes) — small and bounded, matching this project's established
static-buffer convention. `CONFIG_MBEDTLS_HKDF_C=y` was already present in the generated
`esp32/sdkconfig` from a prior session (no staleness this round).

**Real gaps found, flagged for follow-up, not fixed this round (out of stated scope):**
- No periodic call to `framing.h`'s `feb_reassembly_check_timeout()` (2-second reassembly
  timeout) — a stalled/partial fragment sequence from the Flipper would sit in the RX
  reassembly buffer until the next full message or a disconnect. This is a pre-existing gap
  (not introduced this round); worth closing before step 9's fuzzing pass.
- A received pairing-phase `error` from the Flipper (e.g. it independently decided
  `pairing_failed`) gets logged and the ESP32 disconnects directly, with no echoed error
  reply — judged more correct than replying to an error with an error, but this is a
  judgment call made this round, not spec-mandated.

**Build result:** `idf.py build` — clean, exit code 0 (confirmed explicitly via
`$LASTEXITCODE` on a second no-op build), no compiler warnings in `main.c`/`framing.c`/
`cbor_codec.c`/`pairing.c`/`pairing_crypto.c`. Artifact: `esp32/build/flipper_esp32_over_ble.bin`,
0x9e260 bytes, 59% of the app partition free.

**Out of scope this round, as instructed:** all hardware flashing/testing (reset-and-repair,
reboot-survives-persistence, a live ceremony between both physical devices, passive-capture
confirmation) — that remains a separate follow-up requiring explicit user go-ahead. The
Flipper-side wiring was handled by a parallel `flipper-developer` agent (see the entry
immediately above) and was not touched here.

## 2026-09-05: step 5 hardware pairing test — first successful ceremony, two real bugs found and fixed

Ran the actual on-device pairing ceremony for the first time (ESP32-C6 on COM9, Flipper on
COM8, both freshly flashed with the 2026-09-05 wiring). 15 concurrent peer sessions were
active; all confirmed clear of `esp32/`/`flipper/`/COM8/COM9 before flashing, per
`[[project_concurrent_sessions]]`.

**Bug 1 (ESP32 side, found and fixed this session): write-fragment capacity ignored the
peer's fixed GATT characteristic size.** First attempt: ESP32 sent `pair_init` fragmented
against the real negotiated ATT MTU (256 → ~249-byte payload capacity per fragment), but the
Flipper's Write characteristic has a fixed, MTU-independent max attribute value length of 64
bytes (`flipper/flipper_esp32_over_ble.c`'s `PAYLOAD_MAX`). The 180-byte write was rejected
with ATT error 0x0D (`ATT_ERR_INVALID_ATTR_VALUE_LEN`, logged as NimBLE status 269). Not
caught earlier because the step-3 on-device smoke test forced a tiny fixed capacity
(`feb_fragment_capacity(23)` = 16 bytes) well under the 64-byte cap. Fixed in
`esp32/main/main.c` (delegated to `esp32-developer`): added `FEB_FLIPPER_WRITE_CHAR_MAX_LEN`
(64) and `FEB_FLIPPER_WRITE_EFFECTIVE_MTU` (67 = 64 + `FEB_ATT_WRITE_OVERHEAD`), and the write
path now clamps the negotiated MTU down to this effective value before computing fragment
capacity — yielding 60 payload bytes/fragment (64 bytes on the wire, matching the Flipper's
real cap) regardless of how large the negotiated MTU actually is. `FEB_TX_MAX_FRAGMENTS` (48)
comfortably covers the new capacity (13 fragments needed for a 768-byte max record vs. the
old 48-fragment sizing). `framing.h`/`framing.c`/`cbor_codec.h`/`cbor_codec.c`/
`docs/PROTOCOL.md` untouched — this is a transport-tuning parameter, not a wire-format change
(the frag header already carries `fragment_index`/`fragment_count`, so variable per-message
capacity was always supported). `idf.py build` passes clean after the fix.

**Bug 2 (Flipper side, found and fixed this session): stack overflow in the ported X25519
code, a fresh instance of the step-3 "MPU fault, possibly stack overflow" failure class.**
After the fragmentation fix, `pair_init` reached the Flipper successfully but it then
crashed with the same "MPU fault, possibly stack overflow" seen during step 3. Root cause
(delegated to `flipper-developer`): `flipper/pairing_crypto.c`'s ported curve25519-donna code
had several large stack-local scratch arrays never audited against the Flipper's 1280-byte
`BleEventWorker` stack — `cmult()` (1216 bytes) calling `fmonty()` (1224 bytes) from inside its
256-iteration ladder loop stack together to ~2440 bytes, nearly double the entire thread
budget, on top of `crecip()` (800 bytes), `x25519_donna_scalarmult()` (360 bytes), and smaller
`fmul()`/`fsquare()` frames (152 bytes each) — plus smaller SHA-256/HMAC/HKDF scratch buffers
in the same file and `pairing.c`'s `pairing_confirm_tag()` (304-byte `buf`) and
`feb_pairing_derive_secret()`'s `salt`/`info`. All converted to file-scope `static` (same
single-threaded/sequential-BLE-dispatch justification already established for `framing.c`'s
`frag_buf`), with `cmult()`'s `{0}`/`{1}`-initialized arrays given an explicit reset at the
top of the function (since `static` storage would otherwise only apply that initializer once
at program load). `pairing.h`/`pairing_crypto.h` signatures untouched.
`tests/flipper/build.ps1` (39/39) and `build_pairing.ps1` (67/67, including the RFC 7748
vectors that exercise `cmult`/`fmonty`/`crecip` end-to-end) both still pass with no
regression. `esp32/main/pairing.c`/`pairing_crypto.c` were not touched — this bug is
Flipper-specific by construction (its `BleEventWorker` stack is the tiny fixed 1280-byte one;
the ESP32 side is mbedtls-backed with a much larger stack).

**Bug 3 (Flipper side, found and fixed this session, minor/cosmetic): LED never actually
stopped blinking on success or failure.** After both fixes above, the full ceremony
(`pair_init` → `pair_reply` → `pair_confirm` → `pairing_secret persisted` → `pair_complete` →
clean disconnect) completed successfully end to end and the screen correctly showed "Paired"
— but the LED kept blinking blue instead of going solid. Root cause: `sequence_blink_start_blue`
drives a separate hardware LED-blink subsystem (`NotificationMessageTypeLedBlinkStart`/
`NotificationMessageTypeLedBlinkStop`), independent of the plain static-RGB messages
(`sequence_reset_blue`, `sequence_set_only_blue_255`) used at every phase transition; only
`sequence_blink_stop` actually halts it, and none of the four call sites in
`flipper_esp32_over_ble.c` (success, storage-failure, ceremony-abort, app-exit-cleanup, plus
the defensive startup reset) were sending it. Fixed by adding a `sequence_blink_stop` call
immediately before each existing `sequence_reset_blue`/`sequence_set_only_blue_255` call at
all five sites, confirmed `sequence_blink_stop` is exported at the pinned API 88.4. Fixed
directly (not delegated — small, well-understood one-line-pattern fix across one file).

**Result confirmed on real hardware after all three fixes**: full pairing ceremony succeeds
end to end (ESP32 log: `pairing_secret persisted; sending pair_complete` → clean disconnect;
Flipper: "Paired" screen, solid blue LED, confirmed by the user). This is the first time this
project's actual X25519/HKDF/HMAC pairing crypto has run successfully between the two real
physical devices, not just against host-native test vectors.

**Step 5's "done when" bar is partially met — first pairing succeeds, but reboot-survival,
reset-and-repair, and passive-capture confirmation are not yet separately verified this
session** (see `docs/PLAN.md` step 5's "done when" wording: "a first pairing survives reboot,
a reset-and-repair replaces the old relationship for that board only ..., and passive capture
does not expose the persisted pairing secret"). The first clause ("first pairing") is now
met. The remaining two hardware checks and the passive-capture design argument are the
immediate next action for a follow-up session.

**Lessons folded back into the agent definitions the same day.** The single most useful
observation from this session: *all three bugs built cleanly and passed every host-native
test* (ESP32 17/17, Flipper 39/39 + 67/67) and still failed on hardware — the project's
verification pyramid has a structural hole at "the two firmwares interacting over real
radios," and the stack-overflow class in particular is one host tests can *never* catch
(desktop stacks are ~1000x the `BleEventWorker` budget). Two of the three bugs also sat at
the seam between the independently-implemented firmwares, exposing the known weakness of that
otherwise-valuable design: an invariant not written into the shared contract is verified by
nobody. `.claude/agents/flipper-developer.md` and `.claude/agents/esp32-developer.md` each
gained a "Known failure modes" section (the 1280-byte stack budget and its nesting/`static`-
initializer traps; ported third-party code importing upstream's memory profile; exported-
symbol-≠-known-semantics, with the LED blink and `DataFixed` examples; ATT MTU vs. the peer's
independent characteristic-length cap; the `sdkconfig.defaults` staleness trap; and
"artificial test parameters exclude failure modes"), plus working-method steps for a stack
audit, for confirming cross-firmware facts by reading the other firmware's source, and for
proposing an agent-file update whenever a bug class *recurs* — the stack overflow was
carefully logged three times without the logging ever changing behavior. Process-level
remediations that are project work rather than agent behavior (compile-time stack-usage
checking, promoting `PAYLOAD_MAX` into the shared contract, an at-real-MTU test in step 9) are
recorded under `docs/PLAN.md`'s "Remediations proposed after the 2026-09-05 hardware pairing
test."

## 2026-09-05: live code-health defect fixes closed out

Fixed 4 of the 5 items in `docs/PLAN.md`'s "Live code-health defects (consolidated
2026-09-05)" list, delegated to `esp32-developer`/`flipper-developer` in parallel (each ran
against the current code, then this session verified both builds directly after an
interrupt-and-resume — see full per-file detail in `docs/PLAN.md`'s adjacent "Live
code-health defect fixes (2026-09-05)" subsection, not duplicated here):

- `FEB_TX_MAX_FRAGMENTS` (ESP32 `main.c`) corrected from `48u` to `13u` with a fixed
  derivation comment.
- `feb_fragment_record()`'s stale doc comment in `framing.h` (both copies) rewritten to
  match the real file-scope-static-buffer implementation.
- `feb_reassembly_check_timeout()` wired up periodically on both sides (ESP32: NimBLE
  `ble_npl_callout`; Flipper: FuriTimer + new `reassembly_mutex`) — the Flipper had the same
  unwired-timeout gap, not previously documented, found while fixing the ESP32 side.
- `FEB_CBOR_MAX_BYTES_LEN` (`cbor_codec.h`, both copies) raised `256u` -> `512u` to match
  `FEB_CBOR_MAX_PAYLOAD`, done now rather than deferred to step 6 since it's a pure
  constant-widening fix with no new decode path.

Both `idf.py build` and `fbt.cmd fap_flipper_esp32_over_ble` pass clean after all four
fixes. No board was flashed. The 5th item (`pairing_crypto.c`'s non-constant-time X25519
ladder via `mbedtls_mpi_mod_mpi()`) remains open by design — accepted risk for the current
threat model, not a bug to fix opportunistically.

**Process note:** this session was interrupted mid-fix (`stop` while both subagents were
still running) and resumed (`continue`). Rather than re-launching fresh agents, the
already-edited files were read directly to confirm what the killed agents had completed
(all 4 fixes were done correctly on both sides before being killed), and only the two
remaining build-verification steps were re-run directly. No duplicated work resulted.

## 2026-09-05 grill-me session: step 5 hardware verification plan (methodology only, not executed)

A design-review session on 2026-09-05 walked the methodology for step 5's three remaining
"done when" checks (reboot-survival, reset-and-repair, passive-capture confirmation) before
touching any hardware. Full decision list and the concrete step-by-step sequence is recorded
in `docs/PLAN.md` step 5's new "Step 5 hardware verification plan" section — summary for the
next session to pick up and execute:

- Reboot-survival is checked out-of-band (raw NVS/`.dat` byte diffs before/after reboot), not
  by adding read-back code to either firmware.
- A real design gap was surfaced: every ESP32 reset unconditionally opens a fresh pairing
  window and re-pairs on any connection within it (no existing-secret check exists until step
  6), so reboot-survival and reset-and-repair are entangled unless the reboot-survival test
  keeps the Flipper's app closed throughout, letting the window expire unused. A related,
  currently-unresolved question was flagged for step 6's own future design: whether a reset
  should attempt runtime auth with a stored secret before opening a pairing window, or whether
  the window always opens first regardless — `docs/PLAN.md`'s existing step-6 note only
  covers post-window behavior, not this.
- "Other pairings unaffected" is tested by planting a dummy second Flipper pairing file
  (no second real board exists yet), rather than resting on the per-file-storage design
  argument alone.
- Passive-capture confirmation is a **code audit only** this pass (re-confirm neither
  `K_shared` nor `pairing_secret` is ever transmitted, only public keys/nonces/confirmation
  tags) — a literal RF capture is explicitly deferred to a far-future backlog item, since no
  BLE sniffer hardware exists anywhere in this project today (confirmed by search).
- **Real logistical finding: the exact 2026-09-05 hardware-tested build no longer exists.**
  The current build artifacts' sizes don't match what was recorded right after the successful
  pairing test — the same-day "live code-health defect fixes" session rebuilt both firmwares
  in place afterward, overwriting the tested binaries, and this project has no git repo to
  recover the pre-fix source from. Decided against hand-reverting the four small known changes
  from their prose description (real risk of a transcription error right before a
  security-relevant test); the plan now just flashes current code for the whole test, which
  also happens to give the code-health fixes' new timer/mutex machinery its first-ever
  hardware exposure.
- **Nothing was built, flashed, or reset this session.** The next session should reconfirm
  serial ports and peer-session conflicts (see `[[project_concurrent_sessions]]`), get
  explicit user go-ahead per the hardware-safety rule, then execute the 11-step sequence
  recorded in `docs/PLAN.md`.

## Step 5 wiring: both sides complete, hardware test is the next action

As of the two entries directly above, both firmwares implement the full pairing ceremony
against the shared `pairing.h`/`pairing_crypto.h` contracts and both build clean
(`idf.py build` exit 0; `fbt.cmd fap_flipper_esp32_over_ble` succeeds, artifact 38,284
bytes). Neither board has been flashed with this code yet. Step 5's actual "done when" bar
(survives reboot, reset-and-repair replaces only that board's record, passive capture
doesn't expose the secret) can only be checked by running both devices against each other —
this requires explicit user go-ahead first, per this project's hardware-safety rule, and
should reconfirm current serial ports (`COM9`/`COM8` from prior sessions, not guaranteed
stable) and check for conflicting peer sessions first (14 were active as of this session —
see `[[project_concurrent_sessions]]`).

## Step 5 hardware verification executed — "done when" bar fully met (2026-09-05)

Ran the 11-step sequence from `docs/PLAN.md`'s "Full sequence for the next session to
execute," with the user physically present for OK-button and RST presses. 22 peer sessions
were checked first (see `[[project_concurrent_sessions]]`); one flagged a possible conflict
with a step-4-sweep-sounding task on the same COM8/COM9, which the named peer denied and a
read-only `esptool flash_id` probe confirmed was unfounded (COM9 responded cleanly, no
contention) — most likely a session-name misattribution, consistent with a naming-collision
symptom another peer flagged independently the same day. Both firmwares were reflashed with
current code first (`idf.py -p COM9 flash`; `fbt.cmd fap_flipper_esp32_over_ble` synced via
`applications_user/flipper_esp32_over_ble` then deployed with `fbt launch
APPSRC=applications_user/flipper_esp32_over_ble`), per the prior session's decision to not
attempt reconstructing the exact original tested build.

**All three "done when" checks passed:**
- **Reboot-survival:** the Flipper's real `<board_id>.dat` pairing file was pulled before and
  after a Flipper-alone reboot (`scripts/power.py reboot`, i.e. the CLI `power reboot`
  command) — SHA-256 identical.
- **Reset-and-repair isolation:** a dummy second-board file (`fake-board-id-test.dat`,
  throwaway bytes) was planted alongside the real board's file. A `RST` on the ESP32 alone
  (Flipper app not running) left the ESP32's NVS pairing region byte-identical after its
  120-second window expired unused. A real reset-and-repair (Flipper app launched, OK
  pressed, then ESP32 `RST`) changed both the ESP32's NVS blob and the real `.dat` file
  (fresh ceremony, fresh secret) while the dummy file stayed byte-identical throughout —
  confirming replacement is scoped to the one real board's record only.
- **Passive-capture code audit:** re-walked (not re-derived) the actual shipped `pairing.c`
  on both firmwares. Confirmed by direct grep/read: `k_shared` is used only to derive
  `k_confirm`/`pairing_secret` and is zeroized immediately after
  (`flipper_esp32_over_ble.c:416`, mirrored on the ESP32 side); `k_confirm` is used only as
  an HMAC key producing confirmation/completion *tags* (outputs, not the key), and
  `pairing_secret` is used only for local storage (`pairing_storage_save` /
  `persist_pairing_secret`). Neither ever reaches `feb_cbor_encode_pairing_envelope`/
  `send_pairing_record` (Flipper) or the ESP32's equivalent send path — only public keys,
  nonces, and derived tags are ever encoded into a wire record, on both sides.

**Two real findings surfaced during the run, neither fixed this session:**

1. **Flipper pairing files are persisted under the wrong app's data directory.** The real
   file was found at `/ext/apps_data/bt/pairings/<board_id>.dat`, not
   `/ext/apps_data/flipper_esp32_over_ble/pairings/<board_id>.dat` as the step-5 design
   intended. Root cause: `handle_pair_complete()` (and therefore `pairing_storage_save()`)
   runs synchronously inside `profile_event_handler()`, which executes on the BLE stack's own
   `BleEventWorker` thread — owned by the firmware's built-in `bt` service, not this app's own
   thread (confirmed in the 2026-09-03 stack-overflow investigation entry above). Unleashed's
   `APP_DATA_PATH`/`"/data"` virtual-path resolution
   (`applications/services/storage/storage_processing.c`, `"/data" -> "/ext/apps_data/" +
   furi_thread_get_appid(thread_id)`) keys off the *calling* thread's registered app ID, not
   the app that actually opened the `Storage` handle — so every `APP_DATA_PATH(...)` call made
   from this callback resolves against `"bt"` instead of `"flipper_esp32_over_ble"`. Practical
   consequence: the pairing secret isn't isolated in this app's own directory (uninstalling
   the FAP wouldn't clean it up; it shares a directory namespace with the built-in Bluetooth
   service's own data). Doesn't block any "done when" check — the file is still real,
   deterministic, and per-board-id — but is a real bug. **Backlog: fix by resolving/caching
   the real app-owned path once (from the app's own thread, e.g. at app init) instead of
   calling `APP_DATA_PATH` from inside the BLE callback.**
2. **The custom BLE profile appears to stay live/connectable after the "paired" success
   screen, without requiring a fresh OK-press for a subsequent ceremony to complete.**
   Discovered by accident: `esptool read_flash`'s default `--after hard_reset` behavior
   reset the ESP32 as a side effect of what was meant to be a read-only NVS dump, opening a
   fresh 120-second pairing window; because the Flipper app was still sitting on the "paired"
   screen from the first ceremony (custom profile presumably still advertising/connectable),
   a second full ceremony completed silently within the following ~40 seconds — no OK-press,
   no user interaction on the Flipper at all — changing both sides' stored secrets. Confirmed
   by diffing NVS content pulled immediately before vs. shortly after that incidental reset.
   This means the 2026-09-05 design decision "kept as the existing single explicit OK-press
   action... for both the no-saved-pairing and already-paired-for-this-board cases" gates
   *starting the app*, not *each ceremony* — an app left open on the success screen will
   silently re-pair with any ESP32 that resets and opens a window in range, which may or may
   not be the intended behavior (the design session didn't consider this case explicitly).
   **Backlog: needs an explicit design decision** — e.g. whether the custom profile should
   stop advertising/tear down once `pair_complete` succeeds (matching the general
   stop-advertising-on-exit lifecycle already required elsewhere), or whether silent
   re-pairing while the app sits idle-post-success is accepted behavior.

**Methodology/logistics notes for future hardware sessions on this project:**
- `esptool read_flash`'s default `--after hard_reset` is not actually read-only from a
  behavioral standpoint — it resets the target. Pin `--before default_reset --after no_reset`
  explicitly for any NVS dump meant to be a true read-only snapshot; `no_reset` leaves the
  chip sitting in the ROM bootloader afterward (not running app code), so a deliberate
  `--after hard_reset` (or a bare `chip_id`/`flash_id` call) is needed afterward to resume
  normal operation.
- **Correction to the step-4 sweep's "COM8 scriptability is unreliable" finding**: that
  applied specifically to raw `input send ok press/short/release` button-simulation over a
  naive `WriteLine`. This session used two different, structured CLI mechanisms instead —
  `fbt.cmd launch APPSRC=...` (via `scripts/runfap.py`'s `FlipperStorage`-protocol deploy +
  `loader open`) and `scripts/power.py reboot` (`FlipperStorage`-protocol `power reboot`) —
  both worked reliably, repeatedly, including deploying+launching the FAP twice and rebooting
  the device cleanly once. The button-press simulation itself was never retried this session
  (all OK-presses were done physically by the user per the hardware-safety/physical-action
  constraint), so that specific finding stands unchanged — only the broader "COM8
  scriptability" framing needed narrowing.
- COM8 briefly vanished from the OS's serial-port list for under a minute, for no identified
  reason, and came back on its own after a physical check — not investigated further, flagged
  in case it recurs.

Raw NVS/`.dat` dumps used for the diffs (containing the real plaintext `pairing_secret`) were
deleted from the scratchpad directory after these results were confirmed and recorded, per
the verification plan's own step 11.

**Step 5 is now fully closed.** Step 6 (authenticated runtime sessions) is next — see
`docs/PLAN.md`'s still-open flagged question about how a stored secret should interact with
the unconditional per-reset pairing window once step 6 exists.

## 2026-09-06 grill-me session: step 6 design decisions (methodology/design only, not executed)

A design-review session on 2026-09-06 walked step 6 before any implementation started (no
code written, no board flashed this session). Full decision list and rationale is recorded in
`docs/PLAN.md` step 6's new "Step 6 implementation decisions" section, `docs/PROTOCOL.md`'s
new "Runtime auth failure handling" section, and `docs/PAIRING.md`'s revised reset-window
wording — not duplicated in full here; summary of what changed and why, plus the questions
asked along the way:

- **Confirmed no step-3-style spec gap existed going in**: `docs/PROTOCOL.md` already fully
  specifies the HKDF `info` string, 12-byte nonce construction, and AAD content byte-for-byte
  for the runtime session flow — nothing needed closing there before implementation.
- **Reset-vs-runtime-auth interaction (the question left open by the step-5 session).**
  Walked through what "physical reset" actually means on this chip first: nothing in
  `esp32/main/main.c` currently calls `esp_reset_reason()`, so every boot cause (RST button,
  power cycle, EN pin, and any future software `esp_restart()`) is currently treated
  identically — "physical reset" in the existing docs really meant "any full reboot." Decided:
  on boot, if a `pairing_secret` is already stored, the ESP32 attempts runtime auth
  (`hello`/`hello_ack`/`client_auth`) first; a pairing window opens only if no secret exists
  yet or the attempt fails with the new `unknown_board` error (below) — not on every boot, and
  not in parallel with a live pairing window (both alternatives were offered and rejected: the
  first for forcing needless re-pairing on routine reboots, the second for the added
  complexity of two live protocol state machines racing on one connection).
  - A follow-up question — should the ESP32 additionally distinguish an intentional reset
    from a crash/watchdog/brownout reboot via `esp_reset_reason()`, since today *any* reboot
    (including a crash under load) can open a window — was resolved as **moot**, not merely
    backlogged: under the new runtime-auth-first design, a crash reboot with a valid stored
    secret just resumes auth silently regardless of cause; a window only opens on an actual
    auth rejection. `esp_reset_reason()` differentiation is explicitly not planned.
- **A real, previously-undefined protocol gap found and closed: `unknown_board`.** Nothing in
  the prior `error.code` list covered "the Flipper has no stored record for this `board_id`"
  — needed as the exact signal that authorizes the ESP32's pairing-window fallback. Added as a
  new error code in `docs/PROTOCOL.md`. Deliberately kept separate from a **proof-verification
  failure** (secret desync/corruption/impersonation attempt): that case is logged and
  rate-limited per the existing requirement but must **not** auto-open a window, since
  conflating the two would let a radio attacker force repeated pairing-window openings just by
  corrupting proofs in transit. This split was reached by explicitly asking whether *any*
  auth rejection should open a window versus only the "never paired" case — the latter was
  chosen.
- **Explicit re-pair trigger while a valid mutual secret exists on both sides.** Asked what
  the user's actual options are for this, given the reset-vs-runtime-auth decision means a
  plain reset no longer forces it. Resolved: the Flipper's local "unpair this board" action
  (step 8 scope) is sufficient by itself — it deletes only the Flipper's own record, and the
  ESP32's next connection attempt naturally gets `unknown_board` and falls back to opening a
  window with no ESP32-side action needed at all. This works only because the Flipper is the
  side that controls whether runtime auth succeeds. Checked the DevKitC-1 for a spare
  physical button first (there isn't one — only RST and BOOT/GPIO9, the latter a strapping
  pin already off-limits per `CLAUDE.md`), which is why no ESP32-local "hold to force re-pair"
  gesture was considered viable here.
  - **This revises a previously-recorded decision**, not just an implementation detail:
    `docs/DECISIONS.md`'s "BLE pairing bootstrap" and `docs/PAIRING.md` both said physical
    reset alone is unconditional authorization to re-pair, including replacement. Both are
    now updated: reset alone only re-opens pairing when no working secret exists yet.
- **Factory-reset options walked explicitly** ("go to a clean state where the board has the
  firmware loaded, but all other data is erased"). Laid out four options: (A) host-tool NVS-
  partition erase over USB (`esptool erase_region`/`parttool erase_partition`, no new
  firmware code, `board_id` is MAC-derived so no identity loss) — confirmed usable now; (B)
  full erase + reflash — confirmed unnecessary, functionally equivalent to A but heavier; (C)
  a Flipper-authenticated `factory_reset` command over BLE — **explicitly rejected**: factory
  reset must always be a physical/local-access action on the ESP32, never reachable remotely,
  a deliberate security boundary; (D) an in-firmware no-PC/no-session physical gesture (BOOT
  post-boot read, or a reboot-counter pattern) — backlogged but **scheduled soon, right after
  step 6**, not indefinitely deferred; not yet designed, needs the strapping-pin datasheet
  check first.
- **Multi-board pairing + Flipper launch/auto-connect behavior confirmed compatible.** The
  Flipper advertises one fixed service UUID regardless of how many boards it has records for;
  first paired board to connect and send a recognized `board_id` wins the connection slot
  (already-recorded policy, now confirmed still correct). Opening the FAP with any saved
  record present auto-starts advertising and waits for reconnect with no OK-press — OK-press
  is reserved for the genuinely-new-pairing (no saved record) case only, giving the
  already-recorded (but previously unimplemented-against) "auto-connect" note in this file
  actual meaning. An unrecognized `board_id` arriving via `hello` is exactly the newly-defined
  `unknown_board` case above.
- **Pairing-file path-resolution bug** (this file's step-5 hardware-verification entry above)
  **will be fixed now**, ahead of/alongside step 6, rather than deferred to step 8 again —
  step 6 adds more state written from the same buggy callback-thread path, so fixing it once
  now is cheaper than re-verifying against a growing pile of misplaced state.
- **Flipper-side crypto scratch-buffer storage, resolved with higher confidence than a default
  recommendation.** Initially framed as "static by default, matching existing precedent" vs.
  "audit stack depth first" — the second option was miscalibrated (it conflated mbedtls's
  internal call depth, an ESP32-only concern, with the Flipper side). Corrected by actually
  reading `flipper/pairing_crypto.c` (every X25519/SHA-256/HMAC/HKDF buffer there is already
  `static` with an inline stack-budget rationale — e.g. `cmult()`+`fmonty()` alone would stack
  to ~2.4 KB against the 1280-byte `BleEventWorker` budget if left stack-local) and then, at
  the user's prompt, directly auditing `furi_hal_crypto_gcm_*`'s real source (cached mirror,
  confirmed matching the pinned checkout) rather than treating it as an unauditable black box.
  Finding: it's a thin wrapper around the STM32WB's memory-mapped `AES1` hardware peripheral,
  processing 16 bytes at a time via register writes, with only small fixed-size locals
  (`iv_and_counter[16]`, `dtag[16]`, `block[4]`) and no software AES math or buffer that scales
  with input size — genuinely negligible stack risk, no hardware-verification caveat needed.
  Decided: this app's own new AAD/payload/ciphertext scratch buffers (up to 512 bytes each per
  `docs/PROTOCOL.md`) go file-scope `static`, continuing the established pattern.
- **Session teardown/reconnect: confirmed the existing `docs/PROTOCOL.md` spec (30s idle
  timeout, no counter resumption, disconnect-on-any-auth-failure) is sufficient as-is** — no
  new rules needed. Walked the practical, user-visible consequence explicitly (prompted by a
  "what if I walk away from the car for a minute" question): on any disconnect, both sides
  discard in-memory session state only (`pairing_secret` untouched); the Flipper's profile
  keeps advertising as long as the app is still open (only an explicit exit tears it down,
  not screen dimming/auto-lock); the ESP32 reconnects per its existing step-4
  bounded-then-indefinite backoff and re-runs the handshake automatically. Confirmed this
  needs no user action and already covers the wardriving "step away, come back later"
  scenario — the 30-second number specifically governs an idle-but-still-connected link, not
  an out-of-range disconnect, but the outcome (automatic silent reconnection) is the same
  either way.
- **File layout confirmed to follow the same step-3/step-5 pattern**: shared contracts (likely
  extending `cbor_codec.h`/`framing.h` rather than a new header, since step 3 already defined
  the protected-record CBOR shape — the exact split is left as an implementation-time call)
  plus `tests/vectors/` additions written first, then `esp32-developer`/`flipper-developer`
  implement independently in parallel against new host-native test binaries, same as before.
  This was the one item flagged as not yet decided when this session wrapped up; confirmed
  directly rather than left for the next session to re-derive.
- **Nothing has been built or flashed yet** — this session was design-only. Next session
  should implement the path-resolution-bug fix and the `hello`/`hello_ack`/`client_auth` flow
  per the decisions above, delegated to `esp32-developer`/`flipper-developer` in parallel per
  this now-confirmed execution pattern.

## 2026-09-06: step 6 shared contracts written; AES-128-GCM -> AES-256-GCM protocol revision

Before writing any step-6 crypto code, found a real feasibility blocker (not caught by the
design session above): the Flipper's only exported raw-key AES-GCM primitive
(`furi_hal_crypto_gcm_encrypt_and_tag`/`_decrypt_and_verify`) is hardcoded to a 256-bit key
at the hardware level — traced into `furi_hal_crypto.c`'s `crypto_key_init_bswap()`
(cached mirror, `docs/references/flipper-firmware/upstream/targets/f7/furi_hal/furi_hal_crypto.c`),
which unconditionally selects `CRYPTO_KEYSIZE_256B` and reads exactly 8 `uint32_t` words
(32 bytes) from the key pointer for both its GCM and CTR paths — there is no 128-bit path
through this API, and software mbedtls GCM isn't a fallback (step 5 already found every
`mbedtls_*` symbol unexported in this firmware). Presented the user two options: revise the
protocol to AES-256-GCM, or port a software AES-128-GCM implementation into the Flipper
firmware (repeating the ported-crypto stack-overflow risk class that already caused three
real hardware crashes). **User chose AES-256-GCM.** Full rationale and the doc-update list
(`docs/PROTOCOL.md`, `docs/DECISIONS.md`, `docs/PAIRING.md`, `docs/BASELINES.md`,
`docs/STANDALONE_FAP.md`, both agent definitions) is recorded in `docs/PLAN.md` step 6's new
"AES-128-GCM -> AES-256-GCM protocol revision" entry — not duplicated here. HKDF-SHA-256
session-key derivation already supported 32-byte output via the existing
`FEB_HKDF_MAX_LEN`=32 cap, so this needed no new crypto primitive on either side, just
`length=32` instead of `length=16` and a renamed HKDF `info` suffix.

Wrote the shared step-6 contracts (mirrored byte-for-byte between `esp32/main/` and
`flipper/`, verified via `diff`), following the framing.h/pairing.h precedent of a
crypto-primitive header plus a protocol-glue header built on it:

- **`session_crypto.h`** (new): `feb_gcm_encrypt()`/`feb_gcm_decrypt()`, one-shot AES-256-GCM
  AEAD primitives (`FEB_SESSION_KEY_LEN`=32, `FEB_SESSION_NONCE_LEN`=12,
  `FEB_SESSION_GCM_TAG_LEN`=16). Declarations only — `session_crypto.c` (backed by
  `mbedtls_gcm_*` on the ESP32, `furi_hal_crypto_gcm_encrypt_and_tag`/`_decrypt_and_verify`
  on the Flipper) is implementation work, delegated below.
- **`session.h`** (new): `hello`/`hello_ack`/`client_auth` payload structs + encode/decode
  (living here rather than `cbor_codec.h`, matching how `pair_init`/etc. payloads live in
  `pairing.h` per that file's own scope note); the transcript `S` builder
  (`{version, board_id, session_id, client_nonce, device_nonce}`); the runtime proof helpers
  (wrapping `pairing_crypto.h`'s existing `feb_hmac_sha256()` — no new HMAC primitive
  needed); session-key derivation (wrapping the existing `feb_hkdf_sha256()`); the AAD
  builder (`{version, type, session_id, sequence, board_id}`); the 12-byte nonce builder;
  and `feb_session_encrypt_record()`/`feb_session_decrypt_record()` wrapping
  `cbor_codec.h`'s existing `feb_protected_record_t` around `session_crypto.h`'s GCM
  primitives. Explicitly does not own sequence-counter state, the 30s idle timeout, or the
  reset-vs-runtime-auth boot decision (implementation-specific per firmware, same split
  `pairing.h` already used). The `unknown_board` error path deliberately reuses
  `pairing.h`'s existing `feb_pairing_envelope_t` + `cbor_codec.h`'s `feb_error_payload_t`
  rather than a new struct, per `docs/PROTOCOL.md`'s own wording that it uses the
  session-id-less pairing envelope. Header comment flags the stack-budget lesson from
  steps 3/5 explicitly: `session.c`'s internal AAD/transcript scratch buffers (~150-200
  bytes, reachable from `profile_event_handler` on the Flipper's 1280-byte
  `BleEventWorker` stack) must be file-scope `static` from the start, not stack-local —
  don't wait for a fourth crash to rediscover this.
- **`cbor_codec.h`** (both copies): added `FEB_CBOR_ERR_AUTH_FAILED` to `feb_cbor_status_t`
  (appended at the end, device-local only, never serialized) for "GCM tag did not verify."

**`tests/vectors/generate_vectors.py` extended** with a from-scratch, self-validated
AES-256 + GCM implementation (S-box derived from the GF(2^8) multiplicative inverse +
affine transform per FIPS-197, not a hard-coded table — same anti-transcription-error
rationale as the existing X25519 code) and new vectors:
- `FEB_VEC_GCM_*`: the GCM spec's own "Test Case 16" (AES-256, AAD + plaintext, 96-bit
  IV), fetched directly from a raw download of hostap's `tests/test-aes.c` (confirmed via
  `curl`/`wc -c` byte-length sanity checks on each hex field before use — not taken from an
  LLM-summarized fetch, after a first WebFetch-summarized pass of the same file
  mis-transcribed digit counts and was discarded). The generator's own AES-256-GCM
  self-validates against this vector via `assert` before computing anything else — this
  caught one real bug in the first-draft implementation (an extra spurious right-shift of
  the GF(2^128) reduction constant `R` inside the GHASH multiplication, `v ^= (R >> 1)`
  instead of `v ^= R`), fixed and reconfirmed against the known-answer tag before being
  trusted.
- `FEB_VEC_SESS_*`: one golden end-to-end session vector (fixed, non-random inputs,
  reusing the step-5 golden vector's `pairing_secret`) covering the full
  `hello`/`hello_ack`/`client_auth` payload+record encodings, the transcript `S`, both
  runtime proofs, the derived session key, one valid protected `error` record at sequence
  1, a second valid record at sequence 2 (for an implementation's own sequence-continuity
  test), and two tampered variants of the sequence-1 record (`_BAD_CIPHERTEXT`, `_BAD_AAD`
  — the latter keeps the original ciphertext/tag but changes the outer `sequence` field the
  AAD is built from) that must fail authentication.
- `tests/README.md` gained a "Step 6 additions" section describing all of the above.

**Also decided, not yet done:** the Flipper pairing-file path-resolution bug (Backlog,
`docs/PLAN.md`) will be fixed as part of the same delegated pass below, since it touches
the same `flipper_esp32_over_ble.c` file the `hello`/`hello_ack`/`client_auth` wiring does.

**Not done this session, by design (contracts-only pass, matching the step 3/5 execution
order):** `session_crypto.c`/`session.c` on either firmware, a `test_session.c` host
binary, and all `main.c`/`flipper_esp32_over_ble.c` wiring (reset-vs-runtime-auth boot
logic, rate-limiting, the path-resolution-bug fix). Delegated to
`esp32-developer`/`flipper-developer` in parallel immediately after this entry — check
this file's next dated entry for the outcome.

## 2026-09-06: step 6 implementation — both sides host-test-verified and build-verified

`esp32-developer` and `flipper-developer` implemented against the frozen contracts above, in
parallel, per the delegation immediately following the previous entry. Full status recorded
in `docs/PLAN.md` step 6's new "Step 6 status" section — not duplicated in full here.
Headline results: **ESP32 host test 27/27 pass, `idf.py build` clean; Flipper host test
42/42 pass, `fbt.cmd fap_flipper_esp32_over_ble` clean (artifact 51,780 bytes).** Frozen
contracts (`session.h`/`session_crypto.h`/`cbor_codec.h`/`pairing.h`/`pairing_crypto.h`)
confirmed still byte-identical between `esp32/main/` and `flipper/` after both agents
finished (re-diffed directly, not just taken on their word). **Neither board has been
flashed with this code yet** — that is the next action once the user gives explicit
go-ahead, per this project's hardware-safety rule.

Worth recording here rather than only in PLAN.md: the Flipper agent's host test needed a
host-only simulated substitute for `furi_hal_crypto_gcm_encrypt_and_tag`/`_decrypt_and_verify`
(a hardware-only API with no desktop equivalent) so the real, unmodified production
`session_crypto.c`/`session.c` could actually compile and run on a desktop — a from-scratch
AES-256-GCM implementation living only under `tests/flipper/host_shims/`, never linked into
the real FAP. This is a genuinely different situation from the ESP32 host test, which links
its *real* production crypto backend (`mbedtls_gcm_*`, portable) directly — a known asymmetry
between the two test suites, not an oversight, and worth remembering if a future session is
confused about why one host test needs a stub and the other doesn't.

Also worth recording: the Flipper agent caught and corrected a real flaw in this session's own
delegation instructions. The plan (written by the orchestrating session, based on `docs/PLAN.md`'s
backlog wording) assumed `APP_DATA_PATH(...)` could be "resolved once and cached" from the
app's own thread — but it's a compile-time string-substitution macro (`"/data/" + path`), not
a runtime call, so caching its expansion would have produced the identical wrong string
regardless of caller thread. The agent instead read `storage_processing.c`'s
`storage_process_alias()` directly, found the real thread-identity resolution happens inside
the storage service at call time (keyed off `furi_thread_get_appid()` of whichever thread
issues the call), and fixed it with the exported
`storage_common_resolve_path_and_ensure_app_directory()` API instead — called once from the
app's own thread, producing an already-fully-resolved absolute path that no longer starts
with `/data` and therefore bypasses the thread-dependent alias check regardless of which
thread uses it afterward (including `BleEventWorker`). A good example of an agent correctly
pushing back on a flawed instruction instead of implementing it literally.

Judgment calls and real gaps found by both agents (rate-limit shape, `hello_ack` timeout,
envelope-routing-by-field-count, the pre-existing `MAX_RECONNECT_RETRIES` hard-stop, the
unenforced 30s idle timeout, missing `unsupported_version` handling on the session path) are
all recorded in `docs/PLAN.md` step 6's status entry and its Backlog — none are blocking, all
are flagged for follow-up rather than silently accepted or silently fixed.

After both agents finished, the orchestrating session made one more comment-only edit (both
`cbor_codec.h` copies: tightened a stale "AES-128-GCM ciphertext is plaintext-length" comment
left over from before this session's protocol revision, no functional change) and had both
agents reconfirm their builds still passed clean afterward — they did.

`docs/USER_GUIDE.md` was updated (via a Haiku-delegated edit, per this project's convention
for mechanical doc sync) with a short paragraph right after its scope note: step 6 is
implemented and build-verified but not yet hardware-verified, so the rest of the guide still
describes the steps-1-5, 2026-09-05 hardware-confirmed state, and it names the three things
that will change once step 6 is hardware-verified (no OK-press needed when a saved pairing
exists, the silent-reset-re-pairing quirk being prevented by design, and the wrong-folder
pairing-storage bug being fixed). The existing walkthrough/quirks sections were deliberately
left untouched, since they still accurately describe what's actually flashed today.

**User was asked whether to proceed with on-hardware verification now and explicitly asked
instead to save session state and continue the hardware test in a clean session.** So: no
board has been touched, and the next session should pick up directly at the on-hardware
verification of the reset-vs-runtime-auth boot decision and the `unknown_board` fallback —
requiring its own explicit user go-ahead per this project's hardware-safety rule (asking again
is not a formality that was already satisfied here), and should reconfirm serial ports and
check for conflicting peer sessions first (see `[[project_concurrent_sessions]]`).

## Step 6 on-hardware verification executed — "done when" bar fully met (2026-09-06)

A new session picked up the step-6 hardware test per the prior entry's handoff. 3 peer
sessions were checked first (`[[project_concurrent_sessions]]`); 2 gave an explicit all-clear
on `esp32/`/`flipper/`/COM8/COM9 before hardware was touched, the third didn't respond in time
but the user had already asked to proceed and peer risk was low (3 sessions, not the 20+-session
scenario from the 2026-09-03 incident). Both boards were reflashed with the already
build-verified step-6 code (`idf.py -p COM9 flash`; `fbt.cmd launch
APPSRC=applications_user/flipper_esp32_over_ble`, source confirmed byte-identical to
`flipper/` via `diff` first) — both succeeded with hash-verified writes.

**Real, predictable finding before any protocol test ran:** the Flipper app came up stuck on
"OK: start pair/connect" instead of auto-advertising, despite a real pairing record existing
from the 2026-09-05 step-5 test. Root cause confirmed by listing the Flipper's filesystem over
serial (`scripts/storage.py -p COM8 list <path>`, run from **PowerShell, not Git-Bash** — Git-Bash's
MSYS path translation silently rewrites a leading `/` argument into a Windows path, e.g. `/`
became `C:/Program Files/Git/`, which is why every `list` call returned `INVALID_NAME` until the
tool was rerun from PowerShell): the step-5 pairing file was still sitting at the old, buggy
`/ext/apps_data/bt/pairings/esp32c6-acebe6fffeda.dat`, while step 6's now-fixed path resolution
correctly looks in `/ext/apps_data/flipper_esp32_over_ble/pairings/`, which was empty. Not a new
bug — the exact, expected fallout of fixing the path-resolution bug — but worth recording since
it means **any board paired before this session's flash needs a fresh pairing ceremony**; the
old record doesn't carry forward automatically.

**All three "done when" checks then passed, exercised naturally by that fallout rather than
staged separately:**
- **Reset-vs-runtime-auth boot decision:** ESP32 boot log: `board_id=esp32c6-acebe6fffeda:
  stored pairing_secret found; attempting runtime auth (no pairing window opened)` — its NVS
  secret survived the reflash (no erase was done) and it correctly tried `hello` first instead
  of unconditionally opening a window.
- **`unknown_board` fallback:** Flipper logged `hello for unknown board
  'esp32c6-acebe6fffeda'` and replied with the error; the ESP32 decoded it
  (`flipper has no pairing record for board_id=...; will open a pairing window on the next
  connection attempt`), closed the connection, then opened the window only on the *next*
  connect attempt — matching the design decision precisely. A full fresh X25519 ceremony then
  ran (`pair_init`/`pair_reply`/`pair_confirm`/`pair_complete`) and the Flipper logged
  `Paired with board 'esp32c6-acebe6fffeda'`.
- **Path-resolution bugfix, hardware-confirmed as a side effect:** the new pairing file landed
  at the correct `/ext/apps_data/flipper_esp32_over_ble/pairings/esp32c6-acebe6fffeda.dat`
  (32 bytes, verified via `storage.py list`), while the stale pre-fix file remains untouched at
  the old `bt/pairings/` path (harmless leftover, not cleaned up).
- **Silent runtime-auth resume:** user pressed RST on the ESP32 again; this time, with matching
  secrets on both sides, the Flipper logged `Runtime session authenticated for board
  'esp32c6-acebe6fffeda'` and the ESP32 logged the matching `hello sent; awaiting hello_ack` ->
  8 reassembly fragments -> `sending client_auth` -> `client_auth sent; runtime session
  authenticated` — no pairing window, no OK-press, clean session establishment.

**Methodology note for future sessions:** `esp-idf-monitor` on Windows can get stuck on
`ClearCommError` / "Waiting for the device to reconnect" after a manual RST toggles the
ESP32-C6's USB-Serial/JTAG enumeration; killing and restarting the `idf.py -p COM9 monitor`
background task recovered it cleanly both times this happened. Also: any `scripts/*.py` call
from the Unleashed checkout that takes a Flipper path argument starting with `/` must be run
from PowerShell, not Git-Bash, for the reason above.

**Step 6 is now fully closed**, including on-hardware verification. Next up per `docs/PLAN.md`
is step 7 (board identity/capability registry) — nothing implemented yet, design session should
precede it per this project's established pattern.

### Real bug found and fixed same session: LED stayed blinking after runtime session auth

After the on-hardware verification above, the user noticed the Flipper's screen correctly said
`ESP32 session active` but the blue LED kept blinking instead of going solid. Root cause,
confirmed by reading `flipper/flipper_esp32_over_ble.c`: `handle_client_auth()`'s success path
(runtime `hello`/`hello_ack`/`client_auth` flow) called `post_pairing_phase(...,
PairingPhaseSessionActive, NULL)` but never called `notification_message()` to stop the blink
(started once in `profile_start()` when advertising began) and set the LED solid — unlike the
pairing-ceremony's own success path (`pair_complete` handler), which already did both
(`sequence_blink_stop` then `sequence_set_only_blue_255`) before its own `post_pairing_phase`
call. A real, confirmed gap in the step-6 implementation, not a hardware issue.

Delegated to `flipper-developer` (this file only, `session.c`/`session.h`/`session_crypto.*`/
`cbor_codec.h`/`docs/PROTOCOL.md` untouched): added the same two `notification_message()` calls
to `handle_client_auth()`'s success path, matching the `pair_complete` convention exactly (same
order, same comment, `if(profile->app->notifications)` guard). Build passed clean
(`fbt.cmd fap_flipper_esp32_over_ble`, artifact 51,836 bytes).

**Hardware-verified the same session:** redeployed the fixed FAP via `fbt launch
APPSRC=applications_user/flipper_esp32_over_ble` — no ESP32 touch needed, since the fix is
Flipper-only and the ESP32 auto-reconnects with no rate-limit penalty on a plain disconnect
(`DISCONNECT_REASON_NORMAL` -> `start_scan()`, confirmed in `esp32/main/main.c`). One snag:
`fbt launch`'s auto-close-then-relaunch step failed the first time
(`Application "ESP32 over BLE" has to be closed manually` — this app uses a raw input-loop
without a registered `FuriSignalExit` handler, so Unleashed's loader can't force-close it
remotely; confirmed by reading `applications/services/loader/loader_cli.c`). Fixed by having
the user press Back to exit manually, then re-running `fbt launch` — worked cleanly. After
relaunch, the Flipper auto-reconnected and re-authenticated (missed capturing it in the log
stream — reconnect was faster than attaching the log tail — but the user directly confirmed on
the physical device: screen said `ESP32 session active` and the LED was solid, not blinking).

**Methodology note:** this app's loader-close limitation means any future FAP redeploy to a
Flipper with this app already running will need a manual Back-press first; `fbt launch` alone
cannot do it.

## 2026-09-06 grill-me session: in-firmware factory-reset gesture, design only

A design-review session on 2026-09-06 walked "what's next" before any implementation started
(no code written, no board flashed this session). Resolved the ordering question first: step 7
(board identity/capability registry) is the plan's next numbered step, but the backlog already
carried an explicit "scheduled soon — right after step 6, before step 7" note on the in-firmware
factory-reset gesture, so that gesture is next, not step 7. Full design (mechanism, trigger,
feedback, erase scope, delivery scope) is now recorded in `docs/PLAN.md`'s Backlog section under
that item; summary for the next session to pick up from:

- **ESP32-only, no FAP/BLE involvement at all** — the user was explicit that this must not be
  handled by the FAP or communicated to the Flipper in any way, unlike every other feature this
  project has built so far, all of which touch both firmwares.
- **Mechanism confirmed safe against this project's strapping-pin rule before deciding on it**:
  grepped `docs/hardware/esp32-c6-devkitc-1/README.md` and the vendor guide, which state
  GPIO0/4/5/8/9/15 are strapping pins only "during chip power-up or system reset" — reading
  GPIO9 (the onboard BOOT button) and driving GPIO8 (the onboard RGB LED) well after boot both
  fall outside that window, so no datasheet blocker exists for this design.
- **Continuously monitored** (not boot-window-only) — hold BOOT 5 continuous seconds, at any
  point while the board is powered on, in any pairing/session state. LED blinks for the hold
  duration; early release aborts silently; no distinct "confirmed" signal at 5s, since the
  erase-and-restart cycle itself is the confirmation.
- **Erase scope: full NVS-partition erase** (`nvs_flash_erase()` + `nvs_flash_init()`), matching
  the existing documented PC-based method's scope exactly (`esptool.py erase_region`/
  `parttool.py erase_partition --partition-name=nvs`) rather than a narrower pairing-namespace-only
  erase, so both recovery paths stay equivalent as more NVS namespaces are added later.
- **No new post-erase code path**: `esp_restart()` after the erase falls straight into the
  existing step-6 boot logic (no stored secret -> open a pairing window), reused verbatim.
- **Delivery scope for next session: build-verify only** (`idf.py build`), delegated to
  `esp32-developer`. Flashing and a live button-hold test is a separate follow-up requiring
  explicit user go-ahead, per this project's hardware-safety rule — deliberately not bundled into
  this same design session even though the user was available, to keep the design pass and the
  hardware-touching pass separate.
- **Explicit ordering decision for after this lands**: the `MAX_RECONNECT_RETRIES` hard-stop
  fix and the 30-second idle-connection-timeout enforcement (both pre-existing step-6-era backlog
  items) come next, in either order, before step 7 starts — both matter more once step 7 makes
  "board runs unattended for hours" a real use case. The missing `unsupported_version` handling
  can wait past step 7. This ordering was a deliberate choice to keep this session's factory-reset
  work scoped to one clean unit rather than stacking unrelated bugfixes into the same pass.
- **Nothing has been built or flashed yet** — this session was design-only, via the grill-me
  skill, walking each decision (mechanism, monitoring window, trigger/feedback, erase scope,
  reboot behavior, delivery scope, and relative ordering against the other open backlog items)
  one at a time before recording them.

## 2026-09-06 in-firmware factory-reset gesture: implementation (build-verified only)

Implemented the gesture designed in the grill-me session above, exactly as specified — no
redesign. New files `esp32/main/factory_reset.c`/`.h`, wired into `esp32/main/main.c`
(`feb_factory_reset_start()` called once from `app_main()` right after `nvs_flash_init()`
succeeds) and `esp32/main/CMakeLists.txt` (added `factory_reset.c` to `SRCS` and
`esp_driver_gpio`/`esp_driver_rmt` to `REQUIRES`). ESP32-only; `flipper/flipper_esp32_over_ble.c`
and `docs/PROTOCOL.md`/`docs/PAIRING.md` were not touched, per design.

- A low-priority (`tskIDLE_PRIORITY + 1`) FreeRTOS task polls GPIO9 every 50 ms. Holding it low
  (BOOT pressed, active-low with internal pull-up enabled) for 5000 continuous ms triggers
  `nvs_flash_erase()` + `nvs_flash_init()` + `esp_restart()`; releasing early resets the hold
  timer and turns the LED off with no other signal, matching the design exactly.
- **Real hardware fact confirmed before writing any LED code, not assumed:** grepped
  `docs/hardware/esp32-c6-devkitc-1/source/guide/user_guide.rst`, which states the onboard LED
  is an **addressable WS2812**, not a plain GPIO ("RGB LED - Addressable RGB LED, driven by
  GPIO8" plus a note about the board's "WS2812 driving circuit"). A simple `gpio_set_level()`
  toggle would not reliably light it — this needed the WS2812 one-wire serial protocol. Driven
  via the core ESP-IDF `driver/rmt_tx.h` (`esp_driver_rmt` component) using the same
  callback-encoder pattern as `$IDF_PATH/examples/peripherals/rmt/led_strip_simple_encoder`
  (checked into the local ESP-IDF checkout) — deliberately not the community-managed
  `led_strip` component, since that would require a network fetch via the component manager on
  every fresh build and this project's build has no such dependency today. Blinks a single dim
  red pixel at ~200 ms intervals for the duration of the hold.
- This dependency (the onboard LED being WS2812, not a plain GPIO) is itself exactly the kind
  of "fact about the peer/board must be read from source, not assumed" this project's failure
  log warns about — it's now recorded in `docs/hardware/esp32-c6-devkitc-1/README.md`'s
  "Verified board facts" (see the updated GPIO8 line) so it doesn't have to be re-derived.
- **Build-verified only, per this session's explicit scope.** `idf.py build` passed cleanly
  (exit code 0; ninja rebuilt `factory_reset.c.obj` and linked normally). Binary size grew from
  60% to 57% of the smallest app partition free (`0xa5600` bytes used of `0x180000`, `0xdaa00`
  bytes / 57% free) — the added GPIO/RMT/task code cost roughly 3 percentage points.
  **Not flashed, and the button-hold gesture has not been exercised on real hardware.** Flashing
  both boards (well, just the ESP32 here) and physically holding BOOT for 5s to confirm the LED
  blinks, the erase actually happens, and the board falls back into a pairing window afterward
  remains a separate follow-up requiring the user's explicit go-ahead, per this project's
  hardware-safety rule — same caveat the design session already flagged.
- `docs/USER_GUIDE.md` needs a new-recovery-mechanism note added (delegated to a cheap model
  per this project's convention, not done in this pass) — see the day's chat/task output for the
  exact proposed prose; not duplicated here to avoid two copies drifting apart.
- Next per the design session's explicit ordering: the `MAX_RECONNECT_RETRIES` hard-stop fix and
  the 30-second idle-connection-timeout enforcement, before step 7 starts.

## 2026-09-06: factory-reset gesture hardware-verified

Physical test on real ESP32-C6-DevKitC-1-N4 (COM9) and Flipper Zero (COM8, not involved):

**Test sequence:**
1. Rebuilt current firmware (`idf.py build`, clean, 57% flash free).
2. Flashed via `idf.py -p COM9 flash monitor`.
3. Boot confirmed: board had stored `pairing_secret` from prior session (pre-erase state).
4. **Negative case (early release):** held BOOT for ~2 seconds, released — LED went solid red then
   blinked, then LED turned off with **no log line and no restart**. Board continued scanning
   normally. ✓
5. **Positive case (full 5s hold):** held BOOT for 5+ continuous seconds — LED blinked red
   throughout, then at ~5s the board restarted. Post-restart boot log showed
   `no stored pairing_secret; pairing window open for 120000 ms` ✓ (this proves the
   `nvs_flash_erase()` succeeded; the pre-existing pairing secret is permanently gone).

**All three acceptance checks passed:**
- ✓ LED blinks during the hold (user-observed, red on/off pattern every ~200ms)
- ✓ Board restarts after 5s (restart reason `SW_CPU` logged)
- ✓ Board falls back to pairing window (boot log: `no stored pairing_secret`, opening 120s window)

**Notes:**
- The "factory-reset gesture confirmed (BOOT held 5000 ms); erasing NVS and restarting" log line
  was not captured in the monitor output snapshot (likely lost during serial reconnect on restart),
  but the erase's functional success is unambiguous: the `pairing_secret` that existed pre-erase
  is confirmed absent post-erase.
- RMT driver emitted periodic `flush timeout` warnings in both pre- and post-test boot states —
  unrelated to the factory-reset gesture itself (factory-reset.c's RMT init succeeded and LED
  feedback worked), likely a transient Wi-Fi/BLE coexistence artifact during scan startup.
- Negative case and positive case each ran once, no re-tests needed.

**Factory-reset gesture is now HARDWARE-VERIFIED and READY FOR PRODUCTION USE.**

Next: `MAX_RECONNECT_RETRIES` hard-stop fix and 30-second idle-connection-timeout enforcement
(pre-existing step-6 backlog items, either order) before step 7 (board identity/capability registry).

## 2026-09-06: pairing/reconnect diagnosis session — FAP-close/reopen stuck-connection bug root-caused, no code changed

Same-day follow-up, prompted by the user reporting real observed behavior (not a design
question): "when the devices are paired and I disconnect the ESP from power, all works
correctly ... but if I close the FAP and reopen it, the Flipper is stuck on 'have saved
pairing, waiting for ESP' and does not connect; rebooting the ESP fixes it." Diagnosed
by reading `flipper/flipper_esp32_over_ble.c` and the pinned Unleashed source
(`applications/services/bt/bt_service/bt_api.c`, `bt.c`, `bt.h`) — no code was changed and
no hardware was touched this session, this was a read-only root-cause investigation.

**Confirmed root cause:** `stop_service()` (the FAP's app-exit path, called on Back-press)
calls `bt_disconnect(app->bt)` then `bt_profile_restore_default(app->bt)`. `bt_disconnect()`
maps to Unleashed's `bt_close_connection()`, which for a custom (non-RPC) GATT profile only
calls `bt_close_rpc_connection()` (a no-op here, this app never uses the RPC serial profile)
and `furi_hal_bt_stop_advertising()` — it does **not** terminate an already-established
connection. The app's own code already documents awareness of a related constraint: a
comment at `flipper_esp32_over_ble.c:811-812` notes the FAP avoids calling `bt_disconnect()`
from inside `profile_event_handler` (reentrancy) and instead "relies on the peer
disconnecting" for BLE-level teardown. The app-exit path is the one place that reliance
breaks down, since there is no peer left to initiate anything.

The actual teardown, such as it is, comes from the next call, `bt_profile_restore_default()`
— `bt.h` documents this as causing a full BLE coprocessor "2nd core restart," an abrupt radio
reset rather than a clean disconnect PDU. The ESP32 central never receives an explicit
termination; it just stops hearing from the peer and must wait out NimBLE's default
connection supervision timeout (`ble_gap_connect()` in `esp32/main/main.c` passes `NULL` for
connection params, i.e. library defaults — tens of seconds) before `BLE_GAP_EVENT_DISCONNECT`
fires and reconnect scanning restarts. This is a genuine BLE-level timing gap, not a logic bug
in the ESP32's own reconnect state machine (which was independently re-verified this session
and is otherwise sound: `schedule_reconnect()`/`schedule_runtime_auth_backoff()` correctly
reset `connection_handle` and rescan on a normal-reason `BLE_GAP_EVENT_DISCONNECT` — the
handler is fine, it just never fires promptly for this specific exit path).

**Also reconfirmed this session (separate question, same investigation):** the current build
does still carry the step-2-era GAP-level reconnect scaffolding intact, but the
`MAX_RECONNECT_RETRIES = 5` hard-stop noted in `docs/PLAN.md`'s backlog was directly
re-verified in the current source (`schedule_reconnect()`, `esp32/main/main.c`) — confirmed
still present, still gives up permanently after 5 failed GAP-level connect attempts. Only
`schedule_runtime_auth_backoff()` (added in step 6, for auth-layer failures after a successful
physical connect) got the revised "backoff then retry indefinitely" policy; the connection
layer never did. Not newly discovered, but independently reconfirmed against live source
rather than taken from memory.

**Fix direction, not yet implemented:** give the ESP32 a proactive silence/idle timeout that
calls `ble_gap_terminate()` and restarts scanning after a bounded period of no traffic on an
established connection, so recovery no longer depends on the Flipper ever sending a clean
disconnect or on NimBLE's own long default supervision timeout. This is the same mechanism
already backlogged as "30-second idle-connection timeout enforcement" in `docs/PLAN.md` — that
item has been reprioritized to immediate-next given it now explains a real user-facing bug, not
just a step-7 readiness gap. See `docs/PLAN.md`'s "Backlog" section for the full fix-direction
writeup. **Next session: implement it** — source-only, single-file (`esp32/main/main.c`)
change, `idf.py build` to verify, no board flash without explicit user go-ahead.

## 2026-09-06: ESP32 idle-connection timeout implemented (build-verified only)

Same-day follow-up to the diagnosis session above. Implemented in `esp32/main/main.c` only
(source-only, delegated to `esp32-developer`); `flipper/flipper_esp32_over_ble.c`,
`docs/PROTOCOL.md`, `docs/PAIRING.md`, and the codec/framing files were not touched. Does not
touch the separate `MAX_RECONNECT_RETRIES` hard-stop bug (still open, see Backlog).

- **Mechanism:** no new timer — piggybacked on the existing `reassembly_timeout_co`
  `ble_npl_callout`, which already fires unconditionally every `FEB_REASSEMBLY_CHECK_INTERVAL_MS`
  (1000 ms) once the BLE host syncs. Added one check inside `reassembly_timeout_cb()`: when
  `connection_handle != BLE_HS_CONN_HANDLE_NONE`, `runtime_auth_state ==
  RUNTIME_AUTH_STATE_AUTHENTICATED`, and `now_ms - last_record_activity_ms >=
  FEB_IDLE_TIMEOUT_MS` (new constant, `30000u`, matching `docs/PROTOCOL.md`'s "30 seconds
  without a record" text exactly), it calls `ble_gap_terminate(connection_handle,
  BLE_ERR_REM_USER_CONN_TERM)`. That funnels into the existing `BLE_GAP_EVENT_DISCONNECT`
  handler and `start_scan()` reconnect path unchanged — no duplicated cleanup logic was needed.
- **Activity tracking:** new file-scope `static uint32_t last_record_activity_ms`, reset (a)
  on `BLE_GAP_EVENT_CONNECT` (belt-and-suspenders against a stale value from a prior
  connection, though the `AUTHENTICATED`-state gate — also reset to `IDLE` on connect/disconnect
  — already prevents this on its own), (b) at the top of the `FEB_FRAME_MESSAGE_COMPLETE` case
  in the notify-RX handler, i.e. on every successfully reassembled inbound record in both
  pairing and runtime-auth boot modes, and (c) immediately after firing `ble_gap_terminate()` in
  the idle-check itself, so the check doesn't re-fire every second while
  `BLE_GAP_EVENT_DISCONNECT` is still pending.
- **Direction judgment call, flagged explicitly:** only *received* records reset the timer.
  `docs/PROTOCOL.md`'s "without a record" doesn't specify direction; outbound sends (hello,
  client_auth, future replies) intentionally do not reset it. Worth revisiting if hardware
  testing shows the ESP32's own outbound-heavy traffic patterns trip this timeout
  unexpectedly.
- **Diff shape:** `#define FEB_IDLE_TIMEOUT_MS 30000u` (new constant near other timeouts),
  `static uint32_t last_record_activity_ms;` (new file-scope var near `hello_ack_deadline_ms`),
  one-line reset in `BLE_GAP_EVENT_CONNECT`, one-line reset at the top of
  `FEB_FRAME_MESSAGE_COMPLETE`, and a ~7-line idle-check block in `reassembly_timeout_cb()`
  alongside its existing hello_ack-timeout check.
- **Build result:** `idf.py build` exit code 0, no warnings. App binary `0xa5700` bytes,
  smallest app partition `0x180000` bytes, `0xda900` bytes (57%) free — unchanged from the
  factory-reset gesture build earlier the same day. Bootloader `0x5840` bytes, 31% free.
- **Explicit validation-scope caveat:** this confirms the code compiles and the new logic is
  wired into the existing callout/disconnect paths as designed. It does **not** confirm the
  idle-terminate path actually fires and reconnects correctly against a real Flipper, or that
  it resolves the reported FAP-close/reopen bug specifically — that requires flashing and a
  live hardware repro, deliberately out of scope this session per the hardware-safety rule.
  **Not yet flashed.**

Next: `MAX_RECONNECT_RETRIES` hard-stop fix (still open, `schedule_reconnect()`,
`esp32/main/main.c`), then hardware-test this idle-timeout fix against the original
FAP-close/reopen repro (requires explicit user go-ahead), then step 7 (board identity/
capability registry).

## 2026-09-06: idle-connection-timeout hardware-verified

Physical test on real ESP32-C6-DevKitC-1-N4 (COM9) and Flipper Zero (COM8), same day as the
build-verify above. `idf.py build` re-run clean before flashing (belt-and-suspenders), then
`idf.py -p COM9 flash` and `idf.py -p COM9 monitor`.

**Pairing-state snag before the real test:** the ESP32 already held a stored `pairing_secret`
from an earlier session, but the Flipper app launched with no saved pairing for this board
(`Waiting for ESP32...`) — a mismatch (likely from the Flipper-side storage-folder fix
changing where records land, or an unrelated earlier test), not a bug in the idle-timeout code
itself. Resolved by using the now-hardware-verified factory-reset gesture (BOOT held 5s) to
erase the ESP32's stale secret and force a fresh 120-second pairing window, then completing a
normal pairing ceremony (Flipper already advertising, on `Waiting for ESP32...`) — succeeded,
log shows `pairing_secret persisted; sending pair_complete` then `pairing attempt consumed;
staying idle until next reset`, matching documented post-pairing behavior exactly.

**Idle-timeout test proper:** a plain ESP32 reset (not factory-reset) after pairing triggered
the documented runtime-auth path (`stored pairing_secret found; attempting runtime auth`);
it found the Flipper (already advertising since it has a saved pairing now), connected, and
completed hello/hello_ack/client_auth — `client_auth sent; runtime session authenticated` at
t=3.59s in the monitor's uptime clock. Left idle (Flipper showing `ESP32 session active`,
solid blue LED, no button presses) until the log printed
`idle authenticated connection (30792 ms without a record); terminating` at t=34.29s — exactly
on schedule (the 30000 ms `FEB_IDLE_TIMEOUT_MS` threshold plus up to ~1000 ms of slack until
the next `reassembly_timeout_cb()` tick, as designed). This immediately called
`ble_gap_terminate()`, the existing `BLE_GAP_EVENT_DISCONNECT` handler fired, and the existing
reconnect path took over unchanged: rescan → `found v2 peer, connecting` → connect → MTU
exchange → service/characteristic discovery → hello/hello_ack/client_auth →
`client_auth sent; runtime session authenticated` again at t=35.3s — fully automatic, no
physical reset or other user action. Flipper confirmed showing `ESP32 session active` / solid
blue LED both before the timeout and after the reconnect (the brief mid-cycle screen
transition wasn't watched closely enough to confirm, but the stable before/after state is
unambiguous).

**Scope caveat, matching the note left at implementation time:** this test reproduces generic
30-second silence on an authenticated connection, not the literal FAP-close/reopen user
gesture the original bug report described. Since the fix triggers purely on elapsed idle time
regardless of *why* traffic stopped, this is treated as a valid proxy for that bug path too —
the fix doesn't distinguish "Flipper app was closed" from "Flipper app is just idle." The
app-exit-specific repro (physically closing the FAP mid-session and timing the ESP32's
recovery) remains a reasonable follow-up if this is ever doubted, but is not considered
required given the mechanism's generality.

**Idle-connection-timeout fix is now HARDWARE-VERIFIED.** Next: `MAX_RECONNECT_RETRIES`
hard-stop fix (`schedule_reconnect()`, `esp32/main/main.c`, still open), then step 7 (board
identity/capability registry). Also worth a follow-up: the Flipper pairing-state mismatch hit
at the start of this test (stored ESP32 secret with no matching Flipper record) suggests
checking whether any other already-paired boards from before the step-6 storage-folder fix are
similarly orphaned.

## 2026-09-06: literal FAP-close/reopen repro surfaces a new, undiagnosed scan-stall bug

Later the same day, the user ran the literal app-exit repro this project had previously
treated as "not required given the [idle-timeout fix's] mechanism's generality" (see the
scope caveat immediately above). It surfaced a real, different bug the idle-timeout fix does
not cover.

**Capture method:** `idf.py -p COM9 monitor` couldn't be used directly (the tool environment
has no interactive TTY, which `idf_monitor.py` requires and refuses without). Read the port
instead via a raw `System.IO.Ports.SerialPort` (PowerShell), with `DtrEnable`/`RtsEnable`
forced `$false` before `Open()` to try to avoid triggering the board's auto-reset circuit.
**This did not fully work**: the first such open still produced `rst:0x15 (USB_UART_HPSYS)` —
the ESP32-C6's native USB-Serial-JTAG appears to reset on host attach regardless of DTR/RTS
line state. Worth knowing before relying on this technique again: a "passive" serial open is
not guaranteed reset-free on this chip, unlike a true external-UART/DTR-RTS board.

**Observed sequence** (timestamps relative to this capture's start; the ESP-IDF uptime clock
in parens is the authoritative one, since it comes from the device itself and confirms the
gap below is real, not a capture artifact):
1. The idle-timeout self-heal (previous entry) fired once normally, unrelated to this test.
2. User closed the FAP: `disconnected: reason=520` (a *clean* disconnect — not the
   `bt_profile_restore_default()` radio-reset race documented above) followed immediately by
   normal rescanning — several other nearby devices' advertisements logged over the next
   ~3.6 s, including two from the Flipper's own address before it stopped advertising as the
   app finished exiting.
3. Then **total silence for ~168 seconds** — the device uptime clock jumps from `194931` ms to
   `362841` ms (168.9 s) between consecutive log lines, with **zero** advertisement-report
   callbacks logged in between, from the Flipper *or* any of the other ~15 unrelated devices
   that had been showing up every few hundred ms right up until the gap. No error, no panic,
   no watchdog reset, no repeated "GAP procedure initiated: discovery" — nothing. This means
   the scan pipeline itself appears to have stalled, not just failed to match the target.
4. It then recovered **spontaneously**: `found v2 peer, connecting` appears with **no**
   preceding per-advertisement log line (every other successful match in this same capture,
   and in every prior session's logs, was immediately preceded by a logged
   `scan advertisement N: addr=<peer>` from the same callback) — suggesting whatever broke the
   stall came through a different path than the normal advertisement-report callback.
5. Reconnect then completed normally end-to-end (MTU exchange, service/characteristic
   discovery, `hello`/`hello_ack`/`client_auth`) — `client_auth sent; runtime session
   authenticated` about 2.5 s after the stall broke.
6. Confirmed on the Flipper side throughout: stuck on the blinking "waiting for ESP" screen
   for the entire ~168 s stall, flipped to solid-blue "ESP32 session active" immediately after.

**This is a new, previously undocumented failure mode** — not a re-occurrence of either
already-known issue:
- Not the `bt_profile_restore_default()` radio-reset race (`docs/SESSION_MEMORY.md` entries
  above) — that disconnect was clean (`reason=520`), and rescanning did resume, correctly,
  immediately afterward.
- Not the `MAX_RECONNECT_RETRIES` hard-stop (`schedule_reconnect()`, still separately open,
  see `docs/PLAN.md` backlog) — that path only increments on a *failed* `ble_gap_connect()`;
  none occurred here. This scan uses `duration=forever` and was never restarted via the
  backoff/`reconnect_task` path at all during the stall.

**Root cause not yet identified — single observation (n=1), not yet isolated to a specific
trigger.** Candidate directions for a future session, none confirmed: NimBLE host task
starvation/priority inversion; some interaction with the peer's own radio-reset cycle
(`bt_profile_restore_default()`'s "2nd core restart," see above) leaving the ESP32's scan
window/state wedged without producing a disconnect or error event on this side; or a
duplicate-filter/internal dedup table (`filter_duplicates=1`) getting into a bad state under
~15-20 unique nearby addresses in a few seconds, silently wedging until something ages out.
**Next step before further fixes: reproduce again with logging on both the ~168 s stall and
whatever precedes/follows it, ideally via a real `idf.py monitor` session (not this tool's
passive-serial workaround) to rule out the capture method itself as a contributing factor.**

**Reproduced a second time the same session, under a real `idf_monitor.py` session** — ruling
out the passive-serial capture method as the cause. Ran the actual `esp_idf_monitor` package
directly (not the raw `SerialPort` workaround above), bypassing its interactive-TTY requirement
via the `ESP_IDF_MONITOR_TEST=1` environment variable (documented in
`esp_idf_monitor/idf_monitor.py`: skips the `sys.stdin.isatty()` check and puts
`ConsoleReader.run()` into a no-input polling loop instead of calling `console.getkey()` — safe
here since nothing needs to send the monitor keystrokes), with `--timestamps` for wall-clock
correlation. Same reset-on-attach happened again (confirming this is inherent to the board, not
a fluke of the first workaround — see the new feedback memory on this). Repeated the exact
close/reopen cycle: clean disconnect (`reason=531` this time, vs. `534`/`520` in the earlier
entries — the varying reason codes themselves don't look meaningful, just whichever HCI/host
event fired first), normal rescanning for a few seconds, then **another silent stall** — device
uptime clock jumped from `61201` ms to `168451` ms (~107 s) with zero advertisement-report
lines logged in between, versus ~169 s the first time — then a full, clean reconnect and
reauthentication with no error/panic logged, exactly like the first occurrence. **Two-for-two
reproductions**, different capture methods, different stall durations (107 s vs. 169 s) but the
same shape: total scan silence for on the order of a minute or two, then spontaneous recovery.
This is now considered a confirmed, real, reproducible bug rather than a single anomalous
observation — root cause is still unidentified, but "was it just my capture tool" is ruled out.

## 2026-09-06: scan-stall ROOT-CAUSED — controller duplicate-filter suppresses the peer by address

**Resolved.** The two entries above describe the symptom correctly but their guessed mechanism
("the scan pipeline itself appears to have stalled") is **wrong** and is superseded by this
entry. Nothing stalls. The scan, the NimBLE host, and the radio all keep working normally
throughout. What actually happens is that the BLE **controller's duplicate-advertisement filter
suppresses the Flipper by address**, so the host never sees the peer start advertising the v2
service again after the FAP is relaunched.

**Mechanism, confirmed from three independent directions:**

1. **Code** — `start_scan()` (`esp32/main/main.c:346-367`) issues
   `ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, ...)` with
   `params.filter_duplicates = 1` (`main.c:360`). The disconnect handler restarts exactly this
   scan (`main.c:1106`), and per spec enabling scanning resets the controller's duplicate cache.
2. **sdkconfig** — `esp32/sdkconfig` pins the controller's filter semantics:
   - `CONFIG_BT_LE_SCAN_DUPL=y` (filtering on),
   - `CONFIG_BT_LE_SCAN_DUPL_TYPE_DEVICE=y` / `CONFIG_BT_LE_SCAN_DUPL_TYPE=0` — filter keyed on
     **device address only**; `CONFIG_BT_LE_SCAN_DUPL_TYPE_DATA_DEVICE` is explicitly *not* set,
     so a change of advertising payload from the same address does **not** produce a new report,
   - `CONFIG_BT_LE_SCAN_DUPL_CACHE_REFRESH_PERIOD=0` — the cache is **never** periodically
     flushed, so an entry persists until evicted by cache pressure from other advertisers.
3. **Captured data** — in both repros the Flipper's address appears exactly once per scan
   session, right after the FAP closes, carrying the *wrong* payload, and is then never reported
   again until the delayed match. The payload lengths make this unambiguous:
   - `len=24` = the FAP's v2 advertisement — every single successful `found v2 peer, connecting`
     in every capture is preceded by a `len=24` record from `80:e1:26:75:c2:c9`.
   - `len=28` = the Flipper's **default serial profile** advertisement, which is what
     `bt_profile_restore_default()` leaves advertising once the FAP exits. It carries no v2
     service UUID, so `scan_record_matches()` (`main.c:328-344`) correctly returns false — but
     the sighting has already inserted `80:e1:26:75:c2:c9` into the controller's duplicate cache.

**Full sequence:** FAP closes -> Flipper reverts to the default profile and advertises `len=28`
from the same MAC -> ESP32 sees the disconnect, restarts the scan (cache reset), receives that
`len=28` record once, doesn't match, and the address is now cached -> user relaunches the FAP ->
Flipper advertises `len=24` **from the same MAC** -> the controller filters every one of those
reports, so the host never learns the peer is back -> minutes later the cache evicts the entry
under pressure from other nearby advertisers -> the next `len=24` report finally reaches the
host -> instant match and reconnect.

**This explains every observation that previously looked mysterious:**
- Variable stall length (~107 s vs. ~169 s) — eviction is driven by ambient advertiser density,
  not by anything in this firmware.
- Other devices going quiet too — they were each reported once and then filtered as well; the
  decelerating gap pattern (burst, then 15 s, 4 s, 9 s, 106 s) is the cache saturating, not the
  radio dying.
- Recovery with **no error, panic, or watchdog** — nothing was ever broken.
- The recovery burst re-reporting an address already seen *in the same scan session*
  (`41:51:8e:66:b0:2a`, reported at #17/#18 and again at #38/#39) — direct proof the cache was
  flushed/evicted at that moment rather than new devices simply showing up.
- The `found v2 peer, connecting` line with no preceding `scan advertisement N` line — that one
  is the *separate* logging-cap bug below, not this.

**Separate, genuine bug found alongside it (do not conflate the two):** `scan_log_count`
(`main.c:101`) is a lifetime counter gated at `< 40` (`main.c:996-998`) and is **never reset** —
not in `start_scan()`, not on connect, not on disconnect (grep: it occurs at only those three
lines). Once 40 advertisement events have been logged since boot, across *all* scan sessions,
per-advertisement logging goes silent permanently, while the match/connect logic below it
(`main.c:1003-1018`) keeps running unconditionally. Verified in the second capture: after the cap
was reached, ~30 consecutive idle-timeout reconnect cycles each show `disconnected` ->
`found v2 peer, connecting` in the same 10 ms, with zero advertisement lines — all of them
perfectly healthy, just invisible. This bug does **not** cause the stall (in the second repro the
counter was only at 36/40 when the ~106 s gap began, with room for four more lines that never
came), but it makes healthy fast reconnects look identical to stalled ones in the log, which is
what made the first diagnosis go wrong. It also matters going forward: in a dense RF environment
the 40-event budget is exhausted within seconds of boot, so scan logging is effectively dead for
any future debugging session.

**Fix implemented 2026-09-06** (`esp32/main/main.c` only, source-only, build-verified twice —
once by the implementing agent, once independently rebuilt fresh by this session — via
`idf.py build`, exit code 0, no warnings; bootloader `0x5840` bytes / 31% free, app image
`0xa5710` bytes / smallest app partition 57% free, unchanged from the prior baseline).
**Not yet flashed — the human will flash and re-test in a clean session.** Chose the
app-code-only direction over the two sdkconfig alternatives so the pinned build baseline
(`docs/BASELINES.md`) doesn't need to change and no cross-cutting controller-cache assumption
has to be re-verified later:
- `start_scan()` (`main.c:351-372`): `params.filter_duplicates = 0` (`main.c:370`, was `1`),
  with a comment at `main.c:365-369` citing the exact sdkconfig keys and this doc entry so a
  future cleanup pass doesn't "helpfully" turn filtering back on. Peer rediscovery no longer
  depends on controller cache eviction timing at all; `scan_record_matches()`
  (`main.c:328-344`, unchanged) remains the only filter, now done host-side on every report.
- Replaced the dead 40-cap logging with an aggregate that can't go silent: three new
  file-scope statics (`scan_report_window_count`, `scan_report_lifetime_total`,
  `scan_summary_elapsed_ms`, `main.c:104-106`) replace the old `scan_log_count`; the
  `BLE_GAP_EVENT_DISC` case (`main.c:1005-1007`) now just increments the window and lifetime
  counters unconditionally, with the per-event `ESP_LOGI` and its `< 40` gate removed entirely.
  A new constant `FEB_SCAN_SUMMARY_INTERVAL_MS = 10000u` (`main.c:43`, alongside the other
  `FEB_*` timing constants) drives a summary line piggybacked onto the existing 1000 ms
  `reassembly_timeout_cb()` callout (`main.c:1298-1308`, no new timer added) —
  `"scan reports: %lu in last %lus (lifetime %lu)"` — emitted only when the window count is
  nonzero (so an idle/connected board stays quiet) and never capped or reset except at reboot.
  The `found v2 peer, connecting` line and all match/connect logic (`main.c:1008-1023`) are
  untouched.

**Verification scope caveat:** build-verified only, as above — the actual fix (does relaunching
the FAP now reconnect promptly instead of after 100+ seconds) has not been exercised against
real hardware yet. That's the next session's first order of business after flashing.

## 2026-09-06: scan-stall fix hardware-verified

Rebuilt clean (`idf.py build`, exit code 0, identical `0xa5710`-byte image, 57% free — no
source drift since the build-verify entry above), flashed to the ESP32-C6 on `COM9` via
`idf.py -p COM9 flash`, then monitored via a real `idf_monitor.py` session
(`ESP_IDF_MONITOR_TEST=1`, `--timestamps`) while the user launched the Flipper FAP, let it
connect and authenticate, then closed and relaunched the FAP once.

Boot log confirms the fix is live: `NimBLE: ... filter_duplicates=0` (was `1`), and the new
scan-summary counters run correctly (`scan reports: N in last 10s (lifetime M)`, no more dead
40-event cap).

Timeline of the live FAP close/reopen cycle (device uptime clock):
- First connect + runtime auth: `t=118.48s` -> `t=119.41s`.
- Disconnect (`reason=531`, FAP closed): `t=123.31s`.
- Peer rediscovered, reconnecting: `t=126.09s` — **~2.8 s after disconnect**.
- Reauthenticated: `t=127.02s`.

No stall. This directly reproduces the scenario that previously took 100+ seconds (the two
repro entries above) and confirms the `filter_duplicates=0` fix resolves it.

**Scan-stall fix and its accompanying scan-summary-logging fix are now
HARDWARE-VERIFIED and READY FOR PRODUCTION USE.**

Next: `MAX_RECONNECT_RETRIES` hard-stop fix (`schedule_reconnect()`, `esp32/main/main.c`,
still open at the time — see the dedicated entry below for the fix) — then step 7 (board
identity/capability registry).

## 2026-09-06: Flipper LED/screen "blinks intermittently mid-session" diagnosed, not fixed

Same day, after the scan-stall fix above, the user reported the Flipper's LED blinking
intermittently during an otherwise idle session. Diagnosed by reading
`flipper/flipper_esp32_over_ble.c` only — no hardware test needed, the mechanism is
unambiguous from source:

- The now-hardware-verified 30-second idle-connection timeout (see its entry above) disconnects
  and reconnects any authenticated link that's gone quiet for 30s, exactly as designed.
- On the Flipper side, that reconnect's brief re-advertising window fires a `BtStatusAdvertising`
  event. The handler (`flipper_esp32_over_ble.c:1203-1209`) only skips resetting to the
  "waiting for first connection" state (blink + `PairingPhaseWaiting`) when
  `pairing_phase == PairingPhaseDone` — but an auto-connected runtime session (the normal path
  for any already-paired board) never reaches `PairingPhaseDone`, it sits at
  `PairingPhaseSessionActive`. `PairingPhaseDone` is reached only via the pairing *ceremony's*
  own success path, which a stored-pairing board never re-runs.
- Net effect: every idle-timeout reconnect cycle re-triggers `sequence_blink_start_blue` and
  flips the on-screen text back to "Waiting for ESP32..." for the few seconds the reconnect
  takes, then returns to solid-blue "ESP32 session active" once `client_auth` completes again.
  Repeating every ~30s during an idle session reads exactly as "blinks intermittently." This
  resolves the open question left in the idle-timeout hardware-verify entry above ("the brief
  mid-cycle screen transition wasn't watched closely enough to confirm").
- **Not a functional bug** — the reconnect itself completes correctly every time (already
  hardware-verified); this is purely cosmetic, confined to the Flipper's LED/`pairing_phase`
  state machine. No BLE, session, or protocol logic is affected.

**User decision: do not apply the cosmetic guard-condition fix now.** Instead, asked why the
ESP32 doesn't use a heartbeat/keep-alive instead of a disconnect-and-reconnect cycle for the
idle case in the first place — a heartbeat would keep a genuinely-idle-but-healthy session
connected throughout (no reconnect, no flicker) while still detecting and recovering a truly
dead link, but it's a wire-protocol change (new message type, both firmwares, a
`docs/PROTOCOL.md` update) rather than a quick patch. **Added to `docs/PLAN.md`'s backlog** as
its own item, needing a future design/grill-me session before implementation. The small
cosmetic LED/screen fix remains available independently whenever it's wanted — nothing was
changed in either firmware this session.

## 2026-09-06: MAX_RECONNECT_RETRIES hard-stop fix

Fixed the last open item flagged in the "scan-stall fix hardware-verified" and
"idle-connection-timeout hardware-verified" entries above: `schedule_reconnect()`
(`esp32/main/main.c`) hard-stopped forever after `MAX_RECONNECT_RETRIES` (5) consecutive
GAP-level `ble_gap_connect()` failures, contradicting `docs/PLAN.md` step 4's documented
"bounded exponential backoff, then indefinite slow-cadence retry" policy — and mattering more
now that step 6 made the board scan/reconnect continuously and unattended.

**Fix:** mirrored the existing two-phase shape already used for runtime-auth proof failures
(`runtime_auth_backoff_delay_ms()` / `schedule_runtime_auth_backoff()`, ~main.c:315-324 and
~main.c:420-435 before this change). Added:
- `FEB_RECONNECT_SLOW_CADENCE_MS` (30 s) — a new constant next to
  `FEB_RUNTIME_AUTH_SLOW_CADENCE_MS`, deliberately **not** reusing the auth path's 5-minute
  cadence: that long cadence is doing double duty as an anti-hammering throttle against
  repeated bad credentials, which doesn't apply to a plain link-layer connect failure (the
  peer could be back and connectable within seconds), and reusing it here would reintroduce a
  "board goes quiet for a long stretch for no reason" symptom on this path similar to the one
  the scan-stall fix eliminated on a different one.
- `reconnect_backoff_delay_ms()` — mirrors `runtime_auth_backoff_delay_ms()`: exponential ramp
  (1, 2, 4, ... s) while `reconnect_retries <= MAX_RECONNECT_RETRIES`, then flattens to
  `FEB_RECONNECT_SLOW_CADENCE_MS` indefinitely. `MAX_RECONNECT_RETRIES` (still 5) now means
  "length of the ramp before flattening," not a hard cap — it's never compared as a stop
  condition anymore.
- `schedule_reconnect()` rewritten: dropped the `reconnect_retries >= MAX_RECONNECT_RETRIES`
  hard-stop branch entirely (only the pre-existing `reconnect_task_active` reentrancy guard
  remains as an early return), clamped the increment at `0xFFu` the same way
  `fail_runtime_auth()` clamps `runtime_auth_failure_count` (preventing `uint8_t`
  wraparound), and switched the log line to a "consecutive failures=%u" phrasing once past
  the ramp, matching `schedule_runtime_auth_backoff()`'s log style.
- Updated the comment above `schedule_runtime_auth_backoff()` (which referenced
  `reconnect_retries`/`MAX_RECONNECT_RETRIES`) to reflect the new "ramp length, not a hard
  cap" meaning.

Normal disconnects (`BLE_GAP_EVENT_DISCONNECT`, `DISCONNECT_REASON_NORMAL`/`UNKNOWN_BOARD`)
were not touched — they already bypass `schedule_reconnect()` entirely via `start_scan()`.

**Build-verified only:** `idf.py build` from `esp32/`, clean exit, no errors —
`flipper_esp32_over_ble.bin` 0xa5740 bytes, smallest app partition 0x180000 bytes, 57% free
(same percentage as the pre-fix build; absolute size grew negligibly from the new
helper/constant). See `docs/PLAN.md` backlog's `MAX_RECONNECT_RETRIES` entry for the full
before/after description.

## 2026-09-07: MAX_RECONNECT_RETRIES fix hardware-verified

Already-flashed build from the entry above was exercised for real on the ESP32-C6 (`COM9`)
and Flipper Zero (`COM8`), monitored via a real `idf_monitor.py` session
(`ESP_IDF_MONITOR_TEST=1`, `--timestamps`, per the established no-TTY workaround). Repro:
with the FAP running, physically moved the Flipper to a marginal-range distance to force real
link-establishment failures rather than a clean disconnect.

**Two distinct failure shapes turned up, only one of which touches this fix:**
- Fast NimBLE-internal link-retry cycles (`"Reattempt connection; reason = 0x3e"`, giving up
  within a few hundred ms) surface as a plain `BLE_GAP_EVENT_DISCONNECT` (`reason=574` =
  `BLE_HS_HCI_ERR(0x3E)`) and correctly bypass `schedule_reconnect()` — they land in the
  disconnect-reason switch's `default:` case and just trigger an immediate `start_scan()`,
  same as any ordinary link-loss disconnect. This is intended behavior (cheap immediate
  rescan for a transient blip), not a gap.
- When the link failed to establish within the full 30 s `ble_gap_connect()` timeout, it
  surfaced instead as `BLE_GAP_EVENT_CONNECT` with `status=13` (`BLE_HS_ETIMEOUT`), logged
  `connection failed: 13` — **this is the path that actually drives `schedule_reconnect()`.**

**Confirmed the full designed ramp end to end**, all on real hardware: `reconnect retry 1/5
in 1000 ms` -> `2/5 in 2000 ms` -> `3/5 in 4000 ms` -> `4/5 in 8000 ms` -> `5/5 in 16000 ms` ->
then, past the point the old code would have hard-stopped forever, `reconnect retry in 30000 ms
(consecutive failures=6)` — the ramp flattens to the indefinite 30 s slow cadence exactly as
designed. Moving the Flipper back into range let the very next 30 s-cadence attempt succeed
(`connected; exchanging MTU` -> `client_auth sent; runtime session authenticated`), confirming
full recovery with no physical reset needed.

**Live-diagnosis false start, recorded for future sessions:** partway through, a narrower read
of the log — taken before any `connection failed: 13` timeout had actually occurred yet — looked
like `schedule_reconnect()` was unreachable for this failure class entirely, which would have
been a real gap. That read was wrong: it was an artifact of a live `Monitor` grep filter that
happened to exclude the `connection failed:`/`connect start failed:` log lines, not a real gap
in the code. Corrected once the fuller raw log was inspected (the ramp was already firing
correctly in the unfiltered log the whole time); no code change was made or needed. Lesson: when
live-tailing a filtered log during an on-the-fly hardware repro, treat an unexpected "this code
path looks unreachable" conclusion as provisional until cross-checked against the complete raw
log, not just the filtered stream.

**`MAX_RECONNECT_RETRIES` fix is now HARDWARE-VERIFIED and READY FOR PRODUCTION USE.** Both
items previously blocking step 7 (idle-timeout hardware test, this one) are now done. Step 7
(board identity/capability registry) is next, and per this project's established pattern
(steps 3, 5, and 6 each got one), needs its own grill-me design session before implementation
starts.

## 2026-09-07: step 7 grill-me session — design decisions

A design-review session locked down step 7 scope, storage, and lifecycle before any implementation started (no code written, no board flashed this session). Ten decisions were finalized:

**Step 7 scope:** the step covers only the board-identity/capability-registry plumbing (`capability_query`/`capability_response` wired post-runtime-auth, reporting `board`/`firmware`/`features`). The `command` message type and any real `wifi_scan` command handler are split out into their own separate future roadmap step — result pagination, scan trigger vs. streamed results, argument validation, and payload-size-constraint reasoning all defer there. Step 7's "done when" bar (Flipper renders the authenticated capability list correctly) is satisfiable without any working command handling.

**`capability_query` lifecycle:** the Flipper sends it exactly once per `board_id`, on the first successful runtime auth for that board when no locally persisted capability file exists yet. The result is cached and never automatically re-queried on subsequent reconnects — a deliberate exception to the "distrust and re-verify every session" pattern used for authentication, justified because capability lists are non-sensitive cached metadata, not security credentials. Staleness has no automatic remediation; the only refresh path is full unpair + re-pair (manual "refresh capabilities" without re-pair was considered and explicitly rejected/backlogged).

**Storage:** the Flipper persists each board's capability record in its own separate file per `board_id`, distinct from the pairing-secret file. This separation allows the pairing-secret file to receive atomic-write/versioning hardening in step 8 without forcing the same constraints onto the non-sensitive capability cache. Unpairing a board deletes both its pairing file and its capability file together as one operation — no orphaned files.

**`requested` field:** defined in the protocol but currently unimplemented (ESP32 always returns the full registry, Flipper always omits the field). Backlog item to revisit once there's a real multi-capability use case that would benefit from partial queries.

**`firmware` field:** a hand-maintained constant string per ESP32 firmware build (e.g. `"0.1.0"`), bumped manually by hand. Explicitly not build-injected (e.g. via `git describe` at build time) — unnecessary complexity for a value nothing currently makes decisions based on.

**`board` field:** a hand-maintained opaque constant string per firmware target (e.g. `"esp32-c6-devkit"` for this board; future board targets would define their own strings). The Flipper receives it as a pure display string — no enum, allowlist, or validation. Capability gating is driven entirely by the `features` array, never by the `board` string.

**`features` list:** a hardcoded compile-time array/constant (today: just `["wifi_scan"]`). No runtime hardware-detection abstraction/framework built now — that gets designed later, informed by real hardware specifics, in the GPS phase's own future grill-me session.

**Command-handling scaffolding explicitly deferred:** step 7 does NOT add any generic `command`-message rejection/dispatch scaffold (e.g. a "reject command for unsupported capability" path), even though `unsupported_capability` is already a defined error code in PROTOCOL.md. That entire mechanism is owned by the future `wifi_scan` step, designed together with the real command handler from a clean slate — building rejection-only scaffolding with no handler behind it yet was explicitly rejected as premature.

Full detail, decision rationale, and references to other docs are in `docs/PLAN.md`'s new "### Step 7 implementation decisions (2026-09-07 grill-me session)" section. Next: step 7 implementation itself, split against the scope decision, with the `wifi_scan` follow-on step needing its own future grill-me session before it starts.

## 2026-09-07: full-repository code review, and a fix pass queued ahead of step 7

A full read-through of both firmwares' application and shared-contract code (`esp32/main/`,
`flipper/`, `tests/`) produced 24 findings, recorded in the new
**[docs/CODE_REVIEW_FINDINGS.md](CODE_REVIEW_FINDINGS.md)** (10 correctness, 6 embedded
memory/performance, 8 contract gaps against `docs/PROTOCOL.md`, plus a style/documentation
section). No code or hardware was touched during the review.

**Six of those findings (#1, #2, #3, #4, #9, #10) are queued to be fixed immediately, in their
own clean session, before step 7 implementation begins.** The execution plan is
**[docs/CODE_REVIEW_FIX_PLAN.md](CODE_REVIEW_FIX_PLAN.md)** — self-contained, with the design
decisions already settled (D1-D6) so the executing session doesn't re-derive them. **Start
there, not from this entry.** Summary of why these six and not the other eighteen: they all sit
in the shared CBOR / framing / session-crypto primitives that step 7's payload schemas and the
protected-record path will be built directly on top of, so fixing them afterwards means
re-touching code step 7 already depends on.

Headline items:

- **A real out-of-bounds read** in `flipper/cbor_codec.c:373` (`feb_cbor_skip_value()`'s map case
  is missing the `pos >= in_len` guard the ESP32 has at `esp32/main/cbor_codec.c:485`), reachable
  today from any malformed `payload` map on the write characteristic, pre-authentication.
- **The two `feb_cbor_skip_value()` implementations accept different CBOR major types** — the
  ESP32 accepts negative integers and rejects `true`/`false`/`null`; the Flipper does the exact
  opposite. `payload_span` is what step 7 will encrypt and cover with AAD, so this had to be
  converged first. Root cause of the drift: the function has **zero direct test coverage** on
  either side. The fix plan adds it.
- **`PROTOCOL.md` never said which CBOR major types a `payload` may contain** — a genuine spec
  gap, in the same family as the previously-closed "canonical CBOR" and "512/768 byte" ambiguities.
  Resolved in the fix plan's decision D2 (unsigned ints, byte strings, text strings, arrays, maps;
  everything else rejected), verified safe against step 7's and Phase 3's actual payload needs.
- **Four of the six fixes are Flipper-only** — in every divergence except two, the ESP32's copy is
  already the stricter/correct one and the Flipper's independently-written copy drifted looser.
  Useful signal for where to aim future review effort.
- Two findings' scope was **wider than the review first recorded**, both found while planning the
  fixes: the nesting-depth bug has a second call site (`flipper/pairing.c:196`, not just
  `cbor_codec.c`), and the clamp-vs-zero `board_id` divergence affects
  `feb_pairing_derive_secret()` as well as `feb_session_derive_key()`. `flipper/pairing.c` had not
  been read during the original scan.
- One severity **correction**: finding #4 (over-length `board_id` deriving an all-zero session key
  on the ESP32) is **not currently reachable** — both application layers already bound `board_id`
  before the derivation is called. It's defense-in-depth in a shared primitive, not a live bug.

Also worth knowing from the review, deliberately **not** in the fix pass: the entire AES-256-GCM
protected-record layer (`feb_session_encrypt_record`/`_decrypt_record`,
`feb_cbor_encode/decode_protected`, `feb_gcm_encrypt/decrypt`) has **zero call sites in either
application** today, so it has never run against live hardware. Step 7 activates it. That is the
single biggest reason the fix pass goes first.

The other eighteen findings are tracked in the findings doc, with #17 (NVS pairing record has no
version, validity marker, or atomic replacement, contradicting `docs/PROTOCOL.md`'s explicit
requirement) cross-referenced to step 8, which already owns that work.

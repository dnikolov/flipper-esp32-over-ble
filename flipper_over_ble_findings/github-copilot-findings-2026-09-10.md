# GitHub Copilot findings (2026-09-10)

## Review handoff

**Scope.** Reviewed all repository Markdown files, current ESP32 and Flipper sources, shared protocol/codec modules, configurations, host tests, current worktree diff, and independent platform-specific audits. This is a point-in-time review of the current worktree. It does not supersede the historical [docs/CODE_REVIEW_FINDINGS.md](docs/CODE_REVIEW_FINDINGS.md).

**Project summary.** This is a two-firmware BLE system: a Flipper Zero external FAP is the BLE peripheral/GATT server; an ESP32-C6 is the BLE central/GATT client. Reset-gated X25519 pairing produces a long-term secret. Each runtime connection uses HMAC proofs, HKDF-SHA-256, and AES-256-GCM protected CBOR records. Current capabilities include board discovery, Wi-Fi scan, BLE scan, and wardriving capture with ESP32 flash buffering and Flipper WiGLE CSV export.

**Protocol invariants.** Records are at most 768 bytes, plaintext payloads are at most 512 bytes, runtime sequences are 24-bit and must never wrap, and authentication/replay/sequence/nonce failures are fatal. Unsupported versions require an unencrypted `unsupported_version` error, then connection closure. [docs/PROTOCOL.md](docs/PROTOCOL.md) is authoritative.

**Worktree note.** Existing edits in `docs/`, `esp32/main/wardriving_validate.h`, `flipper/wardriving_csv.h`, `esp32/gps_antenna_test/`, and `tools/` were treated as user work; nothing was reverted or changed apart from this report.

## Priority order

1. F1: enforce the 24-bit AES-GCM sequence limit on both firmwares.
2. F2: require Flipper-side local authorization before accepting a pairing ceremony.
3. F3: make Flipper protected-record failures fatal, per the protocol.
4. F4: preserve wardriving backlog streaming when a `start` status is sent.
5. F5-F9: fix storage durability, version bounds, UI queue loss, MTU fallback, and timer rollover.

## Confirmed findings

### F1 - Critical: AES-GCM nonces are reused after the 24-bit sequence boundary

**Evidence.** The protocol encodes only 24 sequence bits in the nonce and prohibits wrapping in [docs/PROTOCOL.md](docs/PROTOCOL.md). Both nonce builders discard higher bits in [esp32/main/session.c](esp32/main/session.c) and [flipper/session.c](flipper/session.c). The ESP32 increments `rt_tx_sequence` after every protected send in [esp32/main/main.c](esp32/main/main.c); the Flipper increments unchecked `session_seq_out` after protected sends in [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c).

**Impact.** At sequence $2^{24}$, the nonce equals sequence zero for the same session ID and direction. AES-GCM nonce reuse under one key breaks encryption and authentication guarantees.

**Reproduce.** Build a nonce for sequence `0` and `0x1000000`; both 12-byte outputs are equal.

**Fix and acceptance.** Centralize `sequence <= 0xFFFFFF` validation in both send and receive state machines. Terminate before encrypting or accepting `0x1000000`. Add matching tests for `0xFFFFFF` accepted once and `0x1000000` rejected without encryption.

### F2 - High: Flipper accepts pairing without local authorization

**Evidence.** [docs/PAIRING.md](docs/PAIRING.md) says the user selects **Add ESP32 board** before the Flipper responds to `pair_init`. The dispatcher routes every valid `pair_init` to `handle_pair_init()` in [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c), where pairing starts immediately. The same FAP automatically advertises at launch if any pairing exists. No pairing-authorization state connects `pair_init` acceptance to the local input action.

**Impact.** Pairing deliberately lacks cryptographic peer authentication. A nearby attacker can connect as a central and complete pairing without a local gesture, overwriting/disrupting a known board record. ESP32 reset-gating does not prove the central sending `pair_init` is genuine.

**Fix and acceptance.** Set authorization only from Add-board input; clear it on connection changes, failure, completion, and timeout. Reject pairing records without authorization but still allow automatic runtime `hello`. Add handler tests for both cases and update the pairing guide.

### F3 - High: Flipper retains fatal protected-record failures as a live keyed session

**Evidence.** [docs/PROTOCOL.md](docs/PROTOCOL.md) requires decrypt, session-ID, replay, and sequence failures to close the link. The protected branch in [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c) logs and returns on these failures, using an explicit drop-only policy. [esp32/main/main.c](esp32/main/main.c) closes for the corresponding failures.

**Impact.** The peers violate the same contract differently. Corrupt or replayed traffic leaves the Flipper key resident and the session live until ESP32 idle timeout, extending a denial of service condition and making recovery nondeterministic.

**Fix and acceptance.** Schedule a session reset and safe BLE disconnect from the Flipper event handler context, zeroizing key and counters. Test tampered tag, incorrect session ID, and wrong sequence for one teardown each; normal protected traffic must still pass.

### F4 - Medium: Wardriving `start` acknowledgement clobbers active backlog drain continuation

**Evidence.** `wardriving_maybe_kick_send()` blocks a second drain using `wardriving_tx_in_flight`, but the `start` handler in [esp32/main/main.c](esp32/main/main.c) sends `started` unconditionally. `queue_and_send_protected()` holds only one `tx_done_action`, so the acknowledgement's `TX_DONE_NONE` overwrites the drain's `TX_DONE_CONTINUE_WARDRIVING` action.

**Impact.** The current batch completes and pending records remain safe in flash, but no next batch starts until reconnect. This is already a documented known-open issue.

**Fix and acceptance.** Defer the acknowledgement until the current drain finishes, or implement a bounded outbound-operation queue. Test a drain plus injected `start` before write completion; every batch and `started` must emit in order.

### F5 - Medium: Flipper replacement storage deletes the last valid state before rename

**Evidence.** `pairing_storage_save()` and `capability_storage_save()` in [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c) sync a temporary file, then remove the final file before renaming the temporary file.

**Impact.** Power loss, storage failure, or termination between operations loses both old and new state. A known pairing becomes absent and recovery pairing is triggered.

**Fix and acceptance.** Use recoverable two-generation storage: write/sync a versioned candidate, retain the prior committed record, and load the newest valid generation. Fault injection at every filesystem operation must retain either old or new valid state; temporary files cannot count as saved pairings.

### F6 - Medium: Flipper CBOR version values narrow from uint64 to uint32

**Evidence.** [flipper/cbor_records.c](flipper/cbor_records.c) and [flipper/pairing.c](flipper/pairing.c) assign decoded `uint64_t` versions to 32-bit fields. [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c) then accepts narrowed `2`.

**Impact.** Canonical value `4294967298` narrows to `2`, bypassing exact version validation.

**Fix and acceptance.** Reject `version > UINT32_MAX` before assignment, then enforce `version == 2`. Add shared vectors for `UINT32_MAX + 2` and another overwide integer.

### F7 - Medium: Flipper scan UI silently drops result and completion events

**Evidence.** The FAP allocates an eight-entry `AppEvent` queue in [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c). Wi-Fi/BLE status handlers enqueue every result plus completion with zero timeout, ignoring the queue-put result. One status record can contain 32 results.

**Impact.** Most results and the terminal event can be lost, leaving a completed scan displayed as in progress.

**Fix and acceptance.** Post one bounded batch event, or retain results behind synchronization and post one wake event. Test a 32-result status while queue consumption is paused, then prove a deterministic result set and correct terminal UI state.

### F8 - Medium: ESP32 MTU-23 fallback cannot send every legal protocol record

**Evidence.** [esp32/main/main.c](esp32/main/main.c) continues after MTU exchange failure using default ATT MTU, but transmit metadata only supports 13 fragments. At ATT MTU 23, payloads are 16 bytes and a legal 768-byte record needs 48 fragments.

**Impact.** Fallback peers cannot receive valid maximum-size records despite the advertised fallback path.

**Fix and acceptance.** Size metadata for `FEB_MAX_FRAGMENTS`, or explicitly require a larger MTU before traffic. Force MTU 23 in a queue test and send/reassemble a near-768-byte record.

### F9 - Medium: ESP32 absolute millisecond deadlines fail across 32-bit rollover

**Evidence.** `pairing_window_is_open()` and the hello-ack deadline in [esp32/main/main.c](esp32/main/main.c) compare `now_ms >= deadline`, where `now_ms` is a 32-bit millisecond conversion of `esp_timer_get_time()`. Idle timeout correctly uses unsigned elapsed subtraction.

**Impact.** Near 49.7 days uptime, a newly set deadline crossing rollover can expire immediately.

**Fix and acceptance.** Compare unsigned elapsed duration or retain 64-bit milliseconds. Cover pairing and hello-ack expiry with a synthetic near-wrap clock.

## Contract and hardening gaps

- **Unsupported version handling:** Both firmwares drop/reject bad versions instead of sending the mandated unencrypted `unsupported_version` error then closing. Classify version before generic malformed/auth errors and add pairing/session vectors on both targets.
- **ESP32 pairing persistence:** `persist_pairing_secret()` in [esp32/main/main.c](esp32/main/main.c) is one raw NVS blob, without the documented version, validity marker, or replacement procedure. Design it together with F5 as cross-device storage hardening; NVS journaling alone does not meet the written format requirement.
- **State-machine coverage:** Host tests cover codec/crypto vectors but do not compile application event handlers. They cannot exercise authorization, GATT errors, session teardown, queue pressure, storage interruption, MTU fallback, or continuation ordering.

## Optimization opportunities

- Use negotiated characteristic capacity rather than permanent MTU-23 fragmentation after session setup to reduce notifications.
- Replace ESP32 reconnect `xTaskCreate()`-then-sleep with a reusable NimBLE callout, removing repeated 3 KB task allocations during outages.
- Add Flipper FAP stack-usage budgets; several stack failures escaped host tests and appeared only on hardware.
- Precompute ESP32 CBOR literal-key lengths instead of calling `strlen()` in matching loops.
- Batch Flipper application events before considering a broader ViewDispatcher/scene-manager migration.

## Documentation findings

- [README.md](README.md) still says AES-128 although the protocol and firmware use AES-256-GCM.
- [esp32/README.md](esp32/README.md) and [flipper/README.md](flipper/README.md) describe obsolete skeleton behavior, not current pairing/session/capability/wardriving behavior.
- [docs/BASELINES.md](docs/BASELINES.md) contains an unlabeled historical future-work statement.
- [docs/PLAN.md](docs/PLAN.md) and [docs/USER_GUIDE.md](docs/USER_GUIDE.md) contain stale wardriving verification wording relative to [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md).
- [.github/agents/esp32-developer.agent.md](.github/agents/esp32-developer.agent.md) names a Heltec board while the active baseline is ESP32-C6-DevKitC-1-N4.

Keep `SESSION_MEMORY.md` current-state-only and move dated narrative to `PROJECT_HISTORY.md`.

## Validation performed

- Reviewed every repository Markdown file, including cached ABI/reference material and agent instructions.
- Ran `tests/esp32/build.ps1`, `build_pairing.ps1`, `build_session.ps1`, and `build_wardriving.ps1`; all passed.
- Ran `tests/flipper/build.ps1`, `build_pairing.ps1`, and `build_session.ps1`; all passed. The general Flipper build includes wardriving and reported `479/479` passing checks.
- `tests/flipper/build_wardriving.ps1` does not exist; the attempted invocation showed it is not an entry point. Align `tests/README.md` with actual scripts or add a wrapper.
- ESP-IDF and FBT builds were not rerun; use pinned commands in [CLAUDE.md](CLAUDE.md) after remediation. No physical hardware was modified.

## Suggested implementation sequence

1. Fix F1 with shared boundary vectors, then run both session suites.
2. Fix F2/F3 in Flipper application lifecycle with focused handler tests and FAP build.
3. Fix F4 by formalizing ESP32 outbound serialization and testing the backlog/start race.
4. Design F5 and ESP32 NVS hardening as one recoverable-storage decision before implementation.
5. Resolve F6-F9 and unsupported-version behavior with mirrored vectors.
6. Run both firmware builds and step-9 hardware checks: forced disconnect during wardriving, multi-minute 20% BLE-duty validation, flash wrap/power-loss, and SD-card CSV verification.

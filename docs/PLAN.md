# Implementation Plan

This plan implements the trusted-environment BLE pairing decision in [DECISIONS.md](DECISIONS.md) and protocol v2 in [PROTOCOL.md](PROTOCOL.md). The target board is the ESP32-C6 DevKitC-1-N4.

## Roadmap phases

- **Phase 1 (✅ done):** board/SDK/firmware/build baselines — see `docs/BASELINES.md`.
- **Phase 2 (✅ done):** core BLE transport, record framing, trusted-environment pairing, and authenticated runtime sessions on the ESP32-C6 — steps 1-7 implemented and hardware-verified.
- **Phase 3a (✅ done, hardware-verified 2026-09-13):** Flipper UI menu redesign — Home/menu shell with capability-aware routing, reconnect-stays-put behavior, GPS/Settings/About screens all working end-to-end. (Note: ViewDispatcher/scene-manager architecture prerequisite was skipped; built directly on existing ViewPort/AppEvent-queue pattern and works reliably. Five-mode BLE-active/passive Scan screen remains backlogged, pending runtime toggle.)
- **Phase 3 (✅ done, hardware-verified 2026-09-13):** Production-ready wardriving on the ESP32-C6. Includes `wifi_scan`, `ble_scan`, `wardriving` with real GPS driver, hardened flash log, per-record timestamps, WiGLE CSV export, LED indicators, and BLE active scanning. Field-usable unattended for hours, survives power loss, backlog drains reliably.
- **Phase 4 (in progress, started 2026-09-16):** Heltec WiFi LoRa 32 V2 board support — a second target (classic ESP32/Xtensa) adding display and LoRa. **Gate overridden by explicit user decision 2026-09-16** — Phase 3's backlog is not cleared and stays fully deferred, not interleaved with Phase 4 work; see the Phase 4 section below for the override rationale and current step status.
- **Phase 5 (scheduled later):** Zigbee/Thread and `gpio_control` — later-phase capabilities pending Phase 3/4 completion.
- **Phase 6 (design frozen 2026-09-17, not yet started):** wardriving log publishing to wdgwars.pl via a Flipper-triggered BadUSB/host-script flow. **Explicit user decision 2026-09-17: proceeds in parallel with Phase 4**, not gated on Phase 4 or Phase 5 completion — same kind of gate override Phase 4 itself carries for the Phase 3 backlog. Full design: [docs/WARDRIVING_PUBLISH.md](WARDRIVING_PUBLISH.md).
- **Phase 7 (implemented and hardware-verified 2026-09-21):** Wardriving screen redesign — Stopped/Running screen split, persisted per-run settings (mode, WiFi scan-dwell "swelling", WiFi cooldown, BLE active/passive, WiFi country code), and GPS speed display/input. **Proceeded in parallel with Phase 4/6**, same gate-override pattern. Full design: [docs/WARDRIVING_REDESIGN.md](WARDRIVING_REDESIGN.md).

For the full dated narrative of how each phase/step was designed, implemented, and debugged, see [docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md). For current state, see [docs/SESSION_MEMORY.md](SESSION_MEMORY.md); for the open backlog, see [docs/BACKLOG.md](BACKLOG.md).

## Confirmed setup choices

- Target board: ESP32-C6-DevKitC-1-N4, connected by USB for flash-size verification.
- Flipper build: standalone external FAP against Unleashed stable `unlshd-092` (API 88.4), pinned to commit `3c9be0fdd9d301a9436765099a2d1780b36a1795`.
- FAP compatibility: target the pinned Unleashed stable API only. A future firmware-version adapter is deferred to phase 2.
- ESP-IDF: use ESP-IDF `v5.5.2` for the initial C6 baseline; record the installed toolchain revision when setup completes.
- Initial phase 1 scope: ESP-IDF firmware skeleton and standalone FAP skeleton, both built before adding project code.

## 1. Establish the build baselines

- Select and record one ESP-IDF release that supports `esp32c6`, NimBLE central mode, X25519, HKDF-SHA-256, HMAC-SHA-256, AES-256-GCM, and encrypted NVS.
- Create the ESP-IDF project under `esp32/` with an `esp32c6` target and a partition table suitable for the board's verified flash size.
- Create the standalone FAP project, delivered as a standalone external FAP, and pin it to the locally cached Flipper firmware API revision. It owns a custom BLE GATT profile while active; custom/in-tree firmware is optional hardening for stronger key storage or Bluetooth coexistence.
- Build an unmodified baseline for both targets before adding project code.

**Done when:** both baseline images build reproducibly and the C6 flash size is measured on the target board. ✅ Met 2026-09-01/02 — see `docs/PROJECT_HISTORY.md`.

## 2. Prove the BLE transport

### Confirmed transport configuration

- Real hardware validation will use both the ESP32-C6 and a Flipper Zero.
- If no ESP32 pairing record exists, the Flipper app presents an explicit pair/connect action and starts its temporary BLE profile for that workflow.
- If a pairing record exists, the Flipper app attempts to connect automatically to the saved ESP32.
- The ESP32-C6 scans and attempts connection automatically at boot.
- The first transport smoke test uses a fixed payload. The implementation will then advance to the protocol CBOR envelope.
- Enforce one active connection and use bounded exponential reconnect backoff with a maximum of five automatic retries (superseded for production behavior — see "Revised long-run reconnect policy" below).

**Done when:** the C6 discovers the service, connects, writes a test record, receives a notification, and recovers from a disconnect on real hardware. ✅ Met on 2026-09-02 — see `docs/PROJECT_HISTORY.md`.

**Future enhancement:** add an adapter layer for later Unleashed API revisions after the pinned stable baseline is working.

### Revised long-run reconnect policy (supersedes "five retries" above for production behavior)

The original five-retry ceiling was designed for recovering from a transient disconnect during active use, not for a board that may go unattended for hours or days (the wardriving use case). Production reconnect behavior is:

- Bounded exponential backoff for the first several attempts, same as above, for fast recovery from a transient disconnect.
- After reaching a backoff ceiling, do not give up — continue retrying indefinitely at a slow, fixed cadence (on the order of tens of seconds) so a board left running for a long unattended stretch is still connectable whenever a Flipper eventually comes into range, without requiring a reboot.
- When BLE-source wardriving scanning is active, do not run a separate dedicated reconnect scan — reuse the same scan pass (**active**, not passive — see 2026-09-11 correction below), filtering for the Flipper's fixed v2 service UUID, and trigger a connection attempt on a match. Fall back to a dedicated reconnect scan using the policy above only when BLE-source wardriving is not running.

This policy is implemented on the ESP32 as two independent two-phase (exponential-then-flatten) backoff paths — one for GAP-level connect failures (`MAX_RECONNECT_RETRIES`, flattening to a 30-second cadence), one for runtime-auth proof failures (a separate, longer 5-minute flattened cadence, since that path also throttles repeated bad credentials) — and is **hardware-verified** on both paths (see `docs/PROJECT_HISTORY.md`'s step-6 stability-fixes entry for the connect-failure path; runtime-auth failures were exercised as part of step 6's own hardware verification). The merged-reconnect-scan behavior (bullet 3 above) was implemented as a **passive** scan pass and code-reviewed but not yet exercised under a live forced disconnect at the time step 4 closed. **2026-09-11**: a live forced disconnect during wardriving's BLE capture found it never reconnects under a passive-only pass; wardriving's BLE re-arm (`wardriving_ble_interval_cb()` and its `start` counterpart in `main.c`) now scans active, matching `start_scan()`'s already-working reconnect scan — see `docs/LESSONS.md`'s "wardriving-passive-scan-reconnect-stall" entry and `docs/PROJECT_HISTORY.md`. Build-verified; hardware re-verification of the fix itself is still pending (see `docs/SESSION_MEMORY.md`).

## 3. Define and implement record framing

- Fragment header format, size limits, and rejection rules are defined in [PROTOCOL.md](PROTOCOL.md#fragmentation) — implement exactly as specified there rather than re-deriving the format here.
- Derive fragment payload capacity from negotiated ATT MTU minus the GATT and fragment-header overhead.
- Reject duplicate, inconsistent, oversized, incomplete, and out-of-order fragments without allocating from peer-controlled lengths.
- Implement canonical CBOR envelope encoding and bounded decoding with limits on nesting, map entries, arrays, text, and byte strings, using the fixed-field-order definition of "canonical" in [PROTOCOL.md](PROTOCOL.md#canonical-cbor-encoding-definition).
- No third-party CBOR library — a hand-rolled, schema-specific canonical codec on both firmwares, since the message shapes are fixed and small and a general-purpose library would still need custom validation layered on top.
- Malformed fragment/message handling (a case PROTOCOL.md leaves connection-level-silent on): drop the reassembly buffer for that `message_id` and keep the connection open — do not close the connection or reply. A persistent flood is expected to be caught by the idle-connection timeout and session/auth layer, not the fragment layer.

**Done when:** two independent codec tests exchange records at ATT MTU 23 and 247, including fragmented records and malformed-input rejection. ✅ Met on 2026-09-02 (host-native tests, both target builds); on-device smoke test over real hardware also passed 2026-09-03, after finding and fixing a real stack-overflow bug in the shared `framing.c` (an internal fragment buffer was stack-local, overflowing the Flipper's 1280-byte `BleEventWorker` thread). Step 3 is fully closed. Full narrative (both host-test results, the stack-overflow root cause and fix, and the on-device exchange) is in `docs/PROJECT_HISTORY.md`.

## 4. Validate BLE / Wi-Fi radio coexistence

The ESP32-C6 has a single 2.4GHz radio shared between Wi-Fi, BLE, and (later) 802.15.4. The `wifi_scan`/`ble_scan`/`wardriving` capabilities depend on this radio being usable concurrently for scanning and for maintaining the BLE connection to the Flipper, so this needed validating before those capabilities were built on unverified assumptions — pulled ahead of pairing/session work rather than left to the final validation pass (step 9).

**802.15.4 is out of scope for this step.** No 802.15.4 code or radio activity exists on either firmware (Zigbee/Thread recon is a later-phase item) and this step's tests only exercise Wi-Fi scanning + BLE. 802.15.4 coexistence gets its own validation pass once the Zigbee/Thread recon phase actually adds 802.15.4 radio activity to test against.

- Using the step 2/3 transport (no pairing/session/capability layers needed), run Wi-Fi scanning, an active BLE connection to the Flipper, and a passive BLE observer scan together for an extended period (30+ minutes).
- Test both configurations: BLE-source scanning **paused** during an active connection, and BLE-source scanning **concurrent** with an active connection. Only build an automatic pause-on-connection-degradation fallback if the concurrent configuration proves unstable; do not build it preemptively.
- Validate the merged reconnect-scan behavior from step 2: while BLE-source scanning is active, confirm it correctly detects and connects to the Flipper's advertised v2 service UUID within the same scan pass, without a separate dedicated reconnect scan running at the same time.
- Record safe minimum/maximum bounds and sensible default values for Wi-Fi and BLE scan intervals from these results — the `wardriving` capability uses these as its interval defaults/bounds rather than guessed values.

**Done when:** Wi-Fi scanning, an active BLE connection, and a BLE observer scan run together without the connection dropping outside the reconnect policy's expected behavior, for both the paused and concurrent configurations, with results and chosen interval bounds recorded here. ✅ Met on 2026-09-03 — see "Step 4 results" below. Full design-decision rationale and orchestration incidents are in `docs/PROJECT_HISTORY.md`.

### Step 4 results (2026-09-03) — interval bounds other capabilities depend on

An overnight sweep on a throwaway harness (`esp32/coex_test/`) validated 5 points; all passed
cleanly with zero disconnects/degradations up to the theoretical maximum duty cycle. Full sweep
table and narrative: `docs/PROJECT_HISTORY.md`'s "Step 4 (radio coexistence) validated on real
hardware" entry. The bounds below are load-bearing (used as `wardriving`/`ble_scan` defaults),
not just historical record:

- **Minimum (most conservative) BLE observer duty:** window=100ms/interval=1000ms, Wi-Fi rescan
  every 30s — proven stable, lowest radio-time cost.
- **Maximum (most aggressive) BLE observer duty:** window=30ms/interval=30ms (NimBLE's own
  default fast-scan params, already relied on by `esp32/main/main.c`'s reconnect scan),
  continuous Wi-Fi scanning — proven stable at 100% duty.
- **Default:** anywhere in the 50-90% duty range tested equally clean; pick based on the
  capability's actual power/latency priorities — a product choice, not a stability constraint.

**Accepted gap:** the merged reconnect-scan behavior was never exercised (zero disconnects
occurred in the sweep) — tracked in the Backlog for step 9's full-system validation. **Still
open as of 2026-09-11**: a live forced disconnect during wardriving's BLE capture exercised it
for the first time and found it never reconnects. The first candidate fix (switching
wardriving's BLE re-arm from passive to active scanning) was flashed and retested live and did
**not** resolve it — zero reconnects over 130+ discovery restarts. See `docs/LESSONS.md`'s
"wardriving-passive-scan-reconnect-stall" entry for the current investigation state and leading
(unconfirmed) suspect.

## 5. Implement trusted-environment pairing

- On ESP reset, generate a fresh 16-byte `pairing_epoch` and open exactly one 120-second pairing window.
- Generate a fresh X25519 ephemeral keypair and 16-byte `device_nonce` for each pairing attempt.
- Implement `pair_init`, `pair_reply`, `pair_confirm`, and `pair_complete` exactly as defined in [PROTOCOL.md](PROTOCOL.md).
- Generate the Flipper X25519 keypair and 16-byte `client_nonce` only after the pairing UI is active.
- Derive `K_shared`, `K_confirm`, and the 32-byte `pairing_secret` with the documented HKDF inputs and transcript.
- Reject an all-zero X25519 shared secret. Compare confirmation tags in constant time. Zeroize all ephemeral secrets on success, failure, expiry, and disconnect.
- Persist `pairing_secret` transactionally on the ESP32 before `pair_complete`; persist it on Flipper only after completion verification.
- Close pairing immediately after success. Return `pairing_disabled` after the window closes or succeeds; a later ESP reset permits replacement pairing **only while no working stored secret exists** — see step 6, which revises this once a secret is stored.
- The Flipper stores multiple pairing records, keyed by `board_id`, rather than a single record — supporting more than one paired board at a time (see step 7). Each board's record lives in its own file (`pairings/<board_id>.dat`), so replacing one board's record cannot touch another's, and `board_id` is validated against a strict `[A-Za-z0-9_-]` charset before it is ever used to build a filesystem path (it arrives before any cryptographic confirmation, so treating it as trusted input early would be a path-traversal-shaped gap).
- `board_id` is recomputed every ESP32 boot from the chip's factory base MAC (`esp32c6-<12 lowercase hex chars>`) — no NVS write needed for it.
- Zeroization on both sides goes through a named helper (`feb_secure_zero()`), not ad-hoc `memset`, since an optimizing compiler can legally elide a `memset` on a variable it proves is dead (CWE-14).

**Done when:** a first pairing survives reboot, a reset-and-repair replaces the old relationship for that board only (other stored pairings are unaffected), and passive capture does not expose the persisted pairing secret. ✅ Met on 2026-09-05 — see `docs/PROJECT_HISTORY.md` for the full design session (including a real feasibility blocker: a standalone FAP cannot link the firmware's own X25519/HKDF/HMAC, resolved by porting `curve25519-donna` and hand-rolling the hash/MAC/KDF primitives into the Flipper), the implementation, two real hardware bugs found and fixed (an ESP32 write-fragment/GATT-characteristic-size mismatch, and a Flipper-side X25519 stack overflow), and the hardware-verification pass.

Two findings from the hardware-verification pass were backlogged rather than fixed in this step (both since resolved during step 6 — see `docs/PROJECT_HISTORY.md`): pairing files were saved under the wrong app's data directory, and the custom BLE profile could stay connectable after a successful pairing, letting a subsequent ESP32 reset silently complete an unwanted second ceremony.

## 6. Add authenticated runtime sessions

- Implement `hello`, `hello_ack`, and `client_auth` using the stored `pairing_secret`.
- The Flipper looks up which stored pairing record to use by the `board_id` carried in the incoming `hello` record.
- Derive a fresh 32-byte **AES-256-GCM** key for every connection using HKDF-SHA-256, `client_nonce`, `device_nonce`, `board_id`, and `session_id`. (Originally specified as a 16-byte AES-128-GCM key; revised to AES-256-GCM during this step's design — the Flipper's only exported raw-key AES-GCM hardware primitive is hardcoded to a 256-bit key, with no software fallback available. See `docs/DECISIONS.md` and `docs/PROJECT_HISTORY.md`.)
- Generate a new ESP32 `session_id` for every authenticated session.
- Enforce per-direction sequence counters, the specified 12-byte nonce construction, canonical-CBOR AAD, and a 16-byte GCM tag.
- Disconnect on authentication failure, replay, unexpected sequence, malformed CBOR, or counter exhaustion. Do not resume counters after reconnect.
- **Reset-vs-runtime-auth boot decision:** on boot, if a `pairing_secret` is already stored, the ESP32 attempts runtime auth first; a pairing window opens only if no secret exists yet, or that attempt fails with `unknown_board` (a new `error.code` value meaning "the Flipper has no stored record for this `board_id`"). A proof-verification failure (as opposed to "never paired") does **not** auto-open a window — it's logged and rate-limited only, so a radio attacker can't force repeated pairing-window openings by corrupting proofs in transit. This revises step 5's "physical reset alone authorizes re-pairing" framing — see `docs/DECISIONS.md`/`docs/PAIRING.md`, which carry the current wording.
- **Explicit re-pair while a valid secret exists on both sides** is done via the Flipper's local "unpair this board" action (step 8 scope) alone — no ESP32-side action is needed, since its next connection attempt gets `unknown_board` from the Flipper.
- **Session teardown/reconnect:** the existing 30-second idle-timeout / no-counter-resumption / disconnect-on-any-auth-failure policy in [PROTOCOL.md](PROTOCOL.md) is sufficient as specified — no additional rules were needed.

**Done when:** both sides pass shared known-answer vectors and reject modified ciphertext, modified AAD, replayed records, and sequence gaps. ✅ Met, and hardware-verified, on 2026-09-06 — see `docs/PROJECT_HISTORY.md` for the full design session (including the AES-256-GCM revision), implementation, and hardware-verification narrative, plus the pairing-file path-resolution bugfix carried over from step 5.

**Judgment calls made during implementation, not blocking:** ESP32 rate-limits repeated proof failures (1/2/4/8/16/32s backoff then a 5-minute fixed cadence, reset on any success) and enforces a 5-second `hello_ack` timeout — neither is numerically specified in the docs. The Flipper routes an incoming record to the pairing- or session-envelope decoder by peeking the CBOR map's field count (the three envelope shapes are distinct sizes). A malformed `hello`/`client_auth` payload is silently dropped with no reply, matching the treatment of a proof-verification failure (arguably a security positive: it keeps the two failure modes indistinguishable to an attacker).

## 7. Implement board identity and capability registry

- Generate and persist an immutable printable ASCII `board_id` during controlled provisioning or first boot before pairing. Derive it from the chip's factory-programmed MAC address to avoid collisions across multiple boards without a separate provisioning step.
- Implement `capability_query` and `capability_response` after runtime authentication only.
- Add commands incrementally, with input validation, request IDs, bounded output, and explicit `unsupported_capability` errors.
- **Scope split:** this step covers only the board-identity/capability-registry plumbing (`capability_query`/`capability_response` reporting `board`/`firmware`/`features`). The `command` message type and any real capability command handler (starting with `wifi_scan`) are a separate follow-on step — see below.
- **`capability_query` lifecycle:** sent exactly once per `board_id`, on the first successful runtime auth when no locally persisted capability file exists yet. The result is cached and never automatically re-queried — a deliberate exception to the project's usual re-verify-every-session pattern, since a capability list is non-sensitive cached metadata, not a security credential. The only refresh path is a full unpair + re-pair. See [CAPABILITIES.md](CAPABILITIES.md) for the full storage/lifecycle policy and the registry format.
- **Storage:** each board's capability record lives in its own file, separate from its pairing-secret file, so the pairing file can get step 8's atomic-write/versioning hardening without forcing the same constraints onto the non-sensitive capability cache. Unpairing a board deletes both files together.
- `board`/`firmware` are hand-maintained constant strings (not build-injected); `features` is a hardcoded compile-time list — see [CAPABILITIES.md](CAPABILITIES.md).

### Capability roadmap

Capabilities ship incrementally, gated on hardware actually present on a given board — see [CAPABILITIES.md](CAPABILITIES.md) for the registry format and the full, current capability list. See "Roadmap phases" above for how these map onto Phase 3/4/5.

1. **`wifi_scan`** (Phase 3) — first capability, needs no extra hardware. ✅ Implemented and hardware-verified 2026-09-07.
2. **`ble_scan` + `wardriving`** (Phase 3): add `ble_scan` and the composite `wardriving` capability. **Reordered 2026-09-07 to no longer wait on GPS hardware** — see "`ble_scan`, `wardriving`, and the GPS-stub reorder" below for why and how. **`ble_scan` implemented and hardware-verified 2026-09-08** (manual on-device scan trigger via Right button on the main screen, results in a scrollable view, capped at 32 devices by RSSI); **`wardriving` implemented on both firmwares and build/host-test-verified 2026-09-09, hardware verification pending** — ESP32 side: autonomous capture engine, power-loss-safe on-device flash log. Flipper side: a one-tap start/stop control/status screen (reachable via Up from the main screen), status/backlog-drain dispatch (including the `request_id = 0` unsolicited-backlog-drain case), and incremental WiGLE CSV export to SD card (one timestamped file per connected session, written record-by-record, never buffered in RAM). See [CAPABILITIES.md](CAPABILITIES.md) for the full design and this step's own "Done when" note above for implementation detail.
3. **Heltec board support** (Phase 4, separate baseline, starts after Phase 3 completes): display and LoRa capabilities on a second, structurally different board — see `docs/BASELINES.md`.
4. **Zigbee/Thread recon** (Phase 5a): passive `zigbee`/`thread` scanning/sniffing capabilities, matching the `wifi_scan`/`ble_scan` pattern — no network joining or commissioning.
5. **Zigbee/Thread participation** (Phase 5b, much later, separately scoped): active stack participation — an order of magnitude larger effort; not committed to a timeline.
6. **`gpio_control`** (Phase 5): generic GPIO control, reserving strapping/JTAG pins (GPIO0, 4, 5, 8, 9, 15) from generic control actions.

### Multi-board pairing

- Multiple boards (e.g. a C6 and a Heltec) may be paired to one Flipper at a time, each with its own stored pairing record (step 5).
- The Flipper is the BLE peripheral, so only one BLE connection is active at a time: whichever paired board connects first occupies the slot. Switching boards today means powering down the currently-connected one so another can connect.
- **Backlog item:** a manual "disconnect current board" Flipper UI action to free the connection slot without physically powering off a board. Automatic arbitration is gated on an unresolved BLE-HAL feasibility question — whether the Flipper's peripheral role can advertise while already serving a connection.

**Done when:** Flipper renders only the authenticated capability list reported by the paired board, correctly reflecting that board's hardware. ✅ Met, and hardware-verified, on 2026-09-07 — see `docs/PROJECT_HISTORY.md` for the design decisions and implementation narrative (including a same-bug-class `BleEventWorker` stack regression found and fixed during implementation).

## 8. Harden persistent state and release configuration

- Store the C6 pairing record in a dedicated encrypted NVS namespace with a version, validity marker, and atomic replacement procedure. Cross-referenced from `docs/CODE_REVIEW_FINDINGS.md` finding #17: today's ESP32 storage (`persist_pairing_secret`/`load_pairing_secret`) is a bare `nvs_set_blob()` with none of version/validity-marker/atomicity — a real, currently-uncosted gap against the written contract that this step owns closing.
- Implement explicit local unpair/factory-reset behavior and define which pairing record is removed on each side (unpairing one board must not disturb other stored pairing records — see step 7).
- Store the Flipper pairing record through an atomic app-owned storage update (temporary file, exact write verification, `storage_file_sync()`, close, rename) and do not log it. Treat local SD-card, debug, and modified-firmware access as outside the standalone FAP protection boundary (see [PROTOCOL.md](PROTOCOL.md) "Implementation security requirements" — this is an accepted limitation, not a gap to close in this phase).
- The wardriving buffer uses the same atomic-persistence philosophy: a hand-rolled, checksummed, append-only log on raw flash (not a FAT-based wear-levelling filesystem), so an unclean power loss (e.g. car ignition cut) loses at most the single record being written at that instant, never the rest of the log. Circular — when full, evicts the oldest **erase-sector's worth** of records at once (raw NOR flash only erases a whole sector at a time; true single-record eviction would need a wear-levelling translation layer, which this bullet's own "not a FAT-based wear-levelling filesystem" already rules out) — not literally the single oldest record. See [PROTOCOL.md](PROTOCOL.md)'s "Flash log eviction" note.
- **Scope note (added 2026-09-07):** the wardriving-log half of this step is being built now, ahead of the rest of Phase 3, as part of "`ble_scan`, `wardriving`, and the GPS-stub reorder" above — not deferred to a later pass through step 8. The pairing-record/capability-file persistence hardening (the first two bullets above) remains deferred; this step isn't "done" until those land too.

**Status:**
- **Wardriving-log persistence:** ✅ done (checksummed circular flash log, `esp32/main/wardriving_log.c`), hardware-verified 2026-09-13 (extended wraparound/power-loss runs, stale record cleanup on boot/replay).
- **Pairing-record/capability-file hardening:** not yet started — future work after Phase 3 completion.

**Done when (future):** interrupted writes, reboot during pairing, unpair, and factory reset leave no ambiguous paired state for pairing records and capability cache files (matching the robustness already achieved for wardriving log).

**See also:** "Deferred: hardware hardening" below — Secure Boot, flash encryption, and related eFuse-dependent work are explicitly out of scope for this phase and are not part of this step's "done when" bar.

## 9. Validate the complete system

System-level validation covering codec/cryptography, error handling, reconnection, and production workloads.

**Completed sub-items:**
- ✅ **Codec/crypto test vectors:** all host-native test suites pass on both targets (481 Flipper checks, 200+ ESP32 checks covering HMAC, AES-GCM, X25519, CBOR, framing).
- ✅ **Error handling:** fragment/CBOR malformed-input rejection tested; session auth failure/replay/sequence-gap tested.
- ✅ **Reconnection flows:** reset-and-repair (2026-09-06), forced-disconnect-during-wardriving (2026-09-11), idle-timeout reconnect (ongoing), all tested on hardware.
- ✅ **Radio-coexistence under real load:** 20% BLE duty cycle (`ble_window_ms=100`, `ble_interval_ms=500`), continuous Wi-Fi scanning, live wardriving streaming tested for hours without idle-timeout spurious disconnect.
- ✅ **Merged-reconnect-scan mechanism:** BLE-only isolation (7/7 reconnects), BLE+Wi-Fi coexistence (reconnect stalls when Wi-Fi source runs concurrently — accepted as Wi-Fi duty-cycle tuning issue, not a protocol bug).
- ✅ **Power loss resilience:** wardriving log circular wraparound tested; old format records gracefully skipped on boot.
- ✅ **GPS cold-start-to-fix cycle:** real NMEA module hardware-verified; fix-dependent record discard/resume working.
- ✅ **WiGLE CSV export:** real SD card, proper timestamping, dedup validation.

**Remaining (future phase work):**
- Full negative-security-test suite (ciphertext tampering, replay, bad confirmation flows).
- Fuzz testing with truncated/oversized/invalid CBOR/framing inputs.
- Multi-day unattended operation at production duty cycles.

**Done when:** ✅ Phase 3 acceptance bar met 2026-09-13 — full pairing-to-wardriving-export flow verified end-to-end on real hardware with production workloads.

## Deferred: hardware hardening (explicitly out of scope for this phase)

Physical possession of either paired device (Flipper or ESP32) is accepted as fully compromising to that device's stored secrets and data for the current phase — see [PROTOCOL.md](PROTOCOL.md) "Implementation security requirements." Treat a lost or stolen paired device as game over for that pairing relationship: reset (ESP32) or unpair (Flipper) immediately. Other stored pairings are unaffected.

Because of that accepted threat model, none of the following are required for any "done when" bar in this plan, and none of them are scheduled:

- Secure Boot, flash encryption, and NVS encryption on the ESP32.
- Signed firmware updates and production debug/download restrictions.
- Any irreversible eFuse configuration.

**No irreversible hardware operations are to be performed on any board in this phase.** If this work is ever picked up, it requires a dedicated sacrificial board for eFuse validation before any irreversible setting is applied to a board actually in use — do not attempt it on the primary development board.

## Wi-Fi scan capability (Phase 3 follow-on step)

The first real use of the generic `command`/`status` message types (defined in [PROTOCOL.md](PROTOCOL.md) but previously unimplemented) — this is the "follow-on `wifi_scan`-command step" named in Phase 3's roadmap description above, not step 8 and not part of step 7 (which explicitly deferred all `command`/`status` handling here).

**Wire format and behavior**: frozen in [PROTOCOL.md](PROTOCOL.md)'s "`wifi_scan` command and status payloads" section and [CAPABILITIES.md](CAPABILITIES.md) — read those directly rather than this plan; nothing here duplicates that spec.

**Done when** (wire-format/build bar): ✅ Met 2026-09-07, both firmwares build- and host-test-verified against the frozen contract and shared vectors.

**Hardware verification:** ✅ Complete 2026-09-07 — full narrative (two real bugs found and fixed, a stack-usage measurement) in `docs/PROJECT_HISTORY.md`'s "wifi_scan capability implemented and hardware-verified" entry.

## `ble_scan`, `wardriving`, and the GPS-stub reorder (Phase 3, decided 2026-09-07)

The original roadmap gated `ble_scan`/`wardriving` on a GY-NEO6MV2/NEO-6M GPS module being
physically wired to the C6 first. The user decided to unblock this work now instead: implement
both capabilities using a **fixed-coordinate GPS stub** behind a clean location-source interface
(`location_get_fix()`), so wiring up real GPS later is a small, localized swap — not a rewrite
— rather than continuing to wait on hardware bring-up. This also pulls forward the
wardriving-log half of step 8 below (the hardened flash-backed persistence), built for real now
scoped to just the wardriving log; the pairing-record/capability-file persistence hardening
that's the other half of step 8 stays deferred (see step 8's note).

Full design (wire protocol, ESP32 engine including the productionized merged-reconnect-scan
mechanism, the raw-flash circular log, and the Flipper UI/WiGLE export) is captured in
[PROTOCOL.md](PROTOCOL.md)'s `ble_scan`/`wardriving` sections and [CAPABILITIES.md](CAPABILITIES.md).
Key decisions from that design pass, made explicitly with the user:

- **`ble_scan` ships as its own standalone manually-triggered capability** (mirrors `wifi_scan`'s
  "Scan now" pattern), in addition to being used internally by `wardriving`'s capture engine.
- **Flash-log eviction is batch-by-sector**, not strict single-record drop-oldest — raw NOR
  flash's erase-block constraint means true single-record eviction would need a wear-levelling
  translation layer this project deliberately avoids (see step 8's "not a FAT-based
  wear-levelling filesystem" framing, and [PROTOCOL.md](PROTOCOL.md)'s "Flash log eviction"
  note). This corrects step 8's original "drop the oldest record" wording below.
- **Wardriving's default cadence was the most aggressive/thorough validated point from step 4**
  (continuous-ish Wi-Fi scanning, ~90-100% BLE observer duty — also NimBLE's own default
  fast-scan parameters), prioritizing capture thoroughness — through 2026-09-09. **Corrected
  2026-09-10**: real wardriving traffic on real hardware showed 100% BLE duty starves the
  active connection itself (see "Known open items" below and `docs/PROTOCOL.md`'s "Interval
  bounds and defaults"); `ble_interval_ms`'s default is now 500ms. **Corrected again same day**:
  `ble_window_ms`'s default (left at 30ms by the first fix, ~6% duty) was raised to 100ms
  (~20% duty) after a short test run showed a real chance of missing every nearby BLE device's
  advertisement at 6% duty — still far below the 100% duty that caused the starvation. Fully
  configurable per-session via the wire protocol regardless.
- **The Flipper's wardriving control screen ships with fixed defaults only for v1** — one-tap
  start/stop, no source-selection or interval-entry UI. The app has no form/settings-entry
  widget anywhere yet; building one is a separate, larger scope addition than anything else here.
  **Corrected 2026-09-11**: a Left/Right source-selection toggle (Wi-Fi/BLE/both, offered only
  when the board advertises both) was added to unblock the reconnect-stall investigation's
  BLE-only isolation test — see `docs/PROJECT_HISTORY.md`. This is a toggle on the existing
  screen, not the form/settings-entry widget described above; interval-entry is still absent.

**Done when:** matches `wifi_scan`'s bar — both capabilities build- and host-test-verified
against the frozen wire contract with shared vectors, then hardware-verified on real devices,
including a forced-disconnect test of the newly-productionized merged-reconnect-scan mechanism
(closing step 4's long-open "never exercised" gap) and an extended unattended run validating the
flash log's wraparound and power-loss behavior.

**`ble_scan`: done, hardware-verified 2026-09-08 (see above). `wardriving`: implemented on
both firmwares, build/host-test-verified 2026-09-09, hardware-verified 2026-09-10.** Two real
ESP32-side bugs were found and fixed during the first hardware test — see
`docs/PROJECT_HISTORY.md`'s "wardriving hardware-verified" entry for the full narrative
(`nimble_host` stack overflow in `wardriving_send_next_batch()`, and GATT-write-flood +
reconnect-scan-restart collision). Both fixes are hardware re-verified. Three items still
open per the "done when" bar — see `docs/SESSION_MEMORY.md`'s "Known open items" for exactly
what remains: forced-disconnect test under live BLE capture (ran 2026-09-11, found and fixed a
real bug — see the "Accepted gap" note above and `docs/SESSION_MEMORY.md`), extended unattended
flash-log wraparound/power-loss run, and CSV export SD-card confirmation.

## Real GPS driver, wardriving fix-dependency, and real wardriving-record timestamps (Phase 3, decided 2026-09-12)

Reached via a grill-me design session with the user. Replaces `esp32/main/location.c`'s
fixed-coordinate stub with a real UART/NMEA-0183 driver, and closes two backlog items that
depended on it (wardriving's discard-on-no-fix behavior becoming real, and replacing the CSV
`FirstSeen` backdating approximation with a real timestamp). Full wire contract:
[PROTOCOL.md](PROTOCOL.md)'s new "`gps` command and status payloads" section, the new
`utc_timestamp_s` wardriving-record field, and the updated "Location source" note; capability
description: [CAPABILITIES.md](CAPABILITIES.md)'s `gps` and updated `wardriving` entries. Not yet
implemented — this section is the frozen design, not a "done when" bar met.

**Scope boundary (deliberately excluded from this design):** making the GPS UART's GPIO pin
assignment runtime-configurable from a Flipper Settings screen — the user's original ask included
this, but it was split out during grilling because it drags in a prerequisite this project
doesn't have yet (any form/pin-entry widget on the Flipper — see [UI_REDESIGN.md](UI_REDESIGN.md)'s
own note that Settings is still an unscoped placeholder) and a new wire-protocol surface (a
get/set config command, ESP32-side persistence, safe re-init of an already-open UART driver).
Pins stay a compile-time constant (`UART_NUM_1`, RX=GPIO18, TX=GPIO19, 9600 8N1, no flow control —
the values [tools/test_gps_antenna.ps1](../tools/test_gps_antenna.ps1) already hardware-verified)
until that later slice happens. See "Backlog" below for the deferred item.

Key decisions from the design session:

1. **Driver behavior.** `location_init()` opens UART1 and starts a background parse task, RX-only
   (never transmits to the module — an earlier wake/cold-start command burst was proven actively
   harmful in the antenna smoke test, forcing re-acquisition on every reconnect). Tracks three
   states — `no_signal`, `acquiring` (valid NMEA traffic, no valid fix yet), `fix` (both `GGA` fix
   quality > 0 and `RMC` status `A`) — rather than the interface's current binary `has_fix`.
   **`no_signal`'s exact definition (reconciled 2026-09-12 after implementation):** any
   checksum-valid `$`-prefixed NMEA sentence of *any* type (not only `GGA`/`RMC`) flips
   `no_signal` → `acquiring` — chosen over the stricter "only `GGA`/`RMC` count" reading because it
   correctly keeps garbage bytes from a wrong baud rate or bad wiring at `no_signal` rather than
   misreporting `acquiring`. (This section's decision list previously said "never seen a byte,"
   which was the same intent stated less precisely — no behavior change, just a wording fix.)
2. **Sentence types parsed: `GGA` and `RMC` only.** `GGA` gives fix quality/satellite
   count/HDOP/lat-lon; `RMC` gives date+time (used for `utc_timestamp_s`) and, incidentally,
   speed/course (parsed but not yet wired to anything — see "Backlog" below). `ZDA` was
   considered for date+time but rejected: the real captured antenna-test log
   (`tools/gps_antenna_last_run.log`) shows the actual module never emits `ZDA` at all, only
   `GGA`/`RMC`/`VTG` — designing against an NMEA sentence this hardware has never been observed
   to send would be a real feasibility risk, whereas `RMC` is already confirmed present and
   carries both fields needed. `VTG` is not parsed (its only content — speed/course — is already
   available from `RMC`).
3. **No fix-quality/HDOP/satellite-count threshold.** Any non-zero `GGA` fix quality counts as a
   fix — a NEO-6M-class module's first fix is typically loose (HDOP 3-10+, 4-6 satellites),
   requiring quality ≥ 2 (DGPS) would likely never be satisfied on this hardware at all, and
   wardriving's own accuracy tolerance (tens of meters, same as any WiGLE-style capture) doesn't
   need tighter. A user-configurable threshold is backlogged separately, not built now.
4. **Wardriving's "depends on a GPS fix" is a record-level filter, not a `start`-level gate.**
   `wardriving start` is never rejected for lack of a fix — it behaves exactly as it does today
   (rejected only for the existing reasons: already running, unsupported source, busy). Instead,
   any record captured while the location driver is not reporting `state = "fix"` is discarded —
   never logged to flash, never streamed to the Flipper — and this is a continuous rule, not a
   one-time "before the first fix" check: a fix lost mid-capture (module unplugged, tunnel, etc.)
   pauses logging until the fix returns, without auto-stopping the capture. Rejected alternative:
   gating `start` itself on an existing fix, considered and then walked back once it became clear
   it just reinvents the record-level discard behavior [CAPABILITIES.md](CAPABILITIES.md) already
   documented as accepted, with worse UX (a hard rejection instead of "start now, it'll catch up").
5. **New `gps` capability + poll-only `gps_status`-shaped command**, not a push/unsolicited
   mechanism. Both the GPS screen (coordinates/time/speed in [UI_REDESIGN.md](UI_REDESIGN.md)) and
   the Wardriving screen's fix icon need live status independent of whether a capture is running,
   which nothing in the wire protocol provided before this — but GPS position changing at
   walking/driving speed doesn't need push latency, so polling only while the relevant screen is
   open is sufficient and keeps this idle-cost-free otherwise (this board runs unattended for
   hours during a real wardriving session).
6. **Wardriving screen's Start action gets a cosmetic label toggle** ("Start" when a fix exists,
   "Start (delayed)" otherwise, read from the same `gps` status the fix icon already needs) rather
   than being disabled/greyed when no fix — matches decision 4: the action always behaves the
   same regardless of label, so disabling it would be misleading, not protective. This applies to
   whichever Wardriving screen ships it — today's existing flat-button screen
   (`flipper/flipper_esp32_over_ble.c`) now, and [UI_REDESIGN.md](UI_REDESIGN.md)'s future
   redesigned screen later, both reading the same underlying `gps` status.
7. **New `utc_timestamp_s` wardriving-record field, additive (not replacing `timestamp_ms`).**
   Unix epoch seconds derived from `RMC`. Guaranteed present and valid on every logged record
   (decision 4 already requires a valid fix, which requires a valid `RMC`, to log at all) — no
   backward-compatibility/optional-field case to design around. The Flipper's CSV exporter uses it
   directly for WiGLE's `FirstSeen` column, in the exact format WiGLE's spec requires
   (`YYYY-MM-DD hh:mm:ss`, UTC — confirmed against https://api.wigle.net/csvFormat.html during
   this design session), replacing the current RTC-anchored backdating approximation for any
   record that carries the new field.

**Done when:** matches the project's established two-stage bar — both firmwares build- and
host-test-verified against the frozen contract above (including shared vectors for the new `gps`
status shape and the `utc_timestamp_s` field), then hardware-verified: the real module correctly
drives all three `gps` states through a cold-start-to-fix cycle, a wardriving capture started
before a fix arrives logs nothing until `state = "fix"`, a fix lost mid-capture pauses logging and
resumes correctly when it returns, and the Flipper's exported CSV `FirstSeen` column matches
WiGLE's format using the new real timestamp.

**Both sides implemented 2026-09-12, build- and host-test-verified independently, hardware
verification of the complete feature not yet started.**

- **ESP32 side:** `esp32/main/nmea_parser.c`/`.h` (new, pure/host-testable `GGA`/`RMC` parser, no
  floats, Howard Hinnant `days_from_civil` for UTC→Unix time), `esp32/main/location.c`/`.h`
  (rewritten: real UART1 driver, dedicated `gps_parse` FreeRTOS task with its own stack and
  file-scope line buffer, `portMUX`-guarded state snapshot read by the NimBLE host task),
  `esp32/main/cbor_gps.c`/`.h` (new, the `gps` capability's wire codec), `"gps"` added to the
  capability list, `handle_gps_command()` in `main.c`, and both wardriving record-capture call
  sites updated for the fix-dependency and `utc_timestamp_s`. `idf.py build` clean; all host-native
  test suites pass, including new `gps` vectors added to the shared `tests/vectors/vectors.h`.
- **Flipper side:** new `gps` command client/status parsing (`flipper/cbor_gps.c`/`.h`), the
  `utc_timestamp_s` wardriving-record field (`flipper/cbor_wardriving.c`/`.h`), a poll-only `gps`
  status query while the Wardriving screen is open (2s cadence, a `FuriTimer` started/stopped on
  screen entry/exit — see `flipper_esp32_over_ble.c`'s `gps_poll_timer`), the Wardriving screen's
  three-state fix indicator and "Start"/"Start (delayed)" label toggle (decision 6), and the WiGLE
  CSV `FirstSeen` column now built directly from `utc_timestamp_s` (the old RTC-anchored
  backdating approximation and `feb_wardriving_backdate_first_seen()` were removed as dead code
  once the field became mandatory on the wire). `fbt.cmd fap_flipper_esp32_over_ble` clean;
  523/523 host-native checks pass; `tools/check_shared_headers.py` confirms both sides'
  `cbor_gps.h`/`cbor_wardriving.h` agree on field order/types.
- **Known implementation notes, not covered by the frozen design above:**
  - **Old on-flash wardriving records will fail to decode and be silently skipped** once this
    ships, since they predate the mandatory `utc_timestamp_s` field — the existing
    checksum/decode-failure path in `wardriving_log.c` already handles this safely (skip, warn,
    don't crash or misread), and the circular log naturally rotates them out as new captures
    continue. **Accepted by the user 2026-09-12** as a one-time cost of this format upgrade — not
    a bug, no migration built.
  - **The already-existing GPS screen** (`draw_gps_screen`, part of the Phase 3a menu shell — see
    [UI_REDESIGN.md](UI_REDESIGN.md)) is now wired to the new live `gps` status (2026-09-12,
    build-verified only, explicitly-approved follow-on) — real fix state, coordinates, and
    GPS-derived UTC time when fixed, falling back to the Flipper's RTC clock otherwise, speed
    still `--`. Found and fixed a pre-existing capability-gating bug along the way:
    `HomeMenuGps`'s visibility was checking `capability_has_wardriving` instead of
    `capability_has_gps`.

## Phase 4: Heltec WiFi LoRa 32 V2 board support

**Status: in progress, started 2026-09-16.** Phase 3's backlog was **not** cleared when this
phase started — `docs/BACKLOG.md` still had open P0 items (G03, G06, G07, G09, BL01) and P1
items (G08, G10, BL05, BL06, G36, BL04, BL10, BL11, BL12), and `docs/HARDENING_BACKLOG.md` had
H01–H04 open. **The user explicitly overrode this phase's own gate ("does not start until
Phase 3 backlog is cleared") on 2026-09-16**, via a grill-me design session, choosing to start
Phase 4 now rather than clear the backlog first. Backlog items stay fully deferred, not
interleaved with Phase 4 work — see `docs/BACKLOG.md`'s note. The step breakdown below,
originally written as a frozen plan for a future session, is now the actual in-progress step
list.

**No physical Heltec board has been acquired.** Hardware facts are recorded in
[docs/hardware/heltec-wifi-lora-32-v2/README.md](hardware/heltec-wifi-lora-32-v2/README.md),
sourced from Heltec's/Espressif's published documentation and clearly marked
per-vendor-documentation-only, not locally verified — mirroring the precision discipline of
[docs/hardware/esp32-c6-devkitc-1/README.md](hardware/esp32-c6-devkitc-1/README.md). See
`docs/BASELINES.md`'s Heltec stub for the pinned-baseline framing of this board.

### Architecture decision: shared component (confirmed 2026-09-16)

Today's `esp32/` is a single ESP-IDF project hardcoded to `CONFIG_IDF_TARGET="esp32c6"` in
`sdkconfig.defaults`. A Heltec target needs `esp32` (classic Xtensa), a different target —
ESP-IDF does not support two targets from one `sdkconfig`/build tree. Two ways to structure
this:

- **(a) Second top-level project (`heltec/`) sharing protocol/crypto logic with `esp32/` via
  an ESP-IDF shared component directory** (`EXTRA_COMPONENT_DIRS`) — `framing.c`, `pairing.c`,
  `pairing_crypto.c`, `session.c`, `session_crypto.c`, and the `cbor_*.c` codec files move into
  a shared component consumed by both `esp32/` and `heltec/`, each with its own board-specific
  `main.c`/capability handlers/`sdkconfig.defaults`/target. `docs/PROTOCOL.md` stays implemented
  exactly once across both ESP32-family boards.
- **(b) Two fully independent project trees**, each separately implementing the protocol —
  duplicates every line of framing/crypto/codec logic, with the drift risk that already
  motivates this project's "keep the two firmwares in lockstep" convention (`CLAUDE.md`) —
  except now across **three** independent implementations (Flipper, C6, Heltec) instead of two.

**Recommendation: (a).** The shared-component approach is a build-system detail (ESP-IDF's
`EXTRA_COMPONENT_DIRS` is designed exactly for this — one component consumed by multiple
project trees/targets), not a rewrite, and it's the only option consistent with this project's
own stated convention that a wire-format/crypto change "is not done until both sides implement
it identically." Duplicating the protocol logic into a third tree triples the surface area for
exactly the convergence bugs `docs/LESSONS.md` already documents recurring between two
implementations.

**Confirmed 2026-09-16 (grill-me session): (a), shared component.** This was flagged per
`CLAUDE.md`'s own rule ("Changing any of these [baselines] is a project decision... confirm
with the user before proceeding"), and the user picked (a) over (b) or a third structure. Step
2 below (project scaffolding) is unblocked.

### Step breakdown

Mirrors how Phase 1–3 steps are written above (numbered steps, each with a "Done when" line).
Step 1 is done (2026-09-16); steps 2–6 have not started.

**1. Board acquisition + baseline bring-up.**
- Acquire a physical unit and confirm its exact revision (V2 vs V2.1 vs V1/V3+) against its
  silkscreen/label — see the hardware doc's "Board revision ambiguity" section.
- Verify flash size via `esptool flash_id` — **read-only**, `--before default_reset --after
  no_reset` per this project's `esptool` read-only convention (see `docs/LESSONS.md`); do not
  flash/erase/write without explicit user request.
- Build an unmodified `esp32` target baseline (plain ESP-IDF example or a minimal skeleton) for
  this specific chip, independent of any project code, the same way Phase 1 did for the C6.

**Done when:** board revision confirmed, flash size measured read-only, an unmodified `esp32`
target baseline builds and boots on the physical unit. ✅ **Met 2026-09-16** — board confirmed
from its own silkscreen as "WiFi LoRa 32 V2" (not V2.1), chip ESP32-D0WDQ6 rev v1.0, MAC
`a4:cf:12:03:ba:58`, 8MB Winbond flash measured via read-only `esptool flash_id`
(`--before default_reset --after no_reset`, no erase/write) on COM10 (Silicon Labs CP210x
bridge) — matches the hardware doc's V2/V2.1 8MB expectation and rules out V1's 4MB. The
classic-`esp32` (Xtensa) toolchain was not yet installed (this project had only ever installed
`esp32c6`'s RISC-V toolchain); installed via `idf_tools.py install --targets=esp32`. An
unmodified `hello_world` example (built in a throwaway temp directory, not committed to this
repo) built and flashed cleanly; serial output confirmed a clean boot (`Hello world!`, correct
chip/flash identification, no crash/reset loop beyond the example's own restart countdown).

**2. Project scaffolding**, per whichever architecture the user confirms above. If (a): create
the shared component directory, move the listed files into it with no behavior change, wire
`EXTRA_COMPONENT_DIRS` into both `esp32/`'s and the new `heltec/`'s `CMakeLists.txt`, and
confirm `esp32/`'s existing build and all its host-native tests are unaffected by the move
before adding any Heltec-specific code.

**Done when:** both `esp32/` (unchanged behavior) and a new empty `heltec/` skeleton build
against the shared component, with `esp32/`'s existing test suite still passing. ✅ **Met
2026-09-16.** Shared component created at `components/feb_protocol/` (13 file pairs moved via
`git mv`, no logic changes: `framing`, `pairing`/`pairing_crypto`, `session`/`session_crypto`,
all `cbor_*` codec files, `cbor_codec.h`/`cbor_internal.h`; `REQUIRES mbedtls` only — none of
the moved files touch ESP-IDF/FreeRTOS headers). `EXTRA_COMPONENT_DIRS` wired into both
`esp32/CMakeLists.txt` and the new `heltec/CMakeLists.txt`. New `heltec/` project targets
`esp32` (classic Xtensa) with a trivial `main.c` that includes `framing.h` and calls
`feb_fragment_capacity()` to prove the link, nothing more. `esp32/`'s `idf.py build` and all
five `tests/esp32/*.ps1` host-native suites pass unchanged (three scripts repointed to the new
shared-component path); `heltec/`'s `idf.py build` passes against the default (uncorrected)
2MB flash-size assumption — the real 8MB partition sizing is out of this step's scope.
`tools/check_shared_headers.py` repointed to the new paths and passing. Full detail:
`docs/PROJECT_HISTORY.md`.

**3. Port/reuse the BLE transport + pairing + session crypto layer onto classic ESP32.**
Nothing here is assumed working without re-verification:
- **NimBLE central-mode support on classic ESP32** — confirm it's available and behaves the
  same as on the C6 (central role, GATT client, notification subscription); classic ESP32 ships
  a different combo Wi-Fi/BT radio than the C6's single 2.4 GHz Wi-Fi6+BLE5+802.15.4 radio, so
  none of Phase 3/step 4's coexistence bounds transfer — this needs its own coexistence
  validation pass (see the radio note below), not an assumption that the C6's numbers apply.
- **mbedTLS primitive availability** — confirm X25519, HKDF-SHA-256, HMAC-SHA-256, and
  AES-256-GCM are equally available on the `esp32` target. mbedTLS itself isn't chip-specific,
  so this should be true, but per this project's "confirm, don't assume" discipline (see
  `docs/BASELINES.md`'s C6 entry, which explicitly confirmed rather than assumed the same list),
  it must be checked against this target's actual sdkconfig/component availability, not inferred
  from the C6 having it.
- Re-implement the ESP32-side halves of `docs/PROTOCOL.md`/`docs/PAIRING.md` against the shared
  component from step 2, byte-for-byte identical wire behavior to the C6 build.

**Done when:** the Heltec build passes the same shared host-native codec/crypto test vectors as
the C6 build, and a real pairing + authenticated session round-trip is hardware-verified against
a Flipper, independent of any capability beyond the base protocol. ✅ **Met 2026-09-16.**
`heltec/main/main.c` has the full transport/pairing/session-auth port (base protocol only);
`heltec`'s `idf.py build` and `esp32`'s own build + all host-native test suites pass. Flashed to
the physical Heltec board (COM10): fresh-boot pairing ceremony against the Flipper completed
(X25519 exchange, `pair_confirm`, secret persisted, `pair_complete`), and a subsequent reset
completed the runtime `hello`/`hello_ack`/`client_auth` round-trip using the stored secret with
no pairing window reopened. Full detail: `docs/PROJECT_HISTORY.md`'s 2026-09-16 entry.

**4. `board_id` / multi-board-pairing implications.** Today's `board_id` format is
`esp32c6-<12 lowercase hex chars>`, derived from the factory MAC (see step 5/7 above and
`docs/PROTOCOL.md`). A Heltec board needs its own distinguishable prefix (e.g.
`heltec-<12 lowercase hex chars>`) so the Flipper's per-`board_id` pairing-record files
(`pairings/<board_id>.dat`) and capability caches keep boards distinct. This is a small but real
shared-contract detail, not a wire-format change — `board_id` is already opaque,
charset-validated text per step 5, so a new prefix value requires no protocol/CBOR shape change,
just an ESP32-side constant and confirmation that the Flipper's existing charset validation
accepts it unchanged.

**Done when:** a Heltec board and a C6 board can be paired to the same Flipper simultaneously
(subject to the existing "one BLE connection at a time" limitation — see "Multi-board pairing"
under step 7 above), each keeping its own pairing record and capability cache with no filename
collision. ✅ **Met 2026-09-16.** No code change was needed: `heltec/main/main.c` already derives
`board_id` as `heltec-<12 lowercase hex chars>` (distinct from the C6's `esp32c6-` prefix), and
the Flipper's pairing/capability storage (`build_pairing_path`/`build_capability_path` in
`flipper/flipper_esp32_over_ble.c`) was already fully generic on `board_id`/`board_id_len` with
no fixed-prefix/length assumption. Verified by inspecting the Flipper's SD card (`scripts/
storage.py list /ext/apps_data/flipper_esp32_over_ble`, read-only): `pairings/
esp32c6-acebe6fffeda.dat` and `pairings/heltec-a4cf1203ba58.dat` coexist (32 bytes each), as do
`capabilities/esp32c6-acebe6fffeda.dat` (82 bytes, real capability list) and `capabilities/
heltec-a4cf1203ba58.dat` (55 bytes, zero-feature base protocol) — no collision.

**5. Radio/coexistence note.** Unlike the C6 (one 2.4 GHz radio shared by Wi-Fi, BLE, and
802.15.4), the Heltec's LoRa radio is a separate SPI-attached chip (SX1276/SX1278) on its own
antenna and sub-GHz frequency band — it does not compete with the classic ESP32's Wi-Fi/BLE
radio the way `wifi_scan`/`ble_scan`/`wardriving` compete for the C6's single radio today. That
said, the classic ESP32's Wi-Fi+BT combo radio still needs its own from-scratch coexistence
validation (mirroring Phase 3/step 4's methodology) before `wifi_scan`/`ble_scan`/`wardriving`
are trusted on this board — it is a different SoC with a different radio implementation; none
of the C6's measured bounds are assumed to transfer.

**Done when:** an equivalent of step 4's coexistence sweep (Wi-Fi scan + active BLE connection +
BLE observer scan running together) passes on the Heltec board, with its own recorded interval
bounds — not borrowed from the C6's. **Skipped by explicit user decision, 2026-09-16** — not
done, not attempted. No coexistence bounds exist for this board's Wi-Fi+BT combo radio. Whoever
later ports `wifi_scan`/`ble_scan`/`wardriving` onto the Heltec (no such step is currently
written into this Phase 4 plan) must not assume the C6's bounds transfer and must do this
validation first, or as part of that work.

**6. `display`/`lora` capability design — explicitly out of scope for this document.** Sensible
scope for a `lora` capability (recon/sniff only vs. TX capability, frequency/region regulatory
constraints, antenna-presence assumptions) and a `display` capability (what it renders, whether
it's push or poll, ESP32-local vs. Flipper-driven) cannot be responsibly designed without the
physical board in hand and steps 1–5 done first. This needs its own dedicated grill-me design
pass, the same way `gps` got one before implementation (see the "Real GPS driver..." section
above) — do not design or implement `display`/`lora` wire formats as a side effect of this
phase's earlier steps.

**7. `wifi_scan`/`ble_scan` capability porting** (added 2026-09-17, by explicit user request —
not originally written into this Phase 4 plan). Unlike `display`/`lora`, these two capabilities
are already fully specified (`docs/PROTOCOL.md`, `docs/CAPABILITIES.md`) and implemented on the
C6 — this is a straight port of already-agreed wire behavior onto a second board, not new
capability design. `wardriving` and `gps` are explicitly excluded from this step: `wardriving`
needs the coexistence-interval bounds that step 5 above skipped, and `gps` needs physical UART
wiring not yet documented for this board (see `docs/hardware/heltec-wifi-lora-32-v2/README.md`).

**Done when:** `heltec/main/main.c` reports `wifi_scan`/`ble_scan` in its capability response
and both commands work end-to-end against a real Flipper, exercised concurrently with the
active BLE connection (unlike step 5's skipped sweep, a manual one-shot scan is a bounded,
low-risk action, but this board's Wi-Fi+BT combo radio has never been hardware-tested running a
scan while BLE-connected — that gap must be closed by an actual test, not assumed away).
✅ **Build-verified 2026-09-17** — both capabilities ported from `esp32/main/main.c`'s reference
implementation into `heltec/main/main.c` (reusing the shared, unmodified
`components/feb_protocol/cbor_wifi_scan.c`/`cbor_ble_scan.c` codecs), `feb_features[]` now
`{"wifi_scan", "ble_scan"}`, `idf.py build` passes for both `heltec/` and `esp32/`. **Hardware
verification still pending** — see `docs/PROJECT_HISTORY.md`'s 2026-09-17 entry for the full
change narrative. Flashed to the physical board (COM10) 2026-09-17; boot log confirmed healthy
(Wi-Fi STA init, NimBLE scan start, no crash). The Flipper's stale cached capability record
(`capabilities/heltec-a4cf1203ba58.dat`, zero features from the step-3 test) was deleted the
same day, so the next session's `capability_query` will reach this firmware's real feature
list — an actual paired `wifi_scan`/`ble_scan` round-trip against the Flipper is still
untested.

### Cross-references

- Board/pin facts: [docs/hardware/heltec-wifi-lora-32-v2/README.md](hardware/heltec-wifi-lora-32-v2/README.md).
- Baseline framing: `docs/BASELINES.md`'s Heltec stub section.
- Capability roadmap placement: `docs/CAPABILITIES.md`'s closing note on `display`/`lora`.
- Do not carry forward C6 pin mappings or radio-coexistence bounds to this board, or vice versa
  — both hardware docs state this caution independently.

## Backlog

Every open, not-yet-scheduled item (defects, deferred product decisions, cost/efficiency work)
now lives in the single centralized [docs/BACKLOG.md](BACKLOG.md) — that file explains how to
use it and links to full detail per item. Resolved items' full narrative is in
[docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md); this file does not keep a shadow "resolved" log.

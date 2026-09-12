# Implementation Plan

This plan implements the trusted-environment BLE pairing decision in [DECISIONS.md](DECISIONS.md) and protocol v2 in [PROTOCOL.md](PROTOCOL.md). The target board is the ESP32-C6 DevKitC-1-N4.

## Roadmap phases

- **Phase 1 (done):** board/SDK/firmware/build baselines — see `docs/BASELINES.md`.
- **Phase 2 (done):** core BLE transport, record framing, trusted-environment pairing, and authenticated runtime sessions on the ESP32-C6 — steps 1-9 below are this phase's implementation detail. Steps 1-7 are implemented and hardware-verified; see `docs/SESSION_MEMORY.md` for current status.
- **Phase 3a (immediate next step, 2026-09-12):** Flipper UI architecture + menu redesign. This is the first implementation slice for the user-approved Home/menu design in [UI_REDESIGN.md](UI_REDESIGN.md). It covers the `ViewDispatcher`/scene-manager prerequisite, Home screen menu shell, capability-gated item visibility, reconnect-stays-put behavior, and the GPS/Settings/About placeholders. It does not supersede the remaining Phase 3 wardriving validation work; it is intentionally scheduled immediately before the next production polish pass because the app architecture itself is the gating prerequisite for the redesign.
- **Phase 3 (in progress, decided 2026-09-07):** production-ready wardriving on the ESP32-C6. Covers the follow-on `wifi_scan`-command step (done), the GPS/`ble_scan`/`wardriving` capability, step 8 (hardened persistence for both the pairing record and the wardriving log), and step 9 (full-system validation). "Production-ready" means field-usable unattended for hours, survives power loss without corrupting the wardriving log, and passes step 9's negative-security-test suite — not just "the happy path works once on a bench."
- **Phase 4 (later, decided 2026-09-07):** Heltec WiFi LoRa 32 V2 board support — a second, structurally different target (classic ESP32/Xtensa, not C6) adding display and LoRa capabilities. Does not start until Phase 3 is complete.
- **Phase 5 (later, much larger, decided 2026-09-07):** Zigbee/Thread and `gpio_control`. Zigbee/Thread recon (passive scanning, Phase 5a) first, then participation (active stack join / possible border-router role, Phase 5b) as a separately-scoped, order-of-magnitude-larger effort with no committed timeline. `gpio_control` rides along in this phase rather than blocking Phase 3's wardriving focus.

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

**Done when:** interrupted writes, reboot during pairing, unpair, and factory reset leave no ambiguous paired state, for both the pairing record and the wardriving log. Wardriving-log persistence: implemented as part of the reorder above (checksummed circular flash log, `esp32/main/wardriving_log.c`/`wardriving_record_format.c`), build- and host-test-verified; hardware acceptance for it specifically (extended unattended wraparound/power-loss run) is still open — see [BACKLOG.md](BACKLOG.md). Pairing-record/capability-file hardening: not yet started.

**See also:** "Deferred: hardware hardening" below — Secure Boot, flash encryption, and related eFuse-dependent work are explicitly out of scope for this phase and are not part of this step's "done when" bar.

## 9. Validate the complete system

- Run codec, key-derivation, X25519, HMAC, AES-GCM, and CBOR test vectors on both targets.
- Fuzz the fragment and CBOR parsers with truncated, oversized, duplicated, and invalid inputs.
- Exercise first pair, expired window, reset-and-repair, reconnect, ciphertext tampering, replay, bad confirmation, power loss, and unpair flows.
- Confirm the idle-connection timeout (30 seconds without a record, per [PROTOCOL.md](PROTOCOL.md)) does not fire spuriously during a live wardriving view session through a stretch with no new results — add a keepalive/heartbeat if needed (see Backlog).
- Re-confirm (not re-derive) step 4's radio-coexistence interval bounds under real authenticated, streaming wardriving traffic load — step 4's fixed-payload/throwaway-timer test validates the coexistence *mechanism*, which doesn't depend on payload content, but step 6's per-record AES-256-GCM/HMAC compute and streaming capability traffic are heavier loads step 4 never exercised. Also exercise the merged-reconnect-scan mechanism under a live forced disconnect (step 4 never triggered it — see step 4's "accepted gap").
- Document tested ESP-IDF and Flipper firmware revisions, flashing steps, reset behavior, and residual security boundary.

**Done when:** the full pairing-to-command flow succeeds repeatedly on the physical C6 and Flipper, and every negative security test has the specified rejection behavior. Not yet started.

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

## Backlog

Every open, not-yet-scheduled item (defects, deferred product decisions, cost/efficiency work)
now lives in the single centralized [docs/BACKLOG.md](BACKLOG.md) — that file explains how to
use it and links to full detail per item. Resolved items' full narrative is in
[docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md); this file does not keep a shadow "resolved" log.

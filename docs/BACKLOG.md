# Backlog

The single, centralized list of every open, actionable item that isn't part of the current
roadmap step's own scope: code defects, robustness gaps, deferred product decisions, and
cost/efficiency work. This is the "check before starting related work" list for anything not
already blocking the phase in progress.

- **Current in-flight state** (what's being worked on right now) lives in
  [SESSION_MEMORY.md](SESSION_MEMORY.md), not here.
- **Roadmap steps and their "done when" bars** live in [PLAN.md](PLAN.md), not here.
- **Finished work** (bug found, root-caused, fixed, verified) belongs in
  [PROJECT_HISTORY.md](PROJECT_HISTORY.md) — mark an item `DONE YYYY-MM-DD (commit)` below only
  long enough for the next session to notice, then delete the row once the PROJECT_HISTORY.md
  entry exists.

Each row is a one-line pointer, not the full write-up — follow the link for exact file/line
evidence, a suggested fix, and tests to add. Do not re-derive detail that already lives
elsewhere:

- **G-numbered items** → [grok-4.6-findings-2026-09-11.md](grok-4.6-findings-2026-09-11.md)
  (evidence, exact fix, tests, dependencies).
- **BL-numbered items** → detail inline below (no separate appendix exists for these).
- **Roadmap-gated items** → the named `PLAN.md` step. Do not fix them out of order or sneak
  them into an unrelated PR — see `CLAUDE.md`'s conventions.

Severity scale (matches the grok appendix): **P0** spec/security correctness · **P1** real bug
in normal use · **P2** robustness/defense-in-depth/cost · **P3** style/docs drift.

## P0 — correctness / security

| ID | Title | Status |
| --- | --- | --- |
| G03 | ESP32 marks the session `AUTHENTICATED` on its own GATT write-complete, not on peer confirmation | Open — needs a product decision on the `capability_query`-caveat (see appendix) |
| G04 | Pairing ceremony (`pair_init`→`pair_complete`) has no application-level timeout | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G05 | Absolute `uint32_t` millisecond deadlines wrap at ~49.7 days uptime | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G06 | Neither firmware sends the spec-mandated `unsupported_version` error + close | Open |
| G07 | Any `send_protected*` clobbers an in-flight wardriving backlog drain (generalizes past `start`) | Open — **deferred at explicit user request**; re-confirm before implementing (see "Deferred" below) |
| G09 | Flipper advances `session_seq_out` even when notify delivery is unknown | Open |
| G11 | Flipper never closes the connection on auth/GCM/sequence failure (relies on ESP32's 30s idle timeout) | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| BL01 | Flipper's `handle_pair_init()` runs unconditionally on any incoming `pair_init` — no local user-gesture/authorization-state check, contradicting [PAIRING.md](PAIRING.md) step 3's "user selects Add ESP32 board" | Open — needs a decision: implement the gate, or correct PAIRING.md if none was intended |

`G26` (AES-GCM 24-bit sequence cap not enforced) — **DONE 2026-09-11** (`3111fa2`); see
[PROJECT_HISTORY.md](PROJECT_HISTORY.md).

## P1 — real bugs in normal use

| ID | Title | Status |
| --- | --- | --- |
| G08 | SD-card I/O (pairing, capability cache, CSV) runs on `BleEventWorker`, the BLE-pump thread | Open |
| G10 | Flipper `session_key`/`session_seq_out`/`outgoing_message_id` accessed from two threads with no lock | Open |
| G14 | `any_saved_pairing_exists()` matches any directory entry, including a crashed-save `.dat.tmp` leftover | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G17 | `client_auth` proof failure leaves the Flipper UI stuck on "Authenticating…" | **DONE 2026-09-12** — see PROJECT_HISTORY.md; USER_GUIDE.md sync still pending (on-screen text now changes to "Failed: proof verification failed") |
| G27 | `pending_command_kind` is never cleared; a stray `internal_error` always looks like a wardriving self-stop | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G28 | Wardriving CSV writes a WiGLE header on every `FSOM_OPEN_APPEND`, not only on a genuinely new file | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G30 | Wardriving log/dedup state has no lock between the Wi-Fi `sys_evt` writer and the NimBLE-host drain reader | Open |
| G31 | `backlog_remaining` uses an unlocked `size_t` subtract — can underflow under G30's race | **DONE 2026-09-12** — see PROJECT_HISTORY.md (G30's underlying race is still open) |
| G13 | ESP32 NVS pairing blob has no version, validity marker, or atomic replacement | **Roadmap-gated → PLAN.md step 8.** Do not fix as a drive-by. |
| G36 | Wardriving BLE reconnect can stall permanently (discovery restarts every ~500ms, never matches) when wardriving's Wi-Fi source runs concurrently at its gapless default | Open — coexistence-starvation theory now well-supported (BLE-only isolation test: 7/7 disconnects recovered; earlier `wifi=1 ble=1` capture: stalled permanently), fix not yet designed. See `docs/PROJECT_HISTORY.md`'s "Wardriving reconnect stall" investigation (5 dated entries) and `docs/LESSONS.md`. |
| BL02 | Flipper doesn't query wardriving status on (re)connect — closing/reopening the FAP while the ESP32 is still capturing shows "status unknown" on the wardriving screen instead of the real running state | **DONE 2026-09-12** — landed as part of the GPS-driver commit (`1f0cb8e`): `send_wardriving_status_query()` fires on session auth (`handle_client_auth()`, if the board advertises `wardriving` and this session doesn't yet know its run state) and again on entering the Wardriving screen with the same guard; `handle_wardriving_status()`'s `"data"` branch also now marks the session as running the moment a real data batch arrives, even before a fresh `"started"` ack. Verified by reading the mechanism end-to-end (build-verified only, not independently hardware-retested this pass). |
| BL03 | Wardriving CSV filename is timestamped to the second (`wardriving_csv_ensure_open()`, `flipper_esp32_over_ble.c:1677`) and a new file opens on every reconnect (`wardriving_csv_close()` runs on every disconnect/teardown) — idle-timeout reconnect churn alone can mint many near-empty files per outing | **DONE 2026-09-12** — see PROJECT_HISTORY.md; landed together with G28 as required. USER_GUIDE.md's "one file per connected session" wording is now stale (a file now covers one calendar day, not one session) and needs a sync pass. Also see new BL04 below (dedup table no longer matches the file's new lifetime). |
| BL04 | Wardriving CSV dedup table (`wardriving_dedup_table`) is reset on every disconnect (`wardriving_csv_close()`, via `reset_scan_ui_state()`), but BL03 widened the on-disk CSV file's lifetime to per-calendar-day — a same-day reconnect now reopens the same file with a freshly-empty dedup table, so an address already written earlier that day can be re-logged as a duplicate row (not a duplicate header; G28 still prevents that) | Open — discovered while implementing BL03/G28 (2026-09-12), not fixed as part of that change. Candidate fix: persist/rebuild the dedup table's scope to match the file's calendar-day scope (e.g. seed it from the existing file's addresses on reopen), or accept the quality regression and document it. |
| BL05 | User-reported: staying on the Wardriving screen while wardriving is active breaks the Flipper<->ESP32 BLE connection; leaving the app on any other screen while wardriving continues is fine | Root cause found (2026-09-12, ESP32 side, in progress concurrently — see `esp32/main/main.c`'s `handle_gps_command()`): the Flipper's `gps_poll_timer` (this Flipper commit, 2s period, only runs while the Wardriving or GPS screen is open) drives `send_gps_command()`, and the ESP32's `queue_and_send_protected()`/`queue_encoded_record_for_tx()` share one single-in-flight `tx_fragment_*` state across every capability with no re-entrancy guard — a `gps` status reply sent while a wardriving `"data"` batch is still mid-fragmentation corrupts/loses that shared state, breaking the connection. Exactly explains why it only happens on the two screens that poll `gps` and only while wardriving is actively streaming. Not a Flipper-side bug; no Flipper-side change needed once the ESP32-side guard lands. |
| BL06 | User-reported: the Flipper's LED is constantly solid green once connected during a wardriving session, when the design (`wardriving_flush_led_active`, PROTOCOL.md's `backlog_remaining` semantics) intends solid-green only while flushing a backlog and solid-blue once caught up (`backlog_remaining == 0`) | Investigated 2026-09-12, not fixed — the Flipper-side logic matches PROTOCOL.md's documented semantics exactly (`handle_wardriving_status()`'s `"data"` branch); no Flipper-side bug found. Leading theory: a real, continuously-topped-up backlog (this project's own session notes mention old on-flash records plus live capture) may mean the ESP32 rarely or never actually reports `backlog_remaining == 0` during active use, making "solid green" technically correct-per-protocol but not what a user watching for "richer state" expects. Needs the ESP32 side's actual `backlog_remaining` value sampled live during a real session to confirm, and/or a product decision on whether the LED design itself should change (e.g. distinguish "genuinely never caught up" from "connected, no wardriving activity"). |

## P2 — robustness / cost / defense-in-depth

| ID | Title | Status |
| --- | --- | --- |
| G12 | Flipper fragments every record at ATT MTU 23 (16-byte payload) even after MTU negotiation | Open — ~3-4x fewer BLE notifications per record if fixed |
| G15 | ESP32 HMAC `full[32]` scratch not zeroized after truncating to the 16-byte wire value | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G16 | Factory reset doesn't zeroize in-RAM `stored_pairing_secret` before `esp_restart()` | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G18 | Flipper X25519 donna static ladder scratch (~3-4 KB) never zeroized, resident for the app's lifetime | Open |
| G19 | Reconnect still `xTaskCreate(..., 3072)` just to sleep once, every ~30s during a prolonged outage | Open |
| G20 | `notify_data_callback`'s NULL-context path sets `*data_len = PAYLOAD_MAX` instead of `0` | **Not a bug — the suggested fix was wrong and broke runtime auth.** Reverted 2026-09-12; see PROJECT_HISTORY.md. |
| G21 | Pairing/capability/CSV path buffers sized at 96 bytes, one constant short of the real max (~137) | Open |
| G23 | Flipper reassembly-complete buffer read after mutex release; `profile_start()` resets it unlocked | Open |
| G24 | ESP32 built with `-Og`, not `-Os` | **Product choice, not a bug** — record in BASELINES.md if changed |
| G25 | 256-byte stack buffer in the ESP32's NimBLE notify-RX path (same class as 4 prior stack-overflow bugs) | Open |
| G32 | Factory-reset LED RMT channel leaks on partial init failure | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G33 | `board_id_len` takes `snprintf()`'s return value verbatim; `<stdio.h>` not directly included | **DONE 2026-09-12** — see PROJECT_HISTORY.md |
| G34 | ESP32 `feb_gcm_encrypt` failure path uses `memset`, not `feb_secure_zero` | **DONE 2026-09-12** — see PROJECT_HISTORY.md |

`G35` (ESP32 `wardriving_dedup_reset()` wiped the flash-log-gating dedup table on every
`start`/`stop`) — ✅ done, see [PROJECT_HISTORY.md](PROJECT_HISTORY.md).

`G22` (wardriving dedup table shared 128 slots across Wi-Fi/BLE with silent collision
eviction) — ✅ done, see [PROJECT_HISTORY.md](PROJECT_HISTORY.md).

## Codebase & agent cost-efficiency

Carried over from the retired `docs/OPTIMIZATION.md` (folded here 2026-09-11; its "done" items
are now in [PROJECT_HISTORY.md](PROJECT_HISTORY.md)):

- **Extract a capability-dispatch layer** from both `main.c`/`flipper_esp32_over_ble.c` — pays
  off on every future capability. Hold until a natural roadmap boundary; decide the
  static-buffer-arena question first (see
  [LESSONS.md#static-buffer-pattern-trades-ram-for-stack-safety](LESSONS.md#static-buffer-pattern-trades-ram-for-stack-safety)).
- **BLE active scanning**: enabled for `ble_scan` 2026-09-11 (`e92aad9`), and confirmed
  2026-09-12 to already be hardcoded on for `wardriving`'s own capture engine too — all four
  `ble_gap_disc()` call sites in `esp32/main/main.c` set `passive=0` (the "extending it to
  wardriving's own capture engine" item once tracked here is done, not open). Still open: a
  **runtime active/passive toggle** (nothing today can request passive scanning — a
  prerequisite for [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s "Scan" menu's two passive modes),
  and measuring the real-world name-discovery improvement once hardware-tested.
- Do **not** split `flipper/pairing_crypto.c` (kept diffable against upstream curve25519-donna
  for auditability) or `tests/vectors/vectors.h` (98KB, generated — never `Read` it whole).
- `tests/flipper/build.ps1` fails out-of-the-box on a machine where Visual Studio's
  `vcvars64.bat` shells out to `vswhere.exe` by bare name and the VS Installer directory isn't
  already on `PATH` (surfaced 2026-09-12 while verifying the LED-indicator feature). Needs a
  one-line `PATH` prepend in that script; not yet fixed.
- **No canonical, agent-usable build/flash scripts for either platform** — ✅ done 2026-09-12, see
  PROJECT_HISTORY.md. `tools/build_esp32.ps1` (extended: build, plus optional `-Port`,
  `-SkipBuild`, `-CaptureBootLog`/`-CaptureSeconds`), `tools/build_flipper.ps1` (new: syncs into
  the pinned Unleashed checkout's `applications_user/` and builds, optional `-Port` to also
  transfer), and `tools/flash_flipper.ps1` (new: transfers a built FAP to the Flipper's SD card
  via `runfap.py`, never auto-launches) are the canonical entry points now.

## Other open items (not covered by the cross-model review)

- Promote implicit cross-firmware constants into the shared contract — e.g. the Flipper's
  `PAYLOAD_MAX` (64) is silently duplicated as the ESP32's `FEB_FLIPPER_WRITE_CHAR_MAX_LEN`
  rather than living in `framing.h`/PROTOCOL.md where both sides' tests would catch drift.
- Step 9 must exercise the real negotiated ATT MTU, not only step 3's forced-small fragments —
  the oversized-write path has never been tested this way (how a later real ATT-length bug
  stayed latent through step 3).
- **Build-time stack-budget check** (`-fstack-usage`/`-Wstack-usage=N` in the FAP build).
  Highest-value tooling gap in this list: the same `BleEventWorker`-stack-overflow bug class has
  been found by crashing real hardware four times now (steps 3, 5, 7, wifi_scan).
- **Single cross-implementation CBOR/framing vector-runner tool** — the structural fix for the
  bug class behind the six pre-step-7 convergence findings
  ([CODE_REVIEW_FIX_PLAN.md](CODE_REVIEW_FIX_PLAN.md)'s explicit non-goal). Still not built;
  highest-value missing piece of test tooling.
- Real scrollable capability-list screen on the Flipper, once `features` grows past what the
  single status screen can show.
- Host-test coverage for `capability_query`/`capability_response` on the Flipper side —
  currently zero.
- Manual "disconnect current board" Flipper UI action, to free the BLE connection slot without
  powering a board off (step 7 scope).
- **No unpair UI action exists at all yet** (a broader gap than the disconnect item above) —
  confirmed a real practical cost 2026-09-12: after adding the `gps` capability to the ESP32,
  an already-paired board's Flipper-side capability cache (`docs/CAPABILITIES.md`'s "queried
  exactly once, never auto-refreshed" rule) kept it permanently invisible until the cached
  `capabilities/<board_id>.dat` file was deleted by hand via the Unleashed checkout's
  `scripts/storage.py -p COM8 remove ...` over the Flipper's CLI port — the only available
  workaround today. Any future capability added to an already-paired board will hit the exact
  same silent staleness until a real unpair action exists in the app.
- Automatic BLE arbitration between multiple paired boards — gated on an unresolved BLE-HAL
  question: can the Flipper's peripheral role advertise while already connected?
- **Real GPS driver + wardriving fix-dependency + real record timestamps** — **implemented on
  both firmwares 2026-09-12**, build- and host-test-verified independently on each side; hardware
  verification of the complete feature (both sides together, on a real module) not yet started.
  Full design: [PLAN.md](PLAN.md)'s "Real GPS driver, wardriving fix-dependency, and real
  wardriving-record timestamps" (see its "Known implementation notes" for the accepted
  old-flash-record data loss and the GPS-screen-wiring follow-on); wire contract:
  [PROTOCOL.md](PROTOCOL.md)'s new `gps` section and `utc_timestamp_s` field;
  [CAPABILITIES.md](CAPABILITIES.md)'s `gps` and updated `wardriving` entries. Follow-on items
  this design deliberately left backlogged, not folded in:
  - GPS backfill-to-first-fix (buffer and retroactively backfill pre-fix records instead of
    discarding them) — considered as an alternative to the chosen continuous-discard behavior,
    not built.
  - A user-configurable fix-quality/HDOP acceptance threshold, as a board setting (the frozen
    design uses "any non-zero fix quality," no threshold).
  - Research into improving on-board GPS accuracy (antenna choice, SBAS/WAAS config, update
    rate, etc.) — raised during the design session, not investigated yet.
  - Real speed/heading on the GPS screen, from `RMC`'s speed/course fields — the frozen design
    already parses `RMC` for date/time, so this is now a smaller follow-on (read two fields
    already being parsed) than it would otherwise be, but is still not part of the frozen scope.
  - `wardriving_csv.c`'s WigleWifi-1.4 `AltitudeMeters`/`AccuracyMeters` columns are still
    hardcoded `"0,0"` (added 2026-09-12, GGA `altitude_dm` now exists in `feb_location_t` and
    the `gps` capability's own `altitude_dm_offset` result field — see PROTOCOL.md — but
    wardriving records/`<wardriving-record>` carry no altitude field of their own yet, so the
    CSV exporter has nothing to read). `AccuracyMeters` has no real source at all — the GPS
    module reports HDOP, not a meters-based error estimate (see docs/PROTOCOL.md's `gps`
    `hdop_e1` field) — so filling it would mean either an HDOP-derived approximation, documented
    as such, or leaving it `0`. Not folded into this altitude change since it requires a
    wardriving-record wire-format change (a new field on both firmwares), out of scope for a
    `gps`-capability-only addition.
  - Board-side autostart wardriving, independent of the Flipper initiating the session (a
    board-specific setting) — a new item raised during the design session, unscoped.
  - Runtime-configurable GPS UART GPIO pins via a Flipper Settings screen — the user's original
    ask included this, deliberately split out of the frozen design (see PLAN.md's "Scope
    boundary" note) because it needs a form/pin-entry widget this project doesn't have yet and a
    new get/set wire config surface. Defaults stay compile-time constants for now.
- Non-ASCII SSID rendering is untested on real hardware (host-native codec tests cover the
  encoding; no such network was available during `wifi_scan` verification). Not a blocker.
- Adopt a real `ViewDispatcher`/scene-manager architecture on the Flipper FAP instead of the
  single-`ViewPort`/`AppEvent`-queue pattern every screen has been bolted onto. Structural,
  no deadline. **Corrected 2026-09-12 (was stale):** this was previously framed as a hard
  prerequisite for [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s menu redesign, but that redesign
  shipped the same day built directly on the existing `ViewPort`/`AppEvent`-queue pattern instead
  — the prerequisite was skipped, not satisfied. Back to a structural nice-to-have with no
  blocking dependency, not a blocker for anything currently in flight.
- **New (2026-09-12):** the Flipper's "Scan" menu screen is only a placeholder-level Wi-Fi-scan/
  BLE-scan picker, not [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s actual five-mode BLE-active/passive
  live-view design (reusing Wardriving's capture engine without persistence). Needs its own
  implementation pass once the runtime BLE active/passive toggle above exists.
- **New (2026-09-12):** decide whether `AppScreenLegacy`/`HomeMenuLegacy` (a compatibility screen
  preserving the old direct-button-shortcut flow, found during the Phase 3a implementation but
  never part of [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s original design) is kept long-term or
  removed once Scan/GPS/Settings/About are trusted to fully replace it.
- **New (2026-09-12), cosmetic, needs a hardware/visual check:** the Home menu's "Connection
  lost" banner and each non-Home screen's own title may visually overlap — both are drawn at
  nearly the same canvas position (banner at y=12 `FontSecondary`, titles at y=11 `FontPrimary`).
  Found while reading `draw_callback` during the Phase 3a docs-accuracy pass; not confirmed on a
  real screen.
- **Consolidating/grouping wardriving records on the Flipper side** (e.g. de-duplicating or
  rolling up repeated/nearby sightings for display, as distinct from the ESP32-side capture-time
  dedup that already exists). Not scoped yet — needs its own planning/grill-me session before
  implementation, not a drive-by design call.
- **Reconsider the RSSI-improve dedup gate's comparison basis: last-written vs. best-ever.**
  Both `esp32/main/wardriving_dedup.c`'s `should_log_record()` and
  `flipper/wardriving_csv.c`'s `feb_wardriving_dedup_should_write()` re-trigger a write when RSSI
  improves ≥6dB versus the *last-written* observation for that address — which lets a signal that
  is merely fluctuating (not trending stronger), especially BLE's noisier RSSI, repeatedly clear
  the gate (e.g. -92→-82→-74 dBm, three writes, none of which beat an earlier peak). Investigated
  against WiGLE's own reference Android app (`wigle-wifi-wardriving`,
  `DatabaseHelper.java`'s `addObservation()`, `db/DatabaseHelper.java` on GitHub): its gate is a
  hybrid — a 64-entry in-memory LRU cache (`previousWrittenLocationsCache`) makes it compare
  against last-written for addresses still warm in cache (same behavior as this project), falling
  back to the DB's true best-ever `network.bestlevel` column only on a cache miss (likely the
  common case once a session exceeds ~64 concurrently-active addresses). Worth deciding whether to
  switch to (or approximate) a best-ever comparison here, which would stop repeat-fluctuation
  writes at the cost of never re-logging a device that's still below its historical peak. Related
  to the "Consolidating/grouping wardriving records" item above but narrower — this is about the
  comparison basis inside the existing per-address gate, not a new consolidation feature. Not
  scoped/decided yet.
- **Explore throttling the Wi-Fi source specifically during wardriving results-flush to the
  Flipper** — the flush was observed getting stuck before the recent dedup/sequence-cap fixes
  (`e616d81`, `3111fa2`). Possibly the same BLE/Wi-Fi coexistence starvation as G36, just
  triggered by the drain instead of by a reconnect; not yet isolated whether it still reproduces
  post-fix. Related to the "WiFi-source duty cycle" item under "Deferred by explicit product
  decision" below, but narrower — only during the flush window, not a general default change.

## Deferred by explicit product decision — confirm with the user before touching

- **Idle-connection heartbeat/keep-alive redesign.** The 30s idle-timeout disconnect/reconnect
  cycle works but causes a cosmetic LED/screen flicker. This is a wire-protocol change (both
  firmwares) needing its own design session — backlogged at the user's explicit request, not a
  quick patch.
- **G07** (TX single-flight clobbers wardriving drain) — user previously deferred the
  narrower `start`-only form; this entry generalizes it to `stop`/`capability_query` too.
  Re-confirm the deferral still stands before implementing.
- **G29** — ✅ done, see `docs/PROJECT_HISTORY.md`'s 2026-09-11 "Wardriving CSV dedup reset on
  restart fixed" entry (chosen scope: file lifetime, documented in `docs/CAPABILITIES.md`).
  Hardware re-verification (a real stop/restart mid-capture) still pending.
- **WiFi-source duty cycle** (`wifi_interval_ms` default `30000`, conservative) is now backed by
  the live reconnect-stall investigation and the same BLE coexistence guardrail; the default is set
  to 30s to avoid starving the shared radio during reconnect attempts while leaving a per-session
  override available for throughput-heavy experiments. The remaining work is to validate a non-zero
  Wi‑Fi duty-cycle on a real wardriving run rather than treat it as a fallback-only choice.

## Accepted, not a bug — do not "fix"

- Pairing X25519 is unauthenticated by design ([DECISIONS.md](DECISIONS.md)).
- The Flipper cannot be a BLE central/GATT client (standalone FAP ABI constraint).
- The 30-second idle disconnect is specified behavior; only a heartbeat *redesign* (above) is
  backlogged, not the current mechanism itself.
- `capability_query`'s `requested` field is intentionally unimplemented (full registry only).
- GPS is a fixed-coordinate stub until hardware-verified (real driver + wardriving fix-dependency
  implemented and build/host-test-verified 2026-09-12, hardware verification pending — see
  PLAN.md's "Real GPS driver, wardriving fix-dependency, and real wardriving-record timestamps").
  Discarding captures made without a real fix is specified behavior.
- Old on-flash wardriving records failing to decode (and being silently skipped) once the new
  mandatory `utc_timestamp_s` field ships is an accepted one-time cost of that format upgrade, not
  a bug — the existing decode-failure path already handles it safely, and the circular log
  self-heals as it rotates. Accepted by the user 2026-09-12; see PLAN.md's GPS section.
- `pairing_crypto.c`'s X25519 ladder is not constant-time (`mbedtls_mpi_mod_mpi()`). Accepted for
  the current threat model — physical possession of either device is already fully compromising.
- Step 4's radio-coexistence sweep is not trustworthy evidence for a wardriving duty-cycle
  default (it used synthetic load and missed the real starvation bug found later) — don't cite it
  to justify reverting `ble_interval_ms`/`ble_window_ms`'s current defaults.
- Do not flash, erase, or write either physical board while acting on anything in this file
  unless the user explicitly asks.

## Roadmap-gated (belongs to a specific PLAN.md step — do not steal into an unrelated PR)

- **Step 8:** ESP32 NVS pairing-blob version/validity-marker/atomicity (G13, finding #17);
  Flipper-side pairing/capability persistence hardening; explicit local unpair/factory-reset
  behavior definition.
- **Step 9:** full negative-security-test suite; real-negotiated-MTU exercise (see "Other open
  items" above); re-confirming step 4's coexistence bounds under live authenticated wardriving
  traffic (partially done — see SESSION_MEMORY.md's current-state section for what's left).

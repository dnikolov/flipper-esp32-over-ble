# Backlog

The single, centralized list of every open, actionable item that isn't part of the current
roadmap step's own scope: code defects, robustness gaps, deferred product decisions, and
cost/efficiency work. This is the "check before starting related work" list for anything not
already blocking the phase in progress.

- **Current in-flight state** (what's being worked on right now) lives in
  [SESSION_MEMORY.md](SESSION_MEMORY.md), not here.
- **Roadmap steps and their "done when" bars** live in [PLAN.md](PLAN.md), not here.
- **Finished work** (bug found, root-caused, fixed, verified) belongs in
  [BACKLOG_COMPLETED.md](BACKLOG_COMPLETED.md) (a scannable one-line-per-item archive) and
  [PROJECT_HISTORY.md](PROJECT_HISTORY.md) (the full narrative) — mark an item `DONE
  YYYY-MM-DD (commit)` below only long enough for the next session to notice, then move the
  row to BACKLOG_COMPLETED.md once the PROJECT_HISTORY.md entry exists.

Each row is a one-line pointer, not the full write-up — follow the link for exact file/line
evidence, a suggested fix, and tests to add. Do not re-derive detail that already lives
elsewhere:

- **G-numbered items** → [grok-4.6-findings-2026-09-11.md](grok-4.6-findings-2026-09-11.md)
  (evidence, exact fix, tests, dependencies).
- **BL-numbered items** → detail inline below (no separate appendix exists for these).
- **Roadmap-gated items** → the named `PLAN.md` step. Do not fix them out of order or sneak
  them into an unrelated PR — see `CLAUDE.md`'s conventions.
- **H-numbered items** → [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md). Deeper structural/robustness
  issues found during live testing that need their own investigation/design pass before being
  fixed, as distinct from the ready-to-fix bugs and deferred product decisions here.

Severity scale (matches the grok appendix): **P0** spec/security correctness · **P1** real bug
in normal use · **P2** robustness/defense-in-depth/cost · **P3** style/docs drift.

## P0 — correctness / security

| ID | Title | Status |
| --- | --- | --- |
| G03 | ESP32 marks the session `AUTHENTICATED` on its own GATT write-complete, not on peer confirmation | Open — needs a product decision on the `capability_query`-caveat (see appendix) |
| G06 | Neither firmware sends the spec-mandated `unsupported_version` error + close | Open |
| G07 | Any `send_protected*` clobbers an in-flight wardriving backlog drain (generalizes past `start`) | Implemented 2026-09-14 with a bounded ESP32 protected-TX FIFO; hardware reconnect/wardriving retest pending. See [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H03. |
| G09 | Flipper advances `session_seq_out` even when notify delivery is unknown | Open |
| BL01 | Flipper's `handle_pair_init()` runs unconditionally on any incoming `pair_init` — no local user-gesture/authorization-state check, contradicting [PAIRING.md](PAIRING.md) step 3's "user selects Add ESP32 board" | Open — low severity, works in practice; needs design clarification |

## P1 — real bugs in normal use

| ID | Title | Status |
| --- | --- | --- |
| G08 | SD-card I/O (pairing, capability cache, CSV) runs on `BleEventWorker`, the BLE-pump thread | Open |
| G10 | Flipper `session_key`/`session_seq_out`/`outgoing_message_id` accessed from two threads with no lock | Open |
| G13 | ESP32 NVS pairing blob has no version, validity marker, or atomic replacement | **Roadmap-gated → PLAN.md step 8.** Do not fix as a drive-by. |
| BL05 | Flipper reboots with `furi_check_failed` on app relaunch after wardriving | Open |
| BL06 | Flipper does not reconnect when coming back in range of ESP32 while on wardriving screen during wardriving | Open |
| G36 | Wardriving BLE reconnect can stall permanently | **Partially explained, not fully resolved.** BLE-only isolation test (7/7 successful reconnects) confirmed Wi-Fi coexistence starvation is *a* cause. But a live retest 2026-09-13 (after removing BL07's throttle) found a stall with a *different* mechanism — `wardriving_ble_interval_cb()`'s periodic re-arm colliding with its own in-flight connect attempt, independent of Wi-Fi entirely. See [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H01 for the full evidence and proposed fix. Do not treat this as closed. |
| BL04 | Wardriving CSV dedup table resets on disconnect, but file lifetime is per-calendar-day — same-day reconnect can re-log an address already written earlier that day | Open — lower priority (correctness is preserved, just allows edge-case duplicate rows within a day); candidate fix is to seed dedup table from existing file on reopen. |

## P2 — robustness / cost / defense-in-depth

| ID | Title | Status |
| --- | --- | --- |
| G18 | Flipper X25519 donna static ladder scratch (~3-4 KB) never zeroized, resident for the app's lifetime | Open |
| G23 | Flipper reassembly-complete buffer read after mutex release; `profile_start()` resets it unlocked | Open |
| G25 | 256-byte stack buffer in the ESP32's NimBLE notify-RX path (same class as 4 prior stack-overflow bugs) | Open |
| BL07 | ESP32 status LED does not turn green when Flipper connects during wardriving session and starts flushing ESP backlog | Open |
| BL09 | Filter the paired Flipper's BLE address out of scan and wardriving results | Open — define whether filtering applies to dedicated scans, wardriving capture, or both |

## Codebase & agent cost-efficiency

- **Extract a capability-dispatch layer** from both `main.c`/`flipper_esp32_over_ble.c` — pays
  off on every future capability. Hold until a natural roadmap boundary; decide the
  static-buffer-arena question first (see
  [LESSONS.md#static-buffer-pattern-trades-ram-for-stack-safety](LESSONS.md#static-buffer-pattern-trades-ram-for-stack-safety)).
- Still open: a **runtime active/passive BLE scanning toggle** (nothing today can request
  passive scanning — a prerequisite for [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s "Scan" menu's
  two passive modes), and measuring the real-world name-discovery improvement once
  hardware-tested. (Active scanning itself is already enabled — see BACKLOG_COMPLETED.md.)
- Do **not** split `flipper/pairing_crypto.c` (kept diffable against upstream curve25519-donna
  for auditability) or `tests/vectors/vectors.h` (98KB, generated — never `Read` it whole).
- `tests/flipper/build.ps1` fails out-of-the-box on a machine where Visual Studio's
  `vcvars64.bat` shells out to `vswhere.exe` by bare name and the VS Installer directory isn't
  already on `PATH` (surfaced 2026-09-12 while verifying the LED-indicator feature). Needs a
  one-line `PATH` prepend in that script; not yet fixed.

## Other open items (not covered by the cross-model review)

- **BL08 — Home screen space optimization:** consolidate display by showing "Pairing:Y/N" instead of "saved pairing" and "ESP:waiting"/"ESP:session" instead of "Waiting for ESP"; move all current status data to settings screen in a scrollable view for full visibility.
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
- Follow-on items deliberately left backlogged from the real-GPS-driver design (implemented and
  hardware-verified 2026-09-13 — see [PLAN.md](PLAN.md)'s "Real GPS driver..." section):
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
    hardcoded `"0,0"` — GGA `altitude_dm` exists in `feb_location_t` and the `gps` capability's
    own `altitude_dm_offset` result field, but wardriving records carry no altitude field of
    their own yet, so the CSV exporter has nothing to read; `AccuracyMeters` has no real source
    at all (the GPS module reports HDOP, not a meters-based error estimate). Not folded into the
    GPS-capability addition since it requires a wardriving-record wire-format change on both
    firmwares.
  - **Per-board wardriving autostart setting** — add persistent board configuration so
    wardriving can start autonomously after ESP32 boot, independent of the Flipper initiating
    the session; define the Flipper settings UI and get/set wire surface, boot-time interaction
    with GPS fix availability, and how an active capture is stopped or disabled. Unscoped.
  - **Persistent GPS GNSS configuration setting** — expose GPS constellation selection (GPS,
    GLONASS, BeiDou) and related receiver configuration through board settings; define the
    Flipper settings UI, authenticated get/set wire surface, ESP32 persistence, and safe
    one-time UBX provisioning without sending configuration commands on every boot. Unscoped.
  - Runtime-configurable GPS UART GPIO pins via a Flipper Settings screen — deliberately split
    out of the frozen design (see PLAN.md's "Scope boundary" note) because it needs a
    form/pin-entry widget this project doesn't have yet and a new get/set wire config surface.
    Defaults stay compile-time constants for now.
- Non-ASCII SSID rendering is untested on real hardware (host-native codec tests cover the
  encoding; no such network was available during `wifi_scan` verification). Not a blocker.
- Adopt a real `ViewDispatcher`/scene-manager architecture on the Flipper FAP instead of the
  single-`ViewPort`/`AppEvent`-queue pattern every screen has been bolted onto. Structural,
  no deadline, not a blocker for anything currently in flight (the Phase 3a menu redesign
  shipped directly on the existing pattern instead — see [docs/UI_REDESIGN.md](UI_REDESIGN.md)).
- The Flipper's "Scan" menu screen is only a placeholder-level Wi-Fi-scan/BLE-scan picker, not
  [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s actual five-mode BLE-active/passive live-view design
  (reusing Wardriving's capture engine without persistence). Needs its own implementation pass
  once the runtime BLE active/passive toggle above exists.
- Decide whether `AppScreenLegacy`/`HomeMenuLegacy` (a compatibility screen preserving the old
  direct-button-shortcut flow, found during the Phase 3a implementation but never part of
  [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s original design) is kept long-term or removed once
  Scan/GPS/Settings/About are trusted to fully replace it.
- **Cosmetic, needs a hardware/visual check:** the Home menu's "Connection lost" banner and each
  non-Home screen's own title may visually overlap — both are drawn at nearly the same canvas
  position (banner at y=12 `FontSecondary`, titles at y=11 `FontPrimary`). Found while reading
  `draw_callback`; not confirmed on a real screen.
- **Consolidating/grouping wardriving records on the Flipper side** (e.g. de-duplicating or
  rolling up repeated/nearby sightings for display, as distinct from the ESP32-side capture-time
  dedup that already exists). Not scoped yet — needs its own planning/grill-me session before
  implementation, not a drive-by design call.
- **Reconsider the RSSI-improve dedup gate's comparison basis: last-written vs. best-ever.**
  Both `esp32/main/wardriving_dedup.c`'s `should_log_record()` and
  `flipper/wardriving_csv.c`'s `feb_wardriving_dedup_should_write()` re-trigger a write when RSSI
  improves ≥6dB versus the *last-written* observation for that address — which lets a signal that
  is merely fluctuating (not trending stronger), especially BLE's noisier RSSI, repeatedly clear
  the gate. Investigated against WiGLE's own reference Android app
  (`wigle-wifi-wardriving`, `DatabaseHelper.java`'s `addObservation()`): its gate is a hybrid — a
  64-entry in-memory LRU cache makes it compare against last-written for addresses still warm in
  cache (same behavior as this project), falling back to the DB's true best-ever
  `network.bestlevel` column only on a cache miss. Worth deciding whether to switch to (or
  approximate) a best-ever comparison here. Related to the "Consolidating/grouping" item above
  but narrower. Not scoped/decided yet.
- **Explore throttling the Wi-Fi source specifically during wardriving results-flush to the
  Flipper** — the flush was observed getting stuck before the recent dedup/sequence-cap fixes
  (`e616d81`, `3111fa2`). Possibly the same BLE/Wi-Fi coexistence starvation as G36, just
  triggered by the drain instead of by a reconnect; not yet isolated whether it still reproduces
  post-fix. Related to WiFi-source duty cycle optimization below, but narrower — only during the
  flush window, not a general default change.
- **Optimize WiFi scan interval beyond 5 seconds** (2026-09-13) — current validated production
  default is `wifi_interval_ms=5000` (5 seconds, ~12 scans/minute), set after research confirmed
  ESP32 WiFi scans take ~1.4-2 seconds per full 2.4 GHz channel sweep (per ESP-IDF WiFi driver
  documentation). Further optimization to 2-3 second intervals (`wifi_interval_ms=2000` or
  `wifi_interval_ms=3000`) is candidate for future testing to improve wardriving capture density
  while maintaining BLE stability. **Research baseline:** Full 2.4 GHz WiFi scan baseline is
  ~2040ms; optimized channel timing (85ms active for channels 1-11, 255ms passive for 12-13)
  reduces this to ~1445ms, achieving 0.69 Hz scan frequency (see ESP-IDF WiFi driver docs and
  WiFi performance analysis). **Test plan:** Run extended wardriving sessions at 2s, 3s, and 5s
  intervals; measure: (1) BLE reconnect latency if connection drops, (2) backlog drain reliability,
  (3) WiFi capture density (networks/minute), (4) subjective coverage quality. Document tradeoffs
  and settle on production default accordingly.

## Deferred by explicit product decision — confirm with the user before touching

- **Idle-connection heartbeat/keep-alive redesign.** The 30s idle-timeout disconnect/reconnect
  cycle works but causes a cosmetic LED/screen flicker. This is a wire-protocol change (both
  firmwares) needing its own design session — backlogged at the user's explicit request, not a
  quick patch.
- **G07** (TX single-flight clobbers wardriving drain) — deferral lifted 2026-09-14; a bounded
  ESP32 protected-TX FIFO now serializes backlog, handshake, capability, status, and error
  records. Hardware reconnect/wardriving retest remains before closing H03.

## Accepted, not a bug — do not "fix"

- Pairing X25519 is unauthenticated by design ([DECISIONS.md](DECISIONS.md)).
- The Flipper cannot be a BLE central/GATT client (standalone FAP ABI constraint).
- The 30-second idle disconnect is specified behavior; only a heartbeat *redesign* (above) is
  backlogged, not the current mechanism itself.
- `capability_query`'s `requested` field is intentionally unimplemented (full registry only).
- GPS is real UART/NMEA driver, hardware-verified 2026-09-13 — see PLAN.md's "Real GPS driver, wardriving fix-dependency, and real wardriving-record timestamps". Discarding captures made without a real fix is specified behavior.
- Old on-flash wardriving records failing to decode (and being silently skipped) once the new
  mandatory `utc_timestamp_s` field ships is an accepted one-time cost of that format upgrade, not
  a bug — the decode-failure path handles it safely, and the circular log self-heals as it rotates (see commit b23aec0: stale record flags are now cleared on boot/replay instead of appearing as stuck backlog). Accepted by the user 2026-09-12; see PLAN.md's GPS section.
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

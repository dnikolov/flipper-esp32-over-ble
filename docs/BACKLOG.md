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
| G04 | Pairing ceremony (`pair_init`→`pair_complete`) has no application-level timeout | Open |
| G05 | Absolute `uint32_t` millisecond deadlines wrap at ~49.7 days uptime | Open |
| G06 | Neither firmware sends the spec-mandated `unsupported_version` error + close | Open |
| G07 | Any `send_protected*` clobbers an in-flight wardriving backlog drain (generalizes past `start`) | Open — **deferred at explicit user request**; re-confirm before implementing (see "Deferred" below) |
| G09 | Flipper advances `session_seq_out` even when notify delivery is unknown | Open |
| G11 | Flipper never closes the connection on auth/GCM/sequence failure (relies on ESP32's 30s idle timeout) | Open |
| BL01 | Flipper's `handle_pair_init()` runs unconditionally on any incoming `pair_init` — no local user-gesture/authorization-state check, contradicting [PAIRING.md](PAIRING.md) step 3's "user selects Add ESP32 board" | Open — needs a decision: implement the gate, or correct PAIRING.md if none was intended |

`G26` (AES-GCM 24-bit sequence cap not enforced) — **DONE 2026-09-11** (`3111fa2`); see
[PROJECT_HISTORY.md](PROJECT_HISTORY.md).

## P1 — real bugs in normal use

| ID | Title | Status |
| --- | --- | --- |
| G08 | SD-card I/O (pairing, capability cache, CSV) runs on `BleEventWorker`, the BLE-pump thread | Open |
| G10 | Flipper `session_key`/`session_seq_out`/`outgoing_message_id` accessed from two threads with no lock | Open |
| G14 | `any_saved_pairing_exists()` matches any directory entry, including a crashed-save `.dat.tmp` leftover | Open |
| G17 | `client_auth` proof failure leaves the Flipper UI stuck on "Authenticating…" | Open — pairs with G03/G11 |
| G27 | `pending_command_kind` is never cleared; a stray `internal_error` always looks like a wardriving self-stop | Open |
| G28 | Wardriving CSV writes a WiGLE header on every `FSOM_OPEN_APPEND`, not only on a genuinely new file | Open |
| G30 | Wardriving log/dedup state has no lock between the Wi-Fi `sys_evt` writer and the NimBLE-host drain reader | Open |
| G31 | `backlog_remaining` uses an unlocked `size_t` subtract — can underflow under G30's race | Open |
| G13 | ESP32 NVS pairing blob has no version, validity marker, or atomic replacement | **Roadmap-gated → PLAN.md step 8.** Do not fix as a drive-by. |
| G36 | Wardriving BLE reconnect can stall permanently (discovery restarts every ~500ms, never matches) when wardriving's Wi-Fi source runs concurrently at its gapless default | Open — coexistence-starvation theory now well-supported (BLE-only isolation test: 7/7 disconnects recovered; earlier `wifi=1 ble=1` capture: stalled permanently), fix not yet designed. See `docs/PROJECT_HISTORY.md`'s "Wardriving reconnect stall" investigation (5 dated entries) and `docs/LESSONS.md`. |

## P2 — robustness / cost / defense-in-depth

| ID | Title | Status |
| --- | --- | --- |
| G12 | Flipper fragments every record at ATT MTU 23 (16-byte payload) even after MTU negotiation | Open — ~3-4x fewer BLE notifications per record if fixed |
| G15 | ESP32 HMAC `full[32]` scratch not zeroized after truncating to the 16-byte wire value | Open |
| G16 | Factory reset doesn't zeroize in-RAM `stored_pairing_secret` before `esp_restart()` | Open |
| G18 | Flipper X25519 donna static ladder scratch (~3-4 KB) never zeroized, resident for the app's lifetime | Open |
| G19 | Reconnect still `xTaskCreate(..., 3072)` just to sleep once, every ~30s during a prolonged outage | Open |
| G20 | `notify_data_callback`'s NULL-context path sets `*data_len = PAYLOAD_MAX` instead of `0` | Open — one-line fix |
| G21 | Pairing/capability/CSV path buffers sized at 96 bytes, one constant short of the real max (~137) | Open |
| G23 | Flipper reassembly-complete buffer read after mutex release; `profile_start()` resets it unlocked | Open |
| G24 | ESP32 built with `-Og`, not `-Os` | **Product choice, not a bug** — record in BASELINES.md if changed |
| G25 | 256-byte stack buffer in the ESP32's NimBLE notify-RX path (same class as 4 prior stack-overflow bugs) | Open |
| G32 | Factory-reset LED RMT channel leaks on partial init failure | Open |
| G33 | `board_id_len` takes `snprintf()`'s return value verbatim; `<stdio.h>` not directly included | Open |
| G34 | ESP32 `feb_gcm_encrypt` failure path uses `memset`, not `feb_secure_zero` | Open |

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
- **BLE active scanning**: enabled for `ble_scan` 2026-09-11 (`e92aad9`). Still open: a runtime
  on/off toggle, extending it to `wardriving`'s own capture engine, and measuring the real-world
  name-discovery improvement once hardware-tested.
- Do **not** split `flipper/pairing_crypto.c` (kept diffable against upstream curve25519-donna
  for auditability) or `tests/vectors/vectors.h` (98KB, generated — never `Read` it whole).

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
- Automatic BLE arbitration between multiple paired boards — gated on an unresolved BLE-HAL
  question: can the Flipper's peripheral role advertise while already connected?
- GPS backfill-to-first-fix as a Flipper-settable option, instead of discarding every
  pre-fix wardriving result outright (step 7/GPS capability).
- Non-ASCII SSID rendering is untested on real hardware (host-native codec tests cover the
  encoding; no such network was available during `wifi_scan` verification). Not a blocker.
- Adopt a real `ViewDispatcher`/scene-manager architecture on the Flipper FAP instead of the
  single-`ViewPort`/`AppEvent`-queue pattern every screen has been bolted onto. Structural,
  no deadline.

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
- **WiFi-source duty cycle** (`wifi_interval_ms` default `0`, continuous) is unvalidated with an
  active connection — suspected to compete for the same radio via IDF's coexistence arbiter, but
  never isolated the way step 4 isolated the BLE points. Needs its own coexistence check before
  picking a different default.

## Accepted, not a bug — do not "fix"

- Pairing X25519 is unauthenticated by design ([DECISIONS.md](DECISIONS.md)).
- The Flipper cannot be a BLE central/GATT client (standalone FAP ABI constraint).
- The 30-second idle disconnect is specified behavior; only a heartbeat *redesign* (above) is
  backlogged, not the current mechanism itself.
- `capability_query`'s `requested` field is intentionally unimplemented (full registry only).
- GPS is a fixed-coordinate stub; discarding pre-fix captures is specified behavior until real
  GPS lands.
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

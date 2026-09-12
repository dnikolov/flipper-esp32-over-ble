# Grok 4.6 code review findings (2026-09-11)

**At-a-glance status lives in [docs/BACKLOG.md](BACKLOG.md)** — that file is the single
centralized list of every open item across the project; this file is its detailed technical
appendix for the G-numbered items (exact evidence, fix sketch, tests, dependencies). Update the
`FIXED`/status marker in both places when an item closes.

Point-in-time review of both firmwares against the docs as they stood on 2026-09-11.
This is a **work ticket**, not a living status file. Cross items off here as they are
fixed; do not rewrite the descriptions. After a fix, leave a one-line `FIXED YYYY-MM-DD`
marker on that item.

Prior review: [CODE_REVIEW_FINDINGS.md](CODE_REVIEW_FINDINGS.md) (2026-09-07). That
review's "fix now" codec/crypto items (#1–#4, #9, #10) are **already fixed**. This
document does not re-litigate them. It confirms which older items are still live and
adds new findings from the now-live protected-record / wardriving path.

## How another model should use this document

1. Read [SESSION_MEMORY.md](SESSION_MEMORY.md) and [PROTOCOL.md](PROTOCOL.md) first.
2. Do **not** implement Phase 4/5 capabilities or reorder [PLAN.md](PLAN.md) steps.
3. Do **not** flash, erase, or write the physical board unless the user explicitly asks.
4. Prefer the smallest isolated fix that matches existing style (comment-sparse C,
   `feb_secure_zero` for secrets, static buffers on BLE-callback paths).
5. A wire-format or crypto-derivation change is not done until **both** firmwares
   implement it identically and both build. Shared-header edits: run
   `python tools/check_shared_headers.py`.
6. Host-test the touched module (`tests/esp32/`, `tests/flipper/`) after the change.
   Do not claim hardware-verified unless a real-device pass happened.
7. If a change alters on-screen text, buttons, pairing/reconnect/reset, or CSV
   behavior already described in [USER_GUIDE.md](USER_GUIDE.md), sync that guide in
   the same pass via the cheapest capable model (project convention: Haiku).
8. Finished narrative belongs in [PROJECT_HISTORY.md](PROJECT_HISTORY.md), not in
   PLAN.md / SESSION_MEMORY.md. After fixing an item here, add a one-line pointer
   in SESSION_MEMORY.md's "Known open items" only if the item was already listed
   there.

Each finding below has: ID, severity, status vs prior review, files, evidence,
impact, **exact suggested fix**, tests to add, and dependencies / non-goals.

Severity:

| Level | Meaning |
| --- | --- |
| P0 | Spec/security correctness; can desync sessions, reuse GCM nonces, or drop the only TX path |
| P1 | Real bug or contract gap that bites in normal use (pairing, wardriving drain, storage) |
| P2 | Robustness / defense-in-depth / long-run / embedded cost |
| P3 | Style, docs drift, low-probability, or already-accepted threat-model items |

---

## Project snapshot (for implementers who have not read the docs)

Flipper Zero FAP is BLE **peripheral / GATT server**. ESP32-C6-DevKitC-1-N4 firmware
is BLE **central / GATT client**. Pairing is reset-gated unauthenticated X25519 in a
trusted environment; runtime records are AES-256-GCM over canonical CBOR. Phase 2
(steps 1–7) is hardware-verified. Phase 3 wardriving is implemented on both sides and
mostly hardware-verified; a few acceptance items remain (see SESSION_MEMORY.md).

Pinned baselines (do not silently change): ESP-IDF v5.5.2, Unleashed `unlshd-092`
API 88.4, 4 MB C6 flash. Contract: [PROTOCOL.md](PROTOCOL.md).

---

## Status of 2026-09-07 findings (do not re-open FIXED items)

| Old ID | Title | Status 2026-09-11 |
| --- | --- | --- |
| #1 | Flipper CBOR map OOB read | FIXED |
| #2 | `feb_cbor_skip_value` type-set drift | FIXED |
| #3 | Nesting depth drift | FIXED |
| #4 | Over-length `board_id` clamp vs zero | FIXED |
| #5 | Framing flags / mid-fragment capacity | G01 (Flipper flags check) **FIXED 2026-09-12**; G02 (ESP32 mid-fragment capacity) status tracked separately by the ESP32 agent |
| #6 | `client_auth` AUTHENTICATED on local write-complete | **STILL OPEN** → G03 |
| #7 | No pairing-ceremony timeout | **STILL OPEN** → G04 |
| #8 | `uint32_t` ms wrap on absolute deadlines | **STILL OPEN** → G05 |
| #9 | Flipper GCM failure zeroize | FIXED |
| #10 | Trailing bytes | FIXED |
| #11 | X25519 static ladder unzeroized | **STILL OPEN** → G18 |
| #12 | Flipper TX always 16-byte fragments | **STILL OPEN** → G12 |
| #13 | 3 KB reconnect tasks | **STILL OPEN** → G19 |
| #14 | ESP32 `-Og` | **STILL OPEN** → G24 (product choice) |
| #15 | `strlen` in ESP32 field loops | not re-verified; low value |
| #16 | ESP32 RX 256-byte stack buffer | **STILL OPEN** → G25 |
| #17 | NVS pairing blob has no version/validity | **STILL OPEN** → G13 |
| #18 | Pairing I/O on BLE thread | **STILL OPEN, worse** → G08 |
| #19 | Flipper TX ignores notify result | **STILL OPEN, raised** → G09 |
| #20 | `notify_data_callback` NULL `data_len` | **STILL OPEN** → G20 |
| #21 | Path buffers sized 96 | **STILL OPEN** → G21 |
| #22 | `any_saved_pairing_exists` any dirent | **STILL OPEN** → G14 |
| #23 | Protected-record path dead | **OBSOLETE** — path is live |
| #24 | HMAC `full[32]` / factory-reset RAM secret | **STILL OPEN** → G15, G16 |
| SESSION | `unsupported_version` never sent | **STILL OPEN** → G06 |
| SESSION | `start` clobbers backlog drain | **STILL OPEN, generalized** → G07 |
| SESSION | Flipper session_key/seq unsynchronized | **STILL OPEN** → G10 |
| SESSION | X25519 ladder not constant-time | accepted threat model; out of scope |
| SESSION | Idle 30s flicker | by design; backlog heartbeat; not a bug |

---

## P0 — correctness / security

### G01 — Flipper does not reject nonzero fragment `flags`

- **Severity:** P0 (contract) / practical P2 until flags gain meaning
- **Status:** **FIXED 2026-09-12** — see `docs/PROJECT_HISTORY.md`. `feb_reassembly_feed()` now
  rejects `header.flags != 0` with `FEB_FRAME_INVALID_HEADER`, matching the ESP32 side; test
  case added to `tests/flipper/test_flipper_codec.c` (`NONZERO_FLAGS_FRAG0`).
- **Files:** [flipper/framing.c](../flipper/framing.c) (~L116–L123). ESP32 already rejects at [esp32/main/framing.c](../esp32/main/framing.c#L104).
- **Evidence:** Flipper parses `header.flags = fragment[0]` then only checks `fragment_count == 0` / `fragment_index >= fragment_count`. PROTOCOL.md: `flags` reserved, all bits 0.
- **Impact:** Asymmetric acceptance. A future flag bit, or a hostile peer, is ignored on Flipper and rejected on ESP32.
- **Fix:**
  1. In Flipper `feb_reassembly_feed`, after parsing the header: if `header.flags != 0`, `feb_reassembly_reset(r); return FEB_FRAME_INVALID_HEADER;`.
  2. Add the same case to both `tests/esp32/` and `tests/flipper/` framing suites (nonzero flags → `FEB_FRAME_INVALID_HEADER`).
- **Do not:** invent flag semantics.
- **Tests:** shared malformed-fragment vector with `flags=0x01`.

### G02 — ESP32 never enforces mid-message fragment payload ≤ fragment-0 capacity

- **Severity:** P0 (contract) / practical P2
- **Status:** **FIXED 2026-09-12** — see `docs/PROJECT_HISTORY.md`. `feb_reassembly_feed()` now
  checks `payload_len > r->fragment_payload_capacity` before the total-size check, returning
  `FEB_FRAME_OVERSIZED`, matching the Flipper side; regression test added to
  `tests/esp32/test_framing_cbor.c`.
- **Files:** [esp32/main/framing.c](../esp32/main/framing.c) — `fragment_payload_capacity` is stored on fragment 0 and **never read again**. Flipper checks `payload_len > r->fragment_payload_capacity` at [flipper/framing.c](../flipper/framing.c#L152).
- **Impact:** ESP32 can accept a mid-fragment larger than the capacity established by fragment 0 until the 768-byte total cap hits. Status-code ordering also still differs (inconsistent-count vs duplicate).
- **Fix:**
  1. After the existing `fragment_index != next_expected` checks, add Flipper's capacity check: if `payload_len > r->fragment_payload_capacity`, reset, return `FEB_FRAME_OVERSIZED`. Last fragment may be shorter; it must not be longer.
  2. Optionally converge check order with Flipper (inconsistent-count before duplicate) so the same malformed input yields the same `feb_frame_status_t`. Pin that order in both test suites.
- **Tests:** fragment 0 payload 16, fragment 1 payload 17 → oversized on **both** sides.

### G03 — ESP32 marks the session AUTHENTICATED when `client_auth` GATT write completes

- **Severity:** P0
- **Status:** Known #6, still open
- **Files:** [esp32/main/main.c](../esp32/main/main.c) `TX_DONE_RUNTIME_AUTHENTICATED` (~L2637–L2645). Flipper proof-fail path [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) `handle_client_auth` (~L2238–L2241) sends **no reply** and does **not** disconnect (see G11).
- **Evidence:**
  ```c
  case TX_DONE_RUNTIME_AUTHENTICATED:
      runtime_auth_state = RUNTIME_AUTH_STATE_AUTHENTICATED;
      ...
      wardriving_maybe_kick_send(conn_handle);
  ```
- **Impact:** On Flipper proof failure (desynced secret / impersonation), ESP32 logs "runtime session authenticated", starts unsolicited wardriving drain, and only recovers via the 30s idle timeout. PROTOCOL.md "Runtime auth failure handling" + "Cryptographic requirements": verifying peer closes without reply; ESP32 has no confirmation wait.
- **Fix (ESP32 only, no wire change):**
  1. Add `RUNTIME_AUTH_STATE_CLIENT_AUTH_SENT`.
  2. On `TX_DONE_RUNTIME_AUTHENTICATED`, enter that state, **do not** kick wardriving, **do not** accept `command`.
  3. Enter `AUTHENTICATED` only on the first successfully decrypted protected record with `sequence == 1` (today: Flipper `capability_query` after auth, or nothing if capability file is cached — **see caveat**).
  4. Arm a deadline (reuse `FEB_HELLO_ACK_TIMEOUT_MS` or a new named constant) from `client_auth` send; on expiry call `fail_runtime_auth`.
- **Caveat / protocol gap:** If Flipper skips `capability_query` because a capability file is cached, there may be **no** Flipper→ESP32 protected record after auth. Then ESP32 would sit in `CLIENT_AUTH_SENT` until timeout. Implementers must pick one:
  - **A (preferred, small):** Flipper always sends a protected record after auth (existing `capability_query`, or a tiny keepalive). Document in PROTOCOL.md.
  - **B:** Treat first **inbound or outbound** protected success plus Flipper staying connected N ms as enough — weaker.
  - **C:** Add an explicit `session_ready` ack to PROTOCOL.md (wire change; both sides; do not do this unless the user agrees).
- **Do not:** send a pairing window on proof failure.
- **Tests:** host-level state-machine test if one exists; otherwise a comment + code review of the new state. Hardware: Flipper with wrong secret should not cause ESP32 "authenticated" log.

### G04 — Pairing ceremony has no application timeout

- **Severity:** P1 (listed P0-adjacent in prior review; not a crypto break)
- **Status:** Known #7, still open
- **Files:** [esp32/main/main.c](../esp32/main/main.c) — `hello_ack_deadline_ms` exists; `TX_DONE_AWAIT_PAIR_REPLY` only logs. No `pair_reply` / `pair_confirm` deadline.
- **Impact:** Flipper connects during the 120s window and never answers `pair_init` → ESP32 holds the link until BLE supervision timeout.
- **Fix:** Mirror hello_ack: set `pair_reply_deadline_ms` when `pair_init` write completes; in `reassembly_timeout_cb`, if `PAIRING_STATE_INIT_SENT` and expired, `fail_pairing_ceremony` + terminate. Same for confirm if there is a wait after `pair_reply`. Use wrap-safe elapsed compare (G05).
- **Tests:** not easily host-tested; keep timeout in the same callback as hello_ack so the pattern is reviewable.

### G05 — Absolute `uint32_t` ms deadlines wrap at ~49.7 days

- **Severity:** P1 for unattended wardriving uptime (project's own long-run scenario)
- **Status:** **FIXED 2026-09-12** — see `docs/PROJECT_HISTORY.md`. `pairing_window_deadline_ms`
  and `hello_ack_deadline_ms` renamed to `pairing_window_start_ms`/`hello_ack_start_ms` and both
  comparisons converted to the wrap-safe elapsed-time form, matching the existing idle-timeout
  pattern. G04's new pairing-ceremony timeout can build on this once implemented.
- **Files:** [esp32/main/main.c](../esp32/main/main.c)
  - `pairing_window_is_open` (~L522): `now_ms >= pairing_window_deadline_ms` **unsafe**
  - `reassembly_timeout_cb` (~L3087): `now_ms >= hello_ack_deadline_ms` **unsafe**
  - Idle path (~L3093) already uses wrap-safe `(uint32_t)(now_ms - last_record_activity_ms)`
- **Impact:** After wrap, a freshly opened pairing window (including `unknown_board` fallback) can look already expired. hello_ack timeout can fire immediately or never.
- **Fix:** Store `start_ms` + duration, compare elapsed:
  ```c
  if ((uint32_t)(now_ms - pairing_window_start_ms) >= FEB_PAIRING_WINDOW_MS)
  ```
  Same for hello_ack / any new pairing-ceremony deadline (G04) / client_auth wait (G03). Do not mix absolute and elapsed forms.
- **Tests:** unit-test the helper with `start_ms = UINT32_MAX - 1000`, `now_ms = 500`.

### G06 — Neither firmware sends `unsupported_version`

- **Severity:** P1 (explicit PROTOCOL.md requirement)
- **Status:** SESSION_MEMORY, still open
- **Files:**
  - ESP32 [main.c](../esp32/main/main.c) pairing envelope `version != 2` → generic pairing fail (~L2905–L2910); session path similar.
  - Flipper [flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) (~L2337–L2365) logs and drops.
- **PROTOCOL:** respond with unencrypted `error` `code=unsupported_version`, then close.
- **Fix (both sides, same behavior):**
  1. After a **successful** envelope decode with `version != 2` (do not conflate with invalid `board_id`), encode `error` with that code on the **same envelope shape** that arrived (4-field pairing vs 5-field session).
  2. Send it, then disconnect. Flipper: do not `bt_disconnect` from inside `profile_event_handler` (existing reentrancy rule) — post an `AppEvent` to the main loop that sends then tears down.
  3. Protected-record `version != 2` after auth: PROTOCOL groups this with auth failure (silent close, no reply). Document that split in PROTOCOL.md so the two cases stay distinct.
- **Tests:** codec already encodes `error`; add a dispatch test or a documented manual case. Update PROTOCOL.md only if you discover the protected-record case is underspecified — that is a doc clarification, not a new code.

### G07 — Single-flight TX is not exclusive: any `send_protected` clobbers wardriving drain

- **Severity:** P0 for live sessions
- **Status:** Known (SESSION_MEMORY, deferred at user request for `start` only). **Generalized here:** not just `start`.
- **Files:** [esp32/main/main.c](../esp32/main/main.c)
  - `queue_encoded_record_for_tx` (~L860) always rebuilds `tx_fragment_*`
  - `queue_and_send_protected` (~L958) always sets `tx_done_action` and increments `rt_tx_sequence`
  - `wardriving_maybe_kick_send` (~L1845) is the only caller that respects `wardriving_tx_in_flight`
  - `handle_wardriving_command` start (~L2246) / stop (~L2088) call `send_protected` with `TX_DONE_NONE` without checking in-flight drain
  - `handle_capability_query` uses the same send path; Flipper often sends `capability_query` immediately after auth, which is also when drain starts (`TX_DONE_RUNTIME_AUTHENTICATED`)
- **Impact:** Drain's continuation is replaced. `wardriving_tx_in_flight` can remain true with `tx_done_action` no longer `TX_DONE_CONTINUE_WARDRIVING` → no further outbound wardriving until idle disconnect (~30s). No flash data loss (undrained records resend next session). Sequence still advances for the clobbering record. If fragments mix, the peer can see a torn message.
- **Fix (ESP32, no wire change):**
  1. Introduce one TX owner: `tx_busy` if `tx_fragment_next < tx_fragment_total` **or** `wardriving_tx_in_flight`.
  2. `queue_and_send_protected` / `send_protected` / `send_protected_error`: if busy, **do not** rebuild fragments. Either:
     - **Queue** one pending app record (enough for `started`/`stopped`/`capability_response`/`error`), or
     - Return failure and have `handle_wardriving_command` reply `busy` for `start` while drain is in flight (product choice — `busy` already exists).
  3. Preferred: a 1-slot pending record + `tx_done_action` chain: finish current drain record → send pending `started` → resume drain via `wardriving_maybe_kick_send`.
  4. Never leave `wardriving_tx_in_flight == true` after overwriting `tx_done_action`.
- **Do not:** start a second GATT write stream; NimBLE is single-in-flight by design.
- **Tests:** host test is hard (needs fake GAP). Add a focused comment + a state diagram in the function comment. Hardware: reconnect with a full log, immediately tap Start — drain must continue after `started`.
- **Note:** User previously deferred the `start`-only form. This finding documents that **capability_query and stop** have the same bug. Confirm with the user before implementing if they still want this deferred.

### G09 — Flipper advances `session_seq_out` even when notify delivery is unknown

- **Severity:** P0 once a send fails
- **Status:** Known #19, severity raised because protected seq is live
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c)
  - `emit_fragment` (~L297) discards `ble_gatt_characteristic_update` result
  - `send_pairing_record` (~L304) returns `fragment_count != 0` (fragmentation succeeded, not delivery)
  - `send_wifi_scan_command` (~L1948–L1953) increments `session_seq_out` after that
- **Impact:** ESP32 never sees the record; next Flipper record uses `seq+1` → ESP32 "sequence mismatch; closing" (ESP32 **does** terminate on seq mismatch). Session dies. Also applies to `capability_query` / wardriving commands.
- **Fix:**
  1. `emit_fragment` returns bool from `ble_gatt_characteristic_update`.
  2. `feb_fragment_record` cannot currently abort mid-loop on emit failure — either change emit to report failure and stop (shared `framing.c` on **both** sides, header contract), or check after each notify inside a Flipper-only send helper that does not use the shared emit-can't-fail assumption.
  3. If any fragment fails: do **not** increment `session_seq_out` / treat the send as failed; surface UI error. Do not leave a half-sent multi-fragment record without resetting.
- **Caution:** changing `feb_emit_fn` is a shared-contract change — both `framing.c` copies and both tests.
- **Tests:** Flipper host tests stub notify; force false and assert seq unchanged.

### G11 — Flipper does not close the connection on auth / GCM / sequence failure

- **Severity:** P0 vs PROTOCOL.md letter; Flipper comments already admit they rely on ESP32 idle timeout because `bt_disconnect` from `profile_event_handler` is considered unsafe
- **Status:** NEW emphasis (no-reply half was intentional; **close** half is missing)
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c)
  - `handle_client_auth` proof fail (~L2238): `session_reset_state(); return;` — no UI phase, no disconnect
  - Protected decrypt fail (~L2394): drop, keep link, **seq not incremented** (good) but connection stays
  - Seq/session mismatch (~L2410): drop, keep link; if this was a **replay of the current seq**, dropping without close leaves the session able to accept the real next record — if it was a **gap**, Flipper still waits for the missing seq forever
- **PROTOCOL:** "Authentication failure, an unexpected sequence, nonce reuse, or replay is fatal: discard the record and close the BLE connection without replying."
- **Fix:**
  1. Keep **no wire reply**.
  2. Post `AppEvent` (new or reuse pairing-failed) to the **main thread** to disconnect / restore profile. Do not call `bt_disconnect` inside the GATT handler.
  3. On proof fail, also `post_pairing_phase(..., Failed, ...)` so UI does not stick on "Authenticating…" (G17).
  4. On GCM/seq fail after `SessionStageActive`, reset session state **and** disconnect.
- **Tests:** cannot fully host-test BLE teardown; assert the event is posted in a reviewable way (flag/function).

### G26 — AES-GCM 24-bit sequence is not enforced by either application

- **Severity:** P0 at 2^24−1 protected records; P2 in current session lengths
- **Status:** NEW (header already warns callers; nobody checks)
- **Files:** [esp32/main/session.c](../esp32/main/session.c) `feb_session_build_nonce` masks low 24 bits; [esp32/main/main.c](../esp32/main/main.c) `rt_tx_sequence++` unbounded; Flipper `session_seq_out++` / `session_seq_in++` unbounded. [session.h](../esp32/main/session.h#L149) says the function does not enforce the cap.
- **PROTOCOL:** "must never wrap. A new BLE session is required before 2^24−1 protected records."
- **Impact:** Nonce reuse under the same key is a GCM catastrophic failure. Wardriving can emit many protected status records per session.
- **Fix (both sides, same threshold):**
  - Before encrypt: if `seq > 0xFFFFFF` (or `>= 0xFFFFFF` depending on whether 2^24−1 is last legal), refuse encrypt, terminate connection, do not wrap.
  - After decrypt: if incoming seq is at the cap, accept that one record then disconnect (or reject and disconnect — pick one and document).
  - Do this in the **application** (`queue_and_send_protected` / Flipper send helpers), not by silently changing `feb_session_build_nonce` without a caller-visible error.
- **Tests:** host test encrypt with `sequence = 0x1000000` returns 0.

---

## P1 — real bugs in normal use

### G08 — SD-card I/O on `BleEventWorker` (pairing, capability cache, CSV)

- **Severity:** P1
- **Status:** Known #18, scope larger (CSV every data batch; capability files)
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) `pairing_storage_*`, `capability_storage_*`, `handle_wardriving_status` → `wardriving_csv_write_record` (open/write/sync on BLE thread; comments admit this).
- **Impact:** Blocks the thread that pumps BLE. Long SD stalls → missed fragments, reassembly timeout, disconnects. Recurring class in this project.
- **Fix:** BLE thread posts completed decrypted records; main loop or a storage worker does `storage_file_*`. Pairing persist: finish crypto in RAM, then async save; do not advertise "Paired" until save completes if durability matters. CSV: queue rows (bounded) to main; if the queue would block BLE, drop-with-log is worse than a small static ring of encoded rows.
- **Do not:** malloc large queues on BleEventWorker.
- **Tests:** behavioral; hardware soak under wardriving + slow SD.

### G10 — Unsynchronized Flipper `session_key` / `session_seq_out` / `outgoing_message_id`

- **Severity:** P1 (structurally racy; currently UI-gated)
- **Status:** SESSION_MEMORY, still open
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) main-thread `send_*_command` vs BLE-thread `capability_bootstrap` / pairing senders. Shared static `frag_buf` in `feb_fragment_record`.
- **Impact:** Concurrent send corrupts seq/nonce/fragments. Today commands are gated on `capability_has_*` after bootstrap returns, which is an unenforced invariant.
- **Fix:** One `FuriMutex` around `{session_key, seq_*, outgoing_message_id, send_pairing_record}` **or** route all TX through a single queue processed on one thread. Do not rely on comments.
- **Tests:** not easily; the mutex/queue is the test.

### G13 — ESP32 NVS pairing blob has no version, validity marker, or atomic replace

- **Severity:** P1 vs written contract; NVS itself is somewhat atomic
- **Status:** Known #17, still open. Step 8 in PLAN.md is the intended home — **do not steal this into an unrelated PR**, but it remains a contract gap.
- **Files:** [esp32/main/main.c](../esp32/main/main.c) `persist_pairing_secret` / `load_pairing_secret` (~L475–L512): raw 32-byte `nvs_set_blob`.
- **PROTOCOL:** "dedicated NVS namespace with a version, validity marker, and atomic replacement procedure."
- **Fix:** Implement as part of step 8, not as a drive-by. Struct `{magic, version, secret[32], crc}`; dual keys or write+commit+read-back; reject bad magic/len. Zeroize RAM copies on failure.
- **Do not:** change the HKDF/pairing_secret length.

### G14 — `any_saved_pairing_exists` treats any directory entry as a pairing

- **Severity:** P1 UX / boot path
- **Status:** Known #22, still open
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) (~L546)
- **Impact:** Leftover `.dat.tmp` from a crashed save → app auto-advertises "Have saved pairing" with no usable secret → `unknown_board` loops.
- **Fix:** Count only regular files matching `*.dat` (not `*.tmp`), ignore `.` / `..`. Optionally try a short header read.
- **Tests:** if storage is stubbed in host tests, add a tmp-only dir case; otherwise code review.

### G17 — `client_auth` proof failure leaves Flipper UI on Authenticating

- **Severity:** P1 UX; pairs with G03/G11
- **Status:** NEW (UI half of #6)
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) `handle_client_auth` fail path vs success `post_pairing_phase(SessionActive)`.
- **Fix:** On mismatch, `post_pairing_phase(Failed, generic reason)` still with no wire reply; then G11 disconnect. Do not say "unknown_board".
- **USER_GUIDE:** if on-screen text changes, sync via Haiku.

### G27 — `pending_command_kind` is never cleared; `internal_error` always looks like wardriving self-stop

- **Severity:** P1
- **Status:** NEW
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c)
  - Set at send sites (~L1948, L2017, L2123, L2191)
  - **Never** assigned back to `PendingCommandNone`
  - `handle_runtime_error` (~L1882): **any** `internal_error` posts wardriving stopped, ignoring `pending_command_kind`
  - Status routing (~L2442): non-wardriving states fall through to wifi_scan unless pending is ble_scan
- **Impact:** A wifi_scan `internal_error` (or a late error after a previous wardriving command) flips the wardriving UI to stopped. Stale pending kind mis-routes `busy` to the wrong capability.
- **Fix:**
  1. Clear `pending_command_kind` when the matching `complete`/`started`/`stopped`/`error` is handled, and on session reset / disconnect.
  2. Gate `internal_error` → wardriving self-stop on `pending_command_kind == Wardriving*` **or** on an explicit in-run flag, not on every error.
  3. Do not send unmatched `partial`/`complete` to wifi_scan by default; require pending kind or drop.
- **Tests:** none today (SESSION_MEMORY: no Flipper capability_query host tests). Add a small pure dispatcher test if you extract it; otherwise manual.

### G28 — Wardriving CSV writes a header on every `FSOM_OPEN_APPEND`

- **Severity:** P1 for WiGLE parsers if the same path is reopened non-empty
- **Status:** NEW
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) `wardriving_csv_ensure_open` (~L1668–L1672)
- **Impact:** Filename is second-granularity (`wardriving_YYYYMMDD_HHMMSS.csv`). Rapid reconnect in the same second, or a future reopen of the same handle path, prepends a second WiGLE header mid-file.
- **Fix:** After successful open, write the header **only if** `storage_file_size(file) == 0`.
- **Tests:** Flipper host tests for `feb_wardriving_csv_format_header` already exist; add an app-level comment if storage size cannot be stubbed. Hardware: confirm one header on SD (SESSION_MEMORY still pending user CSV check).

### G29 — `"started"` resets CSV dedup while the file stays open

- **Severity:** P1 product semantics / P2 if intentional
- **Status:** NEW
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) `handle_wardriving_status` "started" → `wardriving_csv_reset_state()` (~L1758) without closing `wardriving_csv_file`.
- **Impact:** Stop/start in one BLE session: same BSSID can be written again; FirstSeen anchors reset mid-file.
- **Fix:** Decide and document one of:
  - **A:** Dedup scope = start→stop segment (current behavior) — write it in CAPABILITIES.md / USER_GUIDE.
  - **B:** Dedup scope = CSV file lifetime — do not reset dedup on `started`; only on file close.
  - **C:** Rotate CSV file on each `started`.
- **Do not:** change silently; this is a product choice. Default recommendation: **C** or **B** if WiGLE uniqueness is the goal.

### G30 — Cross-thread wardriving log + dedup (Wi-Fi sys_evt vs NimBLE host)

- **Severity:** P1
- **Status:** NEW
- **Files:** [esp32/main/main.c](../esp32/main/main.c) `wifi_scan_done_handler` (~L1086) runs on **sys_evt** and calls `wardriving_dedup_and_maybe_append` → `wardriving_log_append`. Peek/mark_drained run on NimBLE host from `wardriving_send_next_batch`. [esp32/main/wardriving_log.c](../esp32/main/wardriving_log.c) has **no mutex**; `wd_pending_count`, offsets, peek cache, `wd_undrained_in_sector[]` are plain statics. BLE capture path is same-task (safer).
- **Impact:** Torn counters, double-drain, lost pending, corrupt oldest pointer under concurrent Wi-Fi append + BLE drain.
- **Fix:** Do **not** append from sys_evt. Copy records into a static staging buffer, `ble_npl_callout_reset` to NimBLE host, append there (same pattern as non-wardriving wifi_scan already uses for TX). Alternatively one FreeRTOS mutex around log+dedup; never hold it during flash erase longer than needed — prefer the callout handoff.
- **Tests:** hard on host; add a comment invariant "log API is single-threaded, NimBLE host only" and enforce it by moving the call.

### G31 — `backlog_remaining` uses unlocked `size_t` subtract

- **Severity:** P2 alone; P1 with G30
- **Status:** **FIXED 2026-09-12** — see `docs/PROJECT_HISTORY.md`. `wardriving_send_next_batch()`
  now saturates the subtract (`pending_now >= include_count ? pending_now - include_count : 0`).
  G30 (the underlying cross-thread race that can make `include_count` exceed the live count) is
  still open.
- **Files:** [esp32/main/main.c](../esp32/main/main.c) (~L1935)
  ```c
  remaining_after = wardriving_log_pending_count() - include_count;
  ```
- **Fix:** Saturating subtract, preferably inside the log module as `pending - drained_this_batch` under the same lock/thread as peek. If `include_count > pending`, remaining = 0.

---

## P2 — robustness, cost, defense-in-depth

### G12 — Flipper fragments every record at ATT MTU 23 (16-byte payload)

- **Status:** Known #12
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) `send_pairing_record` uses `feb_fragment_capacity(FEB_DEFAULT_ATT_MTU)` for pairing **and** all session commands.
- **Fix:** After connection/MTU is known, use `PAYLOAD_MAX` (64) + `FEB_ATT_WRITE_OVERHEAD` → 60-byte fragments (`feb_fragment_capacity(64 + FEB_ATT_WRITE_OVERHEAD)`). Keep 23 only before MTU settle. Do **not** use the link's 256 MTU — Flipper characteristic is 64 bytes ([LESSONS.md](LESSONS.md) att-mtu-vs-attribute-length).
- **Gain:** ~3–4× fewer notifications per `hello_ack` / `pair_reply` / command.

### G15 — ESP32 HMAC `full[32]` not zeroized after truncation to 16

- **Status:** Known #24
- **Files:** [esp32/main/session.c](../esp32/main/session.c) `feb_session_flipper_proof` / `feb_session_esp32_proof` (~L420–L436); pairing confirmations in [esp32/main/pairing.c](../esp32/main/pairing.c) (~L838–L863). Flipper already zeroizes.
- **Fix:** `feb_secure_zero(full, sizeof(full));` on every path after `memcpy` to `out`. Same for label scratch in `feb_session_hmac_label` if it holds key-derived bytes.

### G16 — Factory reset does not zeroize in-RAM `stored_pairing_secret`

- **Status:** Known #24
- **Files:** [esp32/main/factory_reset.c](../esp32/main/factory_reset.c) `perform_factory_reset` (~L131) erase NVS + `esp_restart()`. Secret lives in [main.c](../esp32/main/main.c). Soft reset keeps SRAM.
- **Fix:** Export `feb_wipe_pairing_secrets()` from main (or a boot-registered callback) that `feb_secure_zero`s stored secret + session/pairing scratch, call it **before** `esp_restart()`.

### G18 — Flipper X25519 donna static ladder scratch never zeroized

- **Status:** Known #11
- **Files:** [flipper/pairing_crypto.c](../flipper/pairing_crypto.c) `fmonty` / `cmult` / `crecip` static arrays; only `x25519_donna_scalarmult` zeros its own locals (~L762).
- **Impact:** ~3–4 KB scalar-derived limbs in `.bss` for process lifetime. Accepted threat model still wants zeroize-on-all-paths.
- **Fix:** `feb_secure_zero` at each helper's exit, **or** one malloc arena freed+zeroed after pairing (heap is larger than 1280-byte BleEventWorker stack — that is why these are static). Keep the port diffable vs upstream if you only add zeroize at wrapper exits.

### G19 — Reconnect still `xTaskCreate(..., 3072)` to sleep once

- **Status:** Known #13
- **Files:** [esp32/main/main.c](../esp32/main/main.c) `schedule_reconnect` / `schedule_runtime_auth_backoff` (~L630, L676)
- **Fix:** Second `ble_npl_callout` like `reassembly_timeout_co`. Removes 3 KB alloc/free every 30s during unattended outage.

### G20 — `notify_data_callback` NULL context sets `*data_len = PAYLOAD_MAX`

- **Status:** Known #20
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) (~L228)
- **Fix:** `if (data_len) *data_len = 0;`. One-line.

### G21 — Pairing/capability/CSV path buffers are 96 bytes (`FEB_*_PATH_MAX_LEN`)

- **Status:** Known #21
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) `final_path`/`tmp_path`/`path` `char[96]`. Full path = dir + `/` + board_id(32) + `.dat.tmp`. Truncation fails closed via `build_pairing_path`, so long `board_id` silently fails persist.
- **Fix:** Size to `DIR_MAX + 1 + BOARD_ID_MAX + sizeof(".dat.tmp")` (≥160). Surface a UI/log error on truncate, do not fail silently.

### G22 — Dedup table: XOR hash, collision evicts, Wi-Fi+BLE share 128 slots

- **Status:** NEW (module 2026-09-10; **not hardware-tested** per SESSION_MEMORY)
- **Files:** [esp32/main/wardriving_dedup.c](../esp32/main/wardriving_dedup.c)
- **Impact:** Collision → extra flash writes or lost ≥6 dB / 30 m tracking for the evicted address. Comment claims "~64 active devices, collisions rare" without probing.
- **Fix (optional, product):** open addressing or separate wifi/ble maps; store RSSI as `int8_t`. Not blocking if collision-as-log is accepted — then document it in CAPABILITIES.md.
- **Do not:** treat this as a security bug.

### G23 — Reassembly complete buffer used after mutex release; `profile_start` resets unlocked

- **Status:** Known style note
- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) (~L2294 feed+unlock then decode; `profile_start` ~L2470 `feb_reassembly_reset` without mutex)
- **Fix:** Under the mutex, `memcpy` completed record into `static uint8_t complete_record_buf[FEB_MAX_RECORD_SIZE]`, then process the copy. Always lock around reset.

### G24 — ESP32 built with `-Og`

- **Status:** Known #14
- **Fix:** Only if the user wants size. Record the choice in BASELINES.md. Partition is 1.5 MB vs ~677 KB app; no pressure.

### G25 — 256-byte stack RX buffer on NimBLE host

- **Status:** Known #16
- **Files:** [esp32/main/main.c](../esp32/main/main.c) `BLE_GAP_EVENT_NOTIFY_RX` (~L2864)
- **Fix:** `static uint8_t buffer[FEB_RX_FRAGMENT_BUFFER_SIZE];` — same rule as every other BLE-callback buffer in this repo.

### G32 — Factory-reset LED RMT leak on partial init failure

- **Status:** Known style
- **Files:** [esp32/main/factory_reset.c](../esp32/main/factory_reset.c) (~L96)
- **Fix:** `rmt_del_channel` / delete encoder on encoder or enable failure.

### G33 — `board_id_len` from `snprintf` return; missing `<stdio.h>`

- **Status:** Known style
- **Files:** [esp32/main/main.c](../esp32/main/main.c) `compute_board_id`
- **Fix:** `#include <stdio.h>`; clamp length to actual written (`written < 0` → fail; else `min((size_t)written, sizeof(buf)-1)`).

### G34 — ESP32 `feb_gcm_encrypt` failure uses `memset` not `feb_secure_zero`

- **Status:** NEW, minor
- **Files:** [esp32/main/session_crypto.c](../esp32/main/session_crypto.c) vs decrypt's `mbedtls_platform_zeroize`
- **Fix:** `feb_secure_zero` on ciphertext and tag failure paths.

---

## Documentation drift (fix with cheap model; do not mix into firmware PRs unless touching the same behavior)

| File | Drift |
| --- | --- |
| [README.md](../README.md) | ~~Still says AES-128; capability list aspirational.~~ **FIXED 2026-09-11** — rewritten to match PROTOCOL.md/CAPABILITIES.md. |
| [docs/BASELINES.md](BASELINES.md) | ~~Step 2 section says pairing/encryption/CBOR "remain future work."~~ **FIXED 2026-09-11** — reframed as historical, points to SESSION_MEMORY.md for current status. |
| [docs/CODE_REVIEW_FINDINGS.md](CODE_REVIEW_FINDINGS.md) | Scope note "protected path has zero call sites / step 7 not wired" is **false** now. Leave the file as historical; do not rewrite. |
| [docs/PLAN.md](PLAN.md) | ~~Step 8's "done when" said wardriving-log persistence "not yet started."~~ **FIXED 2026-09-11** — it's implemented; hardware acceptance for it is tracked in BACKLOG.md instead. |

---

## Optimizations (not bugs)

Already tracked in [BACKLOG.md](BACKLOG.md)'s "Codebase & agent cost-efficiency" section
(folded in 2026-09-11 from the retired `OPTIMIZATION.md`); still valid:

1. Extract capability dispatch from `esp32/main/main.c` and `flipper/flipper_esp32_over_ble.c` (hold until a roadmap boundary; decide static-buffer arena first).
2. Do **not** split `flipper/pairing_crypto.c` (auditability vs donna upstream).
3. Highest firmware-cost wins in this review: **G12** (Flipper fragment size), **G19** (reconnect task), **G07** (TX scheduler — also a bug).

---

## Suggested implementation order

Small, isolated, both-sides-agree items first. Skip anything the user has explicitly deferred (G07 was deferred as start-clobber — re-confirm).

| Order | IDs | Why this order |
| --- | --- | --- |
| 1 | G01, G02, G20, G32, G33, G34, G15 | Tiny, local, tests exist or are easy |
| 2 | G05, G04, G26 | Timer/seq robustness; G05 before new deadlines |
| 3 | G06 | Both firmwares; protocol-mandated error |
| 4 | G11 + G17 | Flipper close-on-fatal + UI; needed for G03 to mean anything |
| 5 | G03 | ESP32 AUTHENTICATED wait; resolve capability_query caveat with user |
| 6 | G09 | Shared emit-failure contract if you change `feb_emit_fn` |
| 7 | G27, G28, G29 | Flipper command/CSV semantics |
| 8 | G30, G31 | Wardriving log thread safety |
| 9 | G12, G19, G25, G18, G16, G21, G14 | Cost / zeroize / storage hygiene |
| 10 | G08, G10 | Larger Flipper architecture |
| 11 | G13 | Step 8 persistence; do not sneak in early |
| Hold | G07 | User deferred; still the worst live TX bug — ask before coding |
| Hold | G24, G22 | Product choice |
| Out of scope | X25519 constant-time, idle heartbeat, GPS backfill, ViewDispatcher rewrite, multi-board UX | SESSION_MEMORY / PLAN backlog |

---

## Explicit non-findings / do not "fix"

- Pairing X25519 is unauthenticated by design ([DECISIONS.md](DECISIONS.md)).
- Flipper cannot be GATT client (standalone FAP ABI).
- Idle 30s disconnect is specified; heartbeat is a protocol redesign.
- `requested` on `capability_query` is intentionally unimplemented.
- GPS is a fixed-coordinate stub; discarding pre-fix captures is specified.
- Step 4 coexistence sweep is **not** trustworthy evidence for wardriving BLE duty (SESSION_MEMORY); do not revert `ble_interval_ms` 500 / `ble_window_ms` 100 based on that sweep.
- Do not flash hardware in the course of acting on this document.

---

## Verification checklist for a fixer model

After each finding:

- [ ] Both firmwares still build (`tools/build_esp32.ps1`; Flipper `fbt.cmd fap_flipper_esp32_over_ble` via the project's temp `applications_user` sync).
- [ ] `python tools/check_shared_headers.py` if any shared header changed.
- [ ] Relevant `tests/esp32` / `tests/flipper` host suites.
- [ ] USER_GUIDE synced (Haiku) if user-visible behavior changed.
- [ ] SESSION_MEMORY one-liner only; narrative in PROJECT_HISTORY.
- [ ] Mark the finding `FIXED YYYY-MM-DD` in **this** file; do not delete the write-up.

# Cross-model findings backlog

Consolidated from four independent point-in-time reviews — [MAI-Code-1.1-Flash](../flipper_over_ble_findings/mai-code-1.1-flash-findings-2026-09-10.md),
[Gemini 3.8 Flash](../flipper_over_ble_findings/gemini-3.8-flash-findings-2026-09-10.md),
[GitHub Copilot](../flipper_over_ble_findings/github-copilot-findings-2026-09-10.md) (all 2026-09-10),
and [Grok 4.6](grok-4.6-findings-2026-09-11.md) (2026-09-11) — plus this session's own code
verification. Only items with either **cross-model agreement** or **independent code
verification** are listed; each entry says which. Grok's own file already tracks its full
G01-G34 list as a work ticket — this file does not duplicate that; it cross-references it.

This is a work ticket, like `grok-4.6-findings-2026-09-11.md`: cross items off as fixed with a
`FIXED YYYY-MM-DD (commit)` marker, don't rewrite descriptions, don't let this drift into
`PLAN.md`/`SESSION_MEMORY.md`.

---

## Fixed

### Wardriving location-dedup threshold was 1000x too large

**FIXED 2026-09-11 (`3111fa2`)** — `3000000ULL` → `2700ULL` in
[esp32/main/wardriving_dedup.c](../esp32/main/wardriving_dedup.c). Comment's own math (30m ≈
2700 units at 1e7-scaled lat/lon) was correct; the constant used didn't match it, requiring
~33km of movement before logging a new position instead of 30m.

- Agreement: Gemini (BUG-02) only; verified independently by this session via the unit math
  before the fix landed.

### AES-GCM 24-bit sequence cap not enforced (nonce-reuse risk)

**FIXED 2026-09-11 (`3111fa2`)** — `FEB_SESSION_SEQUENCE_MAX = 0xFFFFFFu` added on both
firmwares; `queue_and_send_protected()` (ESP32) and each Flipper command sender now refuse to
encrypt/send at the cap, and both receive paths reject/close on an incoming sequence at or
past it. Verified in the diff: [esp32/main/main.c](../esp32/main/main.c),
[flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c).

- Agreement: Copilot (F1, top priority), Gemini (BUG-07), Grok (G26, P0) — 3 of 4 reviews
  independently converged on this; strongest cross-model agreement of the whole set.

---

## Open — high-confidence, cross-model agreement

### TX single-flight is not exclusive — any `send_protected*` clobbers an in-flight wardriving drain

- **Status:** Open. Verified in code: `queue_and_send_protected()`
  ([esp32/main/main.c:958-969](../esp32/main/main.c#L958-L969)) has no check against
  `wardriving_tx_in_flight` or `tx_fragment_next < tx_fragment_total` before rebuilding the
  shared fragment buffers and overwriting `tx_done_action`. Confirmed concretely for the
  `stop` path at [main.c:2084-2088](../esp32/main/main.c#L2084-L2088) — `stop`'s
  `send_protected(..., "stopped")` runs unconditionally even mid-drain.
- **Impact:** `wardriving_tx_in_flight` sticks `true` (auto-drain permanently wedges for that
  connection until reconnect); the peer likely sees a torn multi-fragment record, which
  PROTOCOL.md's sequence contract treats as fatal — plausible session death, not just a UI
  glitch.
- **Agreement:** Gemini (BUG-04, BUG-05), Copilot (F4), Grok (G07 — "generalized," calls it
  "the worst live TX bug in the review"). Grok notes the user already deferred the narrower
  `start`-only form of this once; this generalizes to `stop` and `capability_query` too.
- **Not a drive-by fix** — needs a product decision first (busy/reject vs. a pending-record
  queue vs. accept the latency of deferring the ack). See this session's prior explanation
  for the three options and their tradeoffs before implementing.

### Flipper accepts `pair_init` with no local user gesture

- **Status:** Open. [docs/PAIRING.md](PAIRING.md) step 3 says "the user selects **Add ESP32
  board**" before the Flipper replies; `handle_pair_init()`
  ([flipper/flipper_esp32_over_ble.c:729](../flipper/flipper_esp32_over_ble.c#L729)) runs
  unconditionally on any incoming `pair_init` — no authorization-state check anywhere.
  Verified by reading both the doc and the handler.
- **Impact:** A nearby device can trigger and complete a full pairing ceremony against an
  already-paired Flipper with no physical interaction with it.
- **Agreement:** Copilot (F2) only. Not a contradiction of `DECISIONS.md`'s "pairing is
  unauthenticated by design" (that's about crypto/no-PSK, a different axis) — this is
  specifically the missing local-gate check the docs describe but the code doesn't implement.
  Needs a decision: implement the gate, or correct PAIRING.md if the gate was never intended.

### Flipper does not close the connection on auth/GCM/sequence failure

- **Status:** Open, but appears to be a **documented, deliberate tradeoff**, not an oversight —
  the code comment at
  [flipper_esp32_over_ble.c:2383-2388](../flipper/flipper_esp32_over_ble.c#L2383-L2388)
  explicitly says the Flipper (BLE peripheral) has no safe way to proactively
  `bt_disconnect()` from inside `profile_event_handler`, and relies on the ESP32's 30s idle
  timeout to reap a stuck connection instead.
- **Impact:** Violates PROTOCOL.md's literal text ("...is fatal: discard the record and close
  the BLE connection..."), but recovery already happens within 30s via idle timeout — bounded,
  not unbounded, DoS.
- **Agreement:** Copilot (F3), Grok (G11, G17). 2 of 4 reviews, both treating it as higher
  urgency than the existing code comment suggests the project already decided. Worth
  confirming with the user whether the 30s-idle-timeout tradeoff is still accepted or whether
  a safe main-thread-dispatched disconnect (Grok's suggested fix: post an `AppEvent`, don't
  call `bt_disconnect` from the GATT callback) is now wanted.

---

## Open — agreed by 3+, lower urgency (cost/perf, not correctness)

### Flipper fragments every record at ATT MTU 23 (16-byte payload) even after negotiation

- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c)
  `send_pairing_record()` always calls `feb_fragment_capacity(FEB_DEFAULT_ATT_MTU)`.
- **Agreement:** MAI (F1), Gemini (BUG-10), Grok (G12 — known since the 2026-09-07 review,
  still open), Copilot (optimization note). 4-way agreement; long-standing, perf-only, not
  correctness.
- **Fix sketch (Grok G12):** use `PAYLOAD_MAX (64) + FEB_ATT_WRITE_OVERHEAD` once MTU is known;
  do not use the link's 256-byte MTU directly — the Flipper characteristic itself caps at 64
  bytes (see LESSONS.md att-mtu-vs-attribute-length).

### Neither firmware sends the protocol-mandated `unsupported_version` error

- **Status:** Verified — no `unsupported_version` string in either firmware's `.c` files.
  Both sides currently drop/reject silently instead.
- **Agreement:** Grok (G06), Copilot (contract gap section). 2 of 4; low real-world impact
  today since both sides run the same protocol version, but it's an explicit PROTOCOL.md
  requirement.

### Unsynchronized cross-thread access to shared send-path state (Flipper GUI thread vs. BLE thread)

- **Files:** [flipper/flipper_esp32_over_ble.c](../flipper/flipper_esp32_over_ble.c) —
  `session_key`/`session_seq_out`/`outgoing_message_id`, `pairing_record_buf`, and
  `framing.c`'s static `frag_buf` are touched from both the GUI/main thread (user-initiated
  sends) and `BleEventWorker` (pairing/auth/notify handling), with no mutex.
- **Agreement:** Gemini (BUG-06), Grok (G10), MAI (general concurrency concern in F2). No
  independent re-verification of a live race by this session — flagged as plausible given the
  no-mutex pattern is consistent with everything else read in this file, but not traced
  end-to-end.

### ESP32 NVS pairing blob has no version/validity marker; Flipper storage does delete-then-rename

- **Agreement:** Copilot (F5, hardening-gaps section), Grok (G13, G14 — both marked "known,
  still open," explicitly scoped to PLAN.md step 8, "do not steal this into an unrelated PR").
- **Action:** Leave for step 8 persistence work, not a drive-by fix — this is Grok's explicit
  guidance and this session agrees with it.

---

## Disputed between models — flag, don't act on either verdict alone

### Wardriving dedup table: 7-bit XOR hash, 128 slots, collisions evict silently

- **Files:** [esp32/main/wardriving_dedup.c](../esp32/main/wardriving_dedup.c)
  `hash_address()`.
- **Gemini (BUG-03):** rates this High severity — flash wear, missed dedup on collision.
- **Grok (G22):** explicitly says "do not treat this as a security bug" — the code's own
  comment already documents the tradeoff ("~64 active devices... collisions should be rare"),
  and it's un-hardware-tested new code, not a regression.
- **This session's read:** the disagreement is about severity framing, not the underlying
  fact (both agree collisions are possible and unhandled). Leans toward Grok's read since the
  tradeoff is already documented in the code, but flagging rather than resolving — this is a
  product call (accept the collision risk vs. spend the flash-wear/complexity budget on a
  better hash) rather than an obvious bug fix.

---

## Open — enhancements and backlog

### BLE active scanning (on/off toggle, on by default)

- **Status:** Implemented in `ble_scan` (active `params.passive = 0`); not yet in wardriving.
- **Rationale:** Passive BLE scanning only gets names advertised in the advertisement packet
  itself. Many nearby devices don't include their name there. Active scanning sends scan
  requests; devices respond with scan response data that often includes full names.
- **Cost:** ~10-20% latency increase per device (wait for scan response), but much better name
  coverage. No connection overhead.
- **Priority:** BLE-related, medium (improves discoverability, not correctness).
- **Backlog notes:**
  - Implement a runtime toggle (similar to interval config) so wardriving can opt in/out.
  - Consider enabling for wardriving too (currently only ble_scan has it).
  - Measure real-world name discovery improvement once hardware-tested.

---

## Not re-litigated here

Grok's [grok-4.6-findings-2026-09-11.md](grok-4.6-findings-2026-09-11.md) already tracks a much
longer single-source list (G01-G34, plus carried-over status of the 2026-09-07 review's #5-#24).
None of those are duplicated in this file unless a second model also independently found them
(the four items above under "agreed by 3+" and "high-confidence" sections). Use Grok's file as
the exhaustive backlog for everything single-sourced; use this file for what multiple
independent reviews converged on.

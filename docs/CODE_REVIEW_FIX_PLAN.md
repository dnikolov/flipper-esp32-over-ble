# Fix plan: pre-step-7 shared-contract convergence (planned 2026-09-07)

Execution plan for the "fix now" subset of [CODE_REVIEW_FINDINGS.md](CODE_REVIEW_FINDINGS.md),
to be run in a clean session **before step 7 implementation starts**. Written to be
self-contained: a fresh session should be able to execute this without re-deriving any of the
decisions below.

Rationale for doing these before step 7 rather than later: all six findings live in the shared
CBOR / framing / session-crypto primitives that step 7's `capability_query`/`capability_response`
payload schemas and the protected-record path will be built directly on top of. Fixing them
afterwards means re-touching code step 7 has already built on, with a larger blast radius and
more re-verification. Fixing them first means step 7 inherits converged primitives.

Scope: 6 work items covering findings #1, #2, #3, #4, #9, #10 (the review's five "fix now"
bullets, one of which grouped two findings). Everything else in the findings doc is explicitly
out of scope — see "Non-goals" at the end.

## Read first

- [CODE_REVIEW_FINDINGS.md](CODE_REVIEW_FINDINGS.md) — findings #1, #2, #3, #4, #9, #10.
- [PROTOCOL.md](PROTOCOL.md) — "Canonical CBOR encoding (definition)" and "Cryptographic
  requirements"; W2 and W6 add to the former.
- `CLAUDE.md` "Conventions" — in particular: a wire-format change is not done until both sides
  implement it identically and both build.

## Decisions already settled (do not re-litigate)

**D1 — Converge on the strictest behavior, not the most permissive.** In every divergence below
where one side accepts input the other rejects, the fix is to make both reject. Loosening a
restriction later is a deliberate, documented protocol change; accidentally shipping a
permissive decoder is how these drifted in the first place.

**D2 — Permitted payload value types (closes a real PROTOCOL.md spec gap).** `docs/PROTOCOL.md`
never states which CBOR major types may appear inside a `payload` map, which is why the two
`feb_cbor_skip_value()` implementations disagree. Decision: **payload values are restricted to
unsigned integers (major 0), byte strings (2), text strings (3), arrays (4), and maps (5).**
Negative integers (1), tags (6), and simple/float values (7 — including `true`/`false`/`null`)
are rejected. Verified safe against the near roadmap: step 7's payloads are text and arrays of
text only (`PLAN.md` step 7 decisions), and Phase 3's wardriving results are specified as
compact binary, i.e. byte strings. When a real need for booleans or negative numbers appears, it
becomes an explicit PROTOCOL.md revision plus matching vectors on both sides — not a silent
decoder difference. This is a judgment call made during planning, not put to a grill-me session;
flagged here per this project's usual practice for a resolved doc ambiguity, and cheap to
reverse if the user disagrees.

**D3 — `FEB_CBOR_MAX_NESTING`'s own documented derivation is the tiebreaker for depth.**
`cbor_codec.h` documents the limit as "outer map -> payload map -> array -> element" (4 levels).
Passing the payload span at `depth = 2` produces exactly that chain; `depth = 1` allows one extra
level. So the ESP32's `2` is correct and the Flipper's `1` is the bug. No protocol change needed.

**D4 — Over-length `board_id` zeroes the output, it does not clamp.** An all-zero key fails GCM
authentication immediately and visibly; a key derived from a silently-truncated `board_id` is
wrong but plausible and would present as an unexplained proof mismatch. So the ESP32's
zero-the-output behavior wins and the Flipper's clamp is removed. The shared headers stay
byte-identical and no function signature changes (these are `void`-returning by frozen contract).

**D5 — Reuse existing `feb_cbor_status_t` members; do not add new ones.** Adding an enum member
means churning both copies of a frozen header plus every test. `FEB_CBOR_ERR_UNEXPECTED_TYPE`
already maps to wire `error.code` `malformed_record` per `cbor_codec.h`'s own comment table and
is the right bucket for both the truncated-map (W1) and trailing-bytes (W6) rejections.

**D6 — Do these edits in the orchestrating session, not via the firmware subagents.** These are
cross-firmware convergence edits whose entire purpose is that both copies end up behaving
identically. `docs/SESSION_MEMORY.md`'s 2026-09-03 `framing.c` stack fix set the precedent: it
was done "by the orchestrating session, not a subagent, to guarantee the two copies got the
identical change." Use `esp32-developer` / `flipper-developer` only for the per-firmware build
verification in step 4 below, which is toolchain-specific and genuinely per-target.

## Work items

Note the pattern: **four of six are Flipper-only fixes.** In every divergence except W2 and W6,
the ESP32's copy is already the correct/stricter one and the Flipper's independently-written copy
drifted looser. Worth remembering for where to aim future review effort.

### W1 — Out-of-bounds read in the Flipper's CBOR map validator (finding #1)

`flipper/cbor_codec.c:373`, in `feb_cbor_skip_value()`'s major-type-5 case. Add the bounds guard
the ESP32 already has, matching its exact form and status so the two agree:

```c
/* current */   if((uint8_t)(in[pos] >> 5) != 3) {
/* target  */   if(pos >= in_len || (uint8_t)(in[pos] >> 5) != 3) {
```

ESP32 reference: `esp32/main/cbor_codec.c:485`. Flipper-only change; ESP32 untouched.

### W2 — Converge `feb_cbor_skip_value()`'s accepted major types (finding #2)

Per D2, both sides must accept exactly majors 0, 2, 3, 4, 5 and reject everything else with
`FEB_CBOR_ERR_UNEXPECTED_TYPE`.

- `esp32/main/cbor_codec.c:392-403` — the `switch (major)` currently has `case 0:` and `case 1:`
  sharing a body. **Remove `case 1:`** so negative integers fall through to `default:` (line 516)
  and are rejected.
- `flipper/cbor_codec.c:405-413` — **remove the `case 7:` block** (which currently accepts
  `ai == 20..23`, i.e. `false`/`true`/`null`/`undefined`) so major 7 falls through to `default:`
  and is rejected. Leave the `default:` comment, updating it to note majors 1, 6 and 7 are all
  unused by this protocol.
- Both: the `ai` local in the Flipper's function becomes unused once `case 7:` is gone — remove
  it to avoid a `-Wunused-variable` build failure.

Both sides change here. Also update `docs/PROTOCOL.md` — see step 3 below.

### W3 — Converge payload nesting depth (finding #3)

Per D3, the Flipper's two record-level decoders must pass `2`, matching the ESP32's:

- `flipper/cbor_codec.c:654-655` — `feb_cbor_decode_unencrypted()`'s payload span: `1` → `2`.
  (ESP32 reference: `esp32/main/cbor_codec.c:758`.)
- `flipper/pairing.c:195-196` — `feb_cbor_decode_pairing_envelope()`'s payload span: `1` → `2`.
  (ESP32 reference: `esp32/main/pairing.c:170`.)

This second call site was **not** in the original findings write-up (`flipper/pairing.c` wasn't
read during the scan) — it was found while planning. Correct finding #3's scope in the findings
doc accordingly.

Note: `feb_cbor_decode_protected()` has no payload span (its `ciphertext` is a plain byte string),
so it needs no depth change on either side.

### W4 — Over-length `board_id` must zero, not clamp (finding #4)

Per D4, remove the Flipper's clamps and zero the output instead, matching the ESP32:

- `flipper/session.c:391-393` — `feb_session_derive_key()`: replace the
  `if(board_id_len > MAX) board_id_len = MAX;` clamp with
  `if(board_id_len > MAX) { feb_secure_zero(out, FEB_SESSION_KEY_LEN); return; }`, keeping the
  existing `feb_secure_zero(salt/info)` cleanup on that path too. ESP32 reference:
  `esp32/main/session.c:451-454`.
- `flipper/pairing.c:617-619` — `feb_pairing_derive_secret()`: same treatment, zeroing `out`
  (`FEB_PAIRING_SECRET_LEN`). ESP32 reference: `esp32/main/pairing.c:799-802`. **This second
  function was not in the original findings write-up** — also found while planning; correct
  finding #4's scope.
- Update the explanatory comment at both sites (currently "this clamp is a defensive backstop")
  to describe the new behavior.

**Corrected severity, record this in the findings doc:** finding #4 as written implies a live
exploit path. It is not currently reachable — both application layers already bound `board_id`
before reaching these functions (ESP32 `main.c:1227-1228` compares against its own ~20-char
`board_id`; Flipper `handle_hello()` calls `board_id_is_valid()`, which rejects `len > 32`). So
this is defense-in-depth in a shared primitive, not a live bug. It stays in this pass because
step 7 adds new callers of the derivation path and shouldn't be able to get it wrong.

### W5 — Flipper GCM must fail deterministically (finding #9)

`flipper/session_crypto.c`. Keep both frozen signatures; only make the failure paths match the
ESP32's:

- `feb_gcm_encrypt()` (line 31): capture `furi_hal_crypto_gcm_encrypt_and_tag()`'s
  `FuriHalCryptoGCMState` instead of discarding it via `(void)`. On anything other than
  `FuriHalCryptoGCMStateOk`, `feb_secure_zero(ciphertext_out, plaintext_len)` (guarded on
  `plaintext_len > 0`) and `feb_secure_zero(tag_out, FEB_SESSION_GCM_TAG_LEN)`. Without this, a
  hardware key-load failure leaves `ciphertext_out` untouched and stale buffer contents go out
  over BLE. ESP32 reference: `esp32/main/session_crypto.c:29-34`.
- `feb_gcm_decrypt()` (line 44): on `state != FuriHalCryptoGCMStateOk`,
  `feb_secure_zero(plaintext_out, ciphertext_len)` (guarded on `ciphertext_len > 0`) before
  returning 0. ESP32 reference: `esp32/main/session_crypto.c:59-64`.
- `session_crypto.c` will need `#include "pairing_crypto.h"` for `feb_secure_zero()` if it isn't
  already reachable; plain `memset` is not acceptable here per `pairing_crypto.h`'s contract that
  all zeroization goes through the one auditable function.

Update the comment block at `feb_gcm_encrypt()` — it currently justifies discarding the return
value, which this change reverses.

### W6 — Reject trailing bytes after a decoded record (finding #10)

Add an exact-consumption check to the three record-level decoders on **both** firmwares. After
the final field, require `pos == in_len`, else return `FEB_CBOR_ERR_UNEXPECTED_TYPE` (per D5):

| Decoder | ESP32 | Flipper |
| --- | --- | --- |
| `feb_cbor_decode_unencrypted` | `cbor_codec.c` (before the final `return FEB_CBOR_OK`) | `cbor_codec.c:662` |
| `feb_cbor_decode_protected` | `cbor_codec.c` | `cbor_codec.c:770` |
| `feb_cbor_decode_pairing_envelope` | `pairing.c` | `pairing.c:200` |

The ESP32's `_unencrypted`/`_protected` decoders are loop-driven and track `pos` across
iterations, so the check goes after the missing-field verification loop, using the same `pos`.

Deliberately **not** applied to the payload decoders (`hello`, `hello_ack`, `client_auth`,
`pair_*`, `error`): those are handed `payload_span`/`payload_span_len`, whose length is the exact
span `feb_cbor_skip_value()` consumed, so trailing bytes are already structurally impossible
there. Adding redundant checks would widen the diff without adding coverage. Note this reasoning
in the PROTOCOL.md addendum so it isn't "fixed" later by someone assuming it was an oversight.

**Verified safe against existing tests:** every current call site in `tests/` passes an exact
vector length. The one exception, `tests/flipper/test_pairing.c:478`, deliberately passes
`FEB_VEC_PAIR_INIT_RECORD_LEN - 1` as a truncation case and will still fail with a truncation
error, unaffected by this check.

## Execution order

### 1. Add vectors and tests first (expect red)

The root cause of W1–W3 is that `feb_cbor_skip_value()` has **zero direct test coverage** on
either side. Write the tests before the fixes so it's provable they actually exercise the bugs.

Add to `tests/vectors/vectors.h` (shared, hand-authored from the spec — not generated from
either codec, per step 3's original discipline):

- A payload map containing a **negative integer** value → both must reject.
- A payload map containing **`true`** (and one with `null`) → both must reject.
- A **truncated map**: header declares more entries than bytes present → both must reject
  (this is W1's repro; on the unfixed Flipper it reads out of bounds).
- A payload nested **one level deeper** than `FEB_CBOR_MAX_NESTING` allows → both must reject,
  and one at exactly the limit → both must accept (pins W3 from both directions).
- A valid record with **one trailing byte** appended → both must reject (W6).

Add matching cases to `tests/esp32/test_framing_cbor.c` and
`tests/flipper/test_flipper_codec.c`, asserting the **same** `feb_cbor_status_t` on both sides,
not just "some error." Status-code equality is the property that was silently broken.

Also add direct `feb_cbor_skip_value()` unit cases (currently it's only ever reached indirectly
through envelope decoding of well-formed payloads).

Run the host tests and confirm the new cases fail in the expected asymmetric pattern before
touching any implementation file.

### 2. Apply W1–W6

In the orchestrating session per D6. Suggested grouping to keep each diff reviewable: W1+W2+W3
(all `feb_cbor_skip_value`-adjacent), then W6 (decoder tails), then W4+W5 (crypto primitives).

After the edits, re-diff the six shared headers to confirm none drifted:

```powershell
foreach ($h in "framing.h","cbor_codec.h","pairing.h","pairing_crypto.h","session.h","session_crypto.h") {
  if (Compare-Object (Get-Content "esp32\main\$h") (Get-Content "flipper\$h")) { "DIFFERS  $h" } else { "IDENTICAL $h" }
}
```

Expected: all six identical. (`framing.h` and `cbor_codec.h` currently differ in comment text
only — finding in the style section of the findings doc. Restoring those two to byte-identical is
a **nice-to-have** in this pass since they're already open in the editor, but it is comment-only
and must not change any declaration.)

### 3. Update docs in the same pass

- **`docs/PROTOCOL.md`** — add to "Canonical CBOR encoding (definition)": the D2 permitted-value-
  types restriction, the D3 nesting-depth clarification, and the W6 trailing-bytes rejection rule
  (including why payload decoders are exempt). These are wire-contract clarifications and are the
  authoritative record of the D2 decision.
- **`docs/CODE_REVIEW_FINDINGS.md`** — mark #1, #2, #3, #4, #9, #10 fixed; correct #3's scope
  (second call site in `flipper/pairing.c`), #4's scope (`feb_pairing_derive_secret` too) and #4's
  severity (not currently reachable — see W4).
- **`docs/PLAN.md`** — record the fixes under a new dated subsection in the existing
  "Live code-health defects" area, following the 2026-09-05 precedent. Separately, cross-reference
  finding #17 (NVS version/validity marker/atomic replacement) into **step 8**'s section, since
  that's the step already scoped to own it.
- **`docs/SESSION_MEMORY.md`** — dated entry: what changed, that it's build-verified only, and
  that step 7 is unblocked.
- **`docs/USER_GUIDE.md`** — **no update needed.** None of W1–W6 changes on-screen text, LED or
  button behavior, build/flash steps, or pairing/reconnect/reset behavior. Confirm this still
  holds at the end of the pass; if it somehow doesn't, delegate that edit to Haiku per CLAUDE.md.

### 4. Verify

All four must pass before the pass is considered done:

```powershell
# host-native tests, both firmwares (6 scripts)
tests\esp32\build.ps1;    tests\esp32\build_pairing.ps1;    tests\esp32\build_session.ps1
tests\flipper\build.ps1;  tests\flipper\build_pairing.ps1;  tests\flipper\build_session.ps1

# ESP32 firmware
. C:\Users\Deyan\esp\esp-idf\export.ps1
Set-Location C:\Users\Deyan\flipper-esp32-over-ble\esp32
idf.py build

# Flipper FAP (sync into the pinned checkout first, per CLAUDE.md's APPSRC constraint)
fbt.cmd fap_flipper_esp32_over_ble
```

Every previously-passing test must still pass, and the new cases must now pass identically on
both sides. Delegate the two firmware builds to `esp32-developer` / `flipper-developer` if
convenient (per D6 they're the only per-target part), but the test-status comparison should be
read in the orchestrating session, since agreement between the two is the actual deliverable.

### 5. Do not flash

Source-only pass; no hardware. Per `CLAUDE.md`'s hardware-safety rule, do not flash, erase, or
write either board without an explicit request.

**Flag as a follow-up requiring the user's go-ahead:** W1–W3 and W6 touch decoders that are on
the *live, already-hardware-verified* pairing and runtime-auth paths, so a hardware
re-verification (one full pairing ceremony plus one stored-secret runtime auth, both devices
monitored) is genuinely warranted before these are trusted in the field — even though the changes
are strictly-narrowing. Recommend running it before step 7 rather than after, so a regression here
can't be confused with a step-7 bug. Ports were `COM9` (ESP32) / `COM8` (Flipper) in prior
sessions; reconfirm, they aren't stable.

## Non-goals for this session

Explicitly out of scope — do not scope-creep into these, they're tracked in
[CODE_REVIEW_FINDINGS.md](CODE_REVIEW_FINDINGS.md):

- Findings #6, #7, #8 (client_auth desync, missing pairing timeout, clock wraparound) — live bugs
  in shipped code, but independent of step 7 and each needs its own design thought.
- Findings #11, #12, #13, #14, #15, #16 (memory/performance) — self-contained and valuable, but
  no ordering dependency on step 7.
- Finding #17 (NVS hardening) — belongs to step 8; cross-reference only.
- Findings #18–#22, #24 and the whole style/documentation section.
- **The single cross-implementation test runner.** This is the real structural fix for the bug
  class W1–W3 represent, and it's the highest-value item in the findings doc's tooling
  observations — but it's tooling work substantially larger than these six fixes and would
  swallow the session. Do it as its own deliberate piece of work.
- Step 7 implementation itself. This pass exists to precede it, not to begin it.

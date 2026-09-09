# Lessons

Narrative record of bug classes found on real hardware in this project, kept separate from
the two developer subagent files (`.claude/agents/esp32-developer.md`,
`.claude/agents/flipper-developer.md`) so those files can stay short, rule-first, and cheap
to load on every invocation. Each agent file states the *rule* in one or two lines and links
here for the *why* — read this file when you want the incident, the root cause, or the
reasoning that produced the rule; skip it when you already know the rule and just need to
apply it.

Organize by symptom/theme, not chronologically — these are meant to be looked up, not read
top to bottom. Add a new entry here (not a new paragraph in an agent file) when a bug's root
cause is worth remembering; add or adjust the *rule* in the relevant agent file only if the
lesson should change future behavior, per this project's own convention (a lesson logged only
here doesn't change behavior — the agent file does).

---

## Shared (both firmwares)

### header-contract-vs-implementation-drift

`framing.h` described `feb_fragment_record()` as writing "into a caller-owned buffer sized >=
`FEB_FRAG_HEADER_SIZE` + capacity — no dynamic allocation," but the function took no buffer
parameter and used a file-scope static instead — contradicting the header comment on **both**
firmwares from 2026-09-03 until fixed on 2026-09-05. A header comment is part of the contract
you must keep true, not decoration: when you change an implementation, re-read the
declaration's prose in the same edit and fix it if it no longer matches.

### unwired-declared-api

`feb_reassembly_check_timeout()` was specified in `framing.h` with a 2-second timeout but was
never called from either `main.c` or `flipper_esp32_over_ble.c` — an identical, independently
discovered gap on both sides. A stalled partial fragment sequence held the reassembly buffer
until the next complete message or a disconnect. Fixed by wiring it to a periodic callback
(NimBLE `ble_npl_callout` on the ESP32 side, a FuriTimer callback guarded by a new
`reassembly_mutex` shared with `feb_reassembly_feed()`'s call site on the Flipper side, since
the timer callback and the BLE-event-driven feed path run on different threads there). A
declared shared-contract API that nobody calls is a silent gap on *both* implementations until
proven otherwise, not just the one you're looking at — adding an API to a shared contract
means wiring a caller on both sides in the same change, or recording explicitly why not.

### two-implementations-agreeing-is-not-two-implementations-being-right

Independent implementation genuinely works here — in step 3 both sides found the same two
`framing.c` defects separately, each on their own. But both derive from *one* shared contract
and *one* shared vector set, so a mistake in the contract (or an unstated assumption neither
side questioned) propagates to both and nothing disagrees. Two confirmed examples:

- `FEB_CBOR_MAX_BYTES_LEN` bounded `ciphertext` to 256 while `FEB_CBOR_MAX_PAYLOAD` was 512,
  and GCM ciphertext is exactly as long as its plaintext regardless of key size — so a
  maximum-size payload could not round-trip. Identical on both firmwares, exercised by no
  vector, latent since step 3; fixed 2026-09-05 by raising it to `512u` to match. Validate
  bounds against `docs/PROTOCOL.md` directly, never against the other implementation or the
  test vectors — agreement between the two is not evidence either one is right.
- wifi_scan (2026-09-07): `feb_cbor_skip_value()`'s nesting-depth budget is a pure internal
  recursion counter with no wire representation, so nothing in `docs/PROTOCOL.md` or the test
  vectors could catch a wrong assumption about it — and both firmwares independently assumed
  the same wrong one (that a field nested inside `payload`, like `command.arguments` or
  `status.result`, should inherit `payload_span`'s depth-2 starting point). It doesn't: a field
  with its own dedicated, schema-aware decoder is its own self-contained span and must get a
  *fresh* depth-0 budget when it recurses into `feb_cbor_skip_value()` for a still-generic
  sub-piece, otherwise a real, spec-legal shape (here, `result` → `aps` array → 6-field
  `<ap-result>` map — 3 real containers) silently exceeds `FEB_CBOR_MAX_NESTING` and gets
  rejected. Both agents hit this the same way, independently — confirming it's a genuine spec
  gap, not implementation carelessness, but also confirming the general point: **when you add
  a new schema-aware decoder for a field that itself sits inside another generically-validated
  field, explicitly decide and state what depth budget it starts from; don't silently inherit
  whatever the outer call site happened to be at.** Documented in `docs/PROTOCOL.md`'s "Nesting
  depth" section — read it before adding another nested payload shape.

**Rule this motivates:** when you and the other firmware's agent add new shared codec
functions/macros/structs in parallel, run `python tools/check_shared_headers.py` before
reporting done — it diffs macro values and function prototypes automatically. It does **not**
catch a struct-body shape divergence (see `wardriving-struct-shape-divergence` below, which it
was blind to), so for any new composite/optional-field/union-like shape, also read the actual
struct definition on both sides side by side, not just the tool's exit code.

### wardriving-struct-shape-divergence

wifi_scan's parallel implementation (2026-09-07) broke the "byte-identical API surface"
convention silently: the two agents wrote genuinely different signatures for
`feb_cbor_encode_wifi_scan_result_payload()` and a differently-named macro
(`FEB_WIFI_SCAN_MAX_APS` vs. `FEB_WIFI_SCAN_MAX_APS_PER_RECORD`), caught only by an explicit
post-hoc diff in the orchestrating session. The wardriving codec (also 2026-09-07) repeated
this at a deeper level and went undetected for longer: `feb_wardriving_record_t` was a tagged
union (`payload_kind` enum + `union { wifi; ble; }`) on the ESP32 side but two always-present
named struct members (`wifi_payload`/`ble_payload`, no discriminator field) on the Flipper
side — both header comments even flagged the choice as "internal representation choice (flag
for ESP32-side cross-check)" and the cross-check never happened. Same root cause both times:
a function-signature or struct-shape decision left to each agent's own judgment, with no
automated check and no PROTOCOL.md text to arbitrate it (correctly so — PROTOCOL.md specifies
the *wire* shape, not the C-level internal representation). Reconciled 2026-09-08 onto the
tagged-union shape (PROTOCOL.md's own "Nesting depth" section already names `payload_kind` as
the anticipated discriminator for a future flattening, so that shape was the intended
direction) and a single `has_ble_params` flag for the command payload's paired
`ble_window_ms`/`ble_interval_ms` fields (PROTOCOL.md states they're required together, so one
flag makes the "one present, one absent" state unrepresentable instead of a decoder-side
guard). When a wire spec leaves an internal representation choice open, state the chosen shape
explicitly in the shared header's comment *and* get it read by the other side before both
implementations diverge, not after — "record it here so the other side's implementer matches"
in a comment nobody reads is not itself the cross-check.

---

## ESP32-specific

### att-mtu-vs-attribute-length

The negotiated ATT MTU (256 here) tells you what the *link* can carry — it says nothing about
what the peer's *attribute* will accept. A GATT characteristic's declared max value length is
an independent cap enforced regardless of MTU headroom; exceeding it fails with ATT error
0x0D (`ATT_ERR_INVALID_ATTR_VALUE_LEN`, surfaced by NimBLE as status 269 = `BLE_HS_ATT_BASE` +
13). The Flipper's Write characteristic is fixed at 64 bytes (`PAYLOAD_MAX` in
`flipper/flipper_esp32_over_ble.c`), so outgoing fragments must be sized against
`FEB_FLIPPER_WRITE_EFFECTIVE_MTU`, not `negotiated_att_mtu` — don't "improve" this back to the
raw MTU. This shipped with a clean build and passing host tests; a test that pins
`feb_fragment_capacity()` to a conservative value (16-byte fragments, exercising multi-fragment
reassembly) guarantees the oversized-write path is never once exercised, which is exactly what
let this sit latent until the first real record went out at the real negotiated MTU. When a
test pins a parameter to a conservative value, state explicitly which failure modes that
pinning *excludes*.

### flipper-facts-must-be-read-not-assumed

The two firmwares are implemented independently against frozen shared contracts, which is
deliberate and has caught real bugs. Its blind spot: an invariant that isn't in the shared
contract is checked by nobody. The 64-byte characteristic cap above was exactly that — a
Flipper-side implementation detail that was silently also a wire constraint. When your code
depends on any fact about the Flipper (buffer sizes, characteristic properties, handle layout,
timing), confirm it by reading `flipper/*.c`, and say in your report that the dependency
should be promoted into `framing.h`/`docs/PROTOCOL.md` rather than left implicit.
"Protocol-shaped" includes transport sizing, not just CBOR field shapes.

### sdkconfig-defaults-not-retroactive

ESP-IDF seeds new keys from `sdkconfig.defaults` only into a *fresh* `sdkconfig`; an
already-answered option keeps its old value. `CONFIG_MBEDTLS_HKDF_C=y` was added to defaults
and the build passed immediately — while the generated `sdkconfig` still said "is not set",
because nothing called `mbedtls_hkdf()` yet and `--gc-sections` stripped the path before it
could fail to link. After changing `sdkconfig.defaults`, grep the generated `sdkconfig` to
confirm the value actually took, and regenerate it (delete + rebuild) if it didn't.

### fragment-count-comment-rot

`FEB_TX_MAX_FRAGMENTS` was `48u`, still documented as
`ceil(FEB_MAX_RECORD_SIZE / feb_fragment_capacity(23))` — a capacity this firmware had stopped
using once fragment sizing moved to `FEB_FLIPPER_WRITE_EFFECTIVE_MTU` (capacity 60, so 13
fragments suffice), leaving the three `tx_fragment_*` arrays ~3.7x oversized and the derivation
false. Fixed to `13u` with a corrected comment. When you change a sizing parameter, grep for
every constant and comment derived from it and update them in the same edit — a comment
stating a derivation is a claim, and claims rot.

### esptool-read-commands-are-not-reset-free

`read_flash` (and most other `esptool` commands) default to `--after hard_reset`, which
reboots the chip into the running app the moment the "read-only" operation finishes — a real
state change here, since every boot unconditionally opens a fresh 120-second pairing window
(`docs/PAIRING.md`). This silently triggered an unplanned second pairing ceremony during the
2026-09-05 step-5 hardware verification (the still-live Flipper app answered the new window
with no user interaction), corrupting what was meant to be a clean before/after NVS snapshot.
Pin `--before default_reset --after no_reset` explicitly for any dump meant to be a true
read-only snapshot; `no_reset` leaves the chip in the ROM bootloader afterward, so a deliberate
`--after hard_reset` or bare `chip_id` call is needed to resume normal operation.

### nimble-host-stack-budget

The NimBLE host FreeRTOS task has a tight 4096-byte stack (`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096`),
shared by BLE event dispatch, pairing handlers, and all BLE-callback-path code. This recurring
bug class has hit the project **five times** (steps 3, 5, 7, wifi_scan, and wardriving):
stack-local buffers, especially stack-allocated structs with embedded arrays, silently overflow
this budget when nested inside the callback chain. A representative example from wardriving
(2026-09-10): `wardriving_send_next_batch()`'s trial-encode struct (`feb_wardriving_status_result_payload_t`,
embedding a 32-entry record array) was 2608 bytes as a stack-local — ~64% of the entire task
stack — versus 32 bytes once moved to file-scope `static`. The sibling `ble_scan_send_next_batch()`
already had this pattern fixed; the fix wasn't converged. Each overflow is painful: the Flipper's
`BleEventWorker` is similarly tight (1280 bytes) and has hit this same class four times as actual
hardware crashes (see `docs/LESSONS.md#ble-event-worker-stack-budget`); both are debugging
hard-to-recognize crashes at dev-time rather than a compile-time error. The standing mitigation:
any non-trivial buffer or struct on the BLE-callback path should default to file-scope `static`,
and get a real `-fstack-usage` check before being trusted at a tight budget (this check is still
a backlog item, not yet wired into the build). Const and single-initialization-at-boot patterns
are safe; true per-call scratch must be static, not stack.

### wardriving-tx-in-flight-cleared-before-delivery-confirmed

2026-09-10 hardware test (after the same-day stack-overflow fix, see `nimble-host-stack-budget`
above) exposed a second bug during a live `wardriving` run with both sources active: a GATT-write flood (`ble_gattc_write_flat()` returning `BLE_HS_ENOMEM`,
NimBLE's 6) followed by a `ble_gap_disc()` restart racing the reconnect path
(`BLE_HS_EBUSY`, 15) and an unconditional `wardriving_self_stop("internal_error")`.

Root cause of the write flood: `wardriving_send_next_batch()` cleared
`wardriving_tx_in_flight` (and called `wardriving_log_mark_drained()`) as soon as
`queue_and_send_protected()` returned — which only confirms the record's *first* fragment
was handed to NimBLE, not that a multi-fragment record actually finished sending
(`write_complete()`'s `tx_done_action` dispatch is the only signal for that). `wifi_scan`/
`ble_scan` have the identical pattern but never manifested it, because nothing ever
re-triggers a new send while the old one's tail fragments are still in flight except a slow,
user-initiated `command`. `wardriving`'s own capture-completion timers fire automatically
every `wardriving_ble_window_ms`/`wardriving_wifi_interval_ms` (as low as 30 ms at the
default "point-4" cadence), so a new batch could be kicked off before the previous one's
fragments finished, clobbering the single shared `tx_fragment_*` state with a second
concurrent send — an actual violation of `docs/PROTOCOL.md#fragmentation`'s single-in-flight
invariant, not just a config-tuning issue. Fixed by always chaining through
`TX_DONE_CONTINUE_WARDRIVING` (never `TX_DONE_NONE`) and only clearing
`wardriving_tx_in_flight`/marking records drained on the *next* re-entry, which only happens
once `write_complete()` confirms full delivery — see `wardriving_pending_drain_count`'s
comment in `esp32/main/main.c`. This also fixes a latent record-loss bug: records were
previously marked drained from the flash log before delivery was confirmed, so a mid-record
write failure silently lost them forever.

Root cause of the EBUSY/self-stop: `BLE_GAP_EVENT_DISC_COMPLETE`'s reconnect-scan restart was
correctly gated on `!wardriving_ble_active` (the "merged-reconnect-scan" invariant,
`docs/PLAN.md`'s "Revised long-run reconnect policy"), but `start_scan()`'s two other callers
— `BLE_GAP_EVENT_DISCONNECT`'s reconnect paths and `reconnect_task()` — were not, so a
reconnect landing while wardriving's BLE capture was active raced `wardriving_ble_interval_cb()`'s
own `ble_gap_disc()` re-arm and lost. **A cross-cutting invariant enforced at only one of
several call sites is not enforced** — moved the guard inside `start_scan()` itself so every
caller is covered by construction. Also hardened `wardriving_ble_interval_cb()` to retry
after a short delay on `BLE_HS_EBUSY` specifically (a connect attempt from `gap_event()`'s own
merged-reconnect match can transiently own GAP master state) instead of treating it as fatal,
per the general principle that a `BLE_HS_EBUSY` from a resource genuinely owned by *this
firmware's own code* is a coordination bug to fix, not a peer condition to give up on.

Fixed 2026-09-10, build-, host-test, and hardware-verified (see `docs/PROJECT_HISTORY.md`'s
"wardriving hardware-verified" entry).

### efficiency-fix-the-constraint-not-the-symptom

The 64-byte Write characteristic means each record costs ~4x the ATT round trips it needs at
the negotiated 256-byte MTU, and fragments are written sequentially (each waiting on the prior
write-completion callback), so latency is round-trips × connection interval —
`pair_init` took ~110 ms across 3 fragments. Irrelevant for a once-per-reset ceremony; it will
matter for bulk transfers like wardriving. The right fix is raising the Flipper's
characteristic declaration (a cross-firmware change), **not** switching to
write-without-response, which would give up the ordered reliable delivery the framing layer
assumes. Before optimizing a transfer path, check whether a declared limit is the real cost.

---

## Flipper-specific

### ble-event-worker-stack-budget

`profile_event_handler` runs synchronously on the `"BleEventWorker"` FuriThread, allocated
`furi_thread_alloc_ex("BleEventWorker", 1280, ...)` in
`targets/f7/ble_glue/ble_event_thread.c`. Everything reachable from it shares that one
1280-byte budget: fragment reassembly, CBOR decode, the pairing handlers, and all of
`pairing.c`/`pairing_crypto.c` beneath them. This has caused three separate crashes.

- **Nesting is what kills, not any single frame.** The X25519 crash was `cmult()` (1216 bytes)
  calling `fmonty()` (1224 bytes) from inside its own 256-iteration ladder loop — each frame
  is survivable alone, nested they were ~2440 bytes, nearly 2x the entire stack. Add up the
  whole chain, don't spot-check individual functions.
- **Host tests are structurally blind to this.** MSVC on a desktop has a megabyte-plus stack,
  so `build_pairing.ps1` passes 67/67 regardless of whether the code fits on the Flipper.
  Never cite a passing host test as evidence of stack safety.
- Converting a local to `static` changes initialization semantics: an `= {0}`/`= {1}`
  initializer then runs **once at program load**, not per call. Add an explicit reset at the
  top of the function for anything that depended on it — a real trap hit in `cmult()`.
- **A recursive validator's stack cost scales with its depth budget, not its named locals** —
  the "grep for large local arrays" heuristic is blind to this. `feb_cbor_skip_value()`
  recurses up to `FEB_CBOR_MAX_NESTING` times per call site, and each frame is small
  individually, so no single grep hit stands out. wifi_scan (2026-09-07) gave
  `command.arguments`/`status.result` their own fresh depth-0 budget (correct for decoding,
  see `two-implementations-agreeing-is-not-two-implementations-being-right` above), but it
  means one call site can now recurse a full 5 levels deep reachable from an authenticated
  peer — estimated ~900-1100 bytes of the 1280-byte budget in the theoretical worst case, not
  measured. Before hardware-testing a change that adds or deepens a recursive validator call
  site, either measure real stack usage (`-fstack-usage` or equivalent) or say explicitly that
  you didn't and it remains a risk.

### ported-code-inherits-upstream-memory-profile

`pairing_crypto.c`'s X25519 is a port of `curve25519-donna`, deliberately kept diffable
against upstream so it stays auditable. Upstream targets hosts with huge stacks. Porting for
*structural* fidelity silently imports the *memory* profile too. When porting or updating any
third-party code into the FAP, audit its stack usage as an explicit step, separate from
checking its correctness — they are unrelated properties, and only one of them is covered by
the test vectors.

### exported-symbol-name-is-not-its-behavior

`targets/f7/api_symbols.csv` proves a symbol exists at API 88.4 and nothing more.

- `sequence_blink_start_blue` drives a **separate hardware blink subsystem**
  (`NotificationMessageTypeLedBlinkStart`), independent of the static-RGB messages
  (`sequence_reset_blue`, `sequence_set_only_blue_255`). Only `sequence_blink_stop` halts it,
  so every "LED off / LED solid" call site was silently a no-op against a running blink.
- `ble_gatt_characteristic_update()`'s `FlipperGattCharacteristicDataFixed` path always sends
  `data.fixed.length` bytes from the source pointer regardless of the real buffer size — which
  is why the Notify characteristic must stay `FlipperGattCharacteristicDataCallback`. Don't
  "simplify" it back.

Read the implementation (`applications/services/notification/notification_messages.c`,
`targets/f7/ble_glue/furi_ble/gatt.c`, etc.) for any state-changing API, and for every
`*_start` locate its matching `*_stop` rather than assuming a later call overrides it.

### app-data-path-resolves-per-calling-thread

Confirmed in `applications/services/storage/storage_processing.c`: `"/data"` resolves to
`/ext/apps_data/<app id of the calling thread>/...`. `handle_pair_complete()` (and therefore
`pairing_storage_save()`) runs synchronously inside `profile_event_handler()` on the BLE
stack's own `BleEventWorker` thread — owned by the firmware's built-in `bt` service, not this
app. The pairing secret was found persisted at `/ext/apps_data/bt/pairings/<board_id>.dat`
instead of `/ext/apps_data/flipper_esp32_over_ble/pairings/<board_id>.dat`: silently wrong
directory, not a crash, so it passed every check that didn't specifically look at the actual
path. Any `APP_DATA_PATH(...)` call made from `profile_event_handler` or anything it calls
resolves against the wrong app. Resolve/cache the real path once from this app's own thread
(e.g. at app init) and pass the resolved path into BLE-callback code — never call
`APP_DATA_PATH` from inside it directly. This is the storage-API sibling of the stack-budget
rule above: `BleEventWorker` is a shared, borrowed thread, and it changes the meaning of any
API that resolves per calling-thread identity, not just what fits on its stack.

### ui-terminal-state-is-not-a-torn-down-profile

The "paired" success screen was added without re-examining whether the underlying custom BLE
profile still advertises/accepts connections afterward. It does: an app left open on that
screen silently completed a second full pairing ceremony (fresh secret, no OK-press, no user
interaction at all) when a nearby ESP32 reset and opened a window — discovered by accident via
an unrelated `esptool` side-effect reset. Screen state answers "what does the user see," not
"what can a peer still do to this connection." Whenever a new terminal/success state is added
to the pairing (or any future) state machine, explicitly decide and test whether the
underlying profile should tear down/stop advertising there.

### gatt-characteristic-length-is-a-wire-constraint

`PAYLOAD_MAX` (64) is the Write characteristic's declared max attribute value length. A GATT
characteristic's declared length caps every write to it independently of the negotiated ATT
MTU — a larger write fails with ATT error 0x0D no matter how much MTU headroom exists. The
ESP32 must size its outgoing fragments against this number, so changing it is a cross-firmware
change: update the ESP32's `FEB_FLIPPER_WRITE_CHAR_MAX_LEN` in the same breath, or don't change
it at all. Any Flipper-side value the peer must know is a shared contract even when it doesn't
live in `framing.h`.

### ui-must-derive-from-real-state

The second status line was hardcoded `"No saved pairing"` — a claim that could never become
true — until it was replaced with a real `any_saved_pairing_exists()` check. A displayed state
must be derived from actual state, not asserted.

### wrap-silent-failure-apis-in-positive-confirmation

`ble_gatt_characteristic_init()` returns `void` and never propagates failure, and
`aci_gatt_add_char()` returns early *without writing* `*Char_Handle` on a non-zero status —
over a `malloc`'d (not `calloc`'d) profile struct, that leaves a garbage handle rather than an
obvious zero. Prefer `calloc` for structs whose fields are only conditionally written, and log
the assigned handles so a future repro has a positive signal, not just an absent error line.

### static-buffer-pattern-trades-ram-for-stack-safety

Moving buffers to file-scope `static` is mandatory on the BLE event path (see
`ble-event-worker-stack-budget` above), but it converts transient stack into permanently
resident `.bss` on a device where RAM is genuinely tight. Size each static to the maximum
actually reachable, not to the largest convenient `FEB_*` constant; keep them file-local
(`static`, never exported); and as this set grows, consider whether mutually-exclusive buffers
can share one arena rather than each reserving its own worst case.

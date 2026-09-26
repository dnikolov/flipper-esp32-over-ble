# Hardening backlog

Separate from [BACKLOG.md](BACKLOG.md): this file tracks deeper structural/robustness issues
found during live testing that are not quick fixes and need their own investigation/design pass
before being implemented — as distinct from BACKLOG.md's mix of ready-to-fix bugs and deferred
product decisions. An item moves here when it's confirmed real but deliberately not actioned
immediately; it moves back out (with a `docs/PROJECT_HISTORY.md` entry) once fixed and verified.

## H01 — Wardriving's periodic BLE re-arm can collide with its own in-flight connect attempt

**Discovered:** 2026-09-13, during a live forced-disconnect/reconnect test of the BL07/G30/G12/G19/G21
fix batch (`docs/BACKLOG.md`). Confirmed via ESP32 serial log
(`logs/esp32_COM9_2026-09-13_10-17-34.log`), not yet fixed.

**Not caused by today's fixes** — `wardriving_ble_interval_cb()` (`esp32/main/main.c`) was not
touched by any of BL07/G30/G12/G19/G21. This is a separate, pre-existing gap in the "merged
reconnect scan" design (docs/PLAN.md step 2's "Revised long-run reconnect policy") that this
specific live test happened to expose. It's timing-dependent/probabilistic — consistent with
earlier tests sometimes showing clean reconnects (e.g. the 7/7 BLE-only isolation test that
resolved G36) and sometimes not.

**Root cause:** `wardriving_ble_interval_cb()` re-arms BLE discovery on its own independent
~500ms timer (`wardriving_ble_interval_co`), with **no check for whether a connect attempt is
already in flight**. Exact sequence captured live:

```
10:23:43.684  GAP: discovery starts
10:23:43.685  "found v2 peer, connecting" -> connect attempt #1 begins
10:23:44.440  GAP: discovery starts AGAIN (756ms later -- before attempt #1 resolved!)
10:23:44.652  "found v2 peer, connecting" -> connect attempt #2
10:23:44.652  "connect start failed: 6"   <- attempt #2 collides with attempt #1
10:23:44.653  reconnect retry 1/5 scheduled (schedule_reconnect(), see note below)
10:23:44.653  NimBLE auto-reattempts connection #1 (reason 0x3e = supervision timeout)
10:23:45.411  GAP: discovery starts AGAIN -- collides with the reattempt too
```

When a peer match happens right as the independent interval timer is about to fire again, the
periodic re-arm restarts discovery mid-connect, killing the very connection attempt it just
triggered. This cascades: the interrupted connect fails, NimBLE tries to auto-reattempt, the
*next* interval tick collides with that reattempt too -- the discovery loop and the connect
attempt now permanently fight each other every ~500ms, never letting a connection land.

**Secondary note (not the root cause, but adds confusion when reading logs):** the
`schedule_reconnect()`/`reconnect retry N/M` backoff path also fires here (triggered by
`ble_gap_connect()` itself returning non-zero), but since `wardriving_ble_active` is true, that
backoff path's eventual `start_scan()` call is a no-op (gated by `wardriving_ble_active`) --
the real driver of the observed loop is `wardriving_ble_interval_cb()`'s own cycle, not this
backoff path. The `reconnect retry` log line is a red herring for diagnosing this specific
failure mode; don't chase it as the primary lead.

**Proposed fix (not yet implemented):** add a guard so `wardriving_ble_interval_cb()` skips
re-arming discovery while a connect attempt is already pending -- e.g. track a new
"connect attempt in flight" flag (set when `BLE_GAP_EVENT_DISC`'s match branch calls
`ble_gap_connect()`, cleared on the corresponding `BLE_GAP_EVENT_CONNECT`, success or failure)
and have the interval callback no-op (rescheduling itself for the next window) while that flag
is set, mirroring how `BLE_GAP_EVENT_DISC_COMPLETE` already guards against `start_scan()`
running concurrently with `wardriving_ble_active`.

**Needs before implementing:**
- Confirm this reproduces reliably enough to verify a fix against (it's timing-dependent).
- Decide whether the same guard should also apply to the non-wardriving `start_scan()` path
  (its own reconnect scan) for symmetry, even though that path already isn't reachable
  concurrently with wardriving's BLE source today (`BLE_GAP_EVENT_DISC_COMPLETE` already skips
  it while `wardriving_ble_active`).
- A live retest after the fix, same forced-disconnect-during-active-wardriving scenario.

**Severity:** P1-equivalent in practice -- this is the actual mechanism behind the still-unresolved
tail of G36/BL07 (wardriving BLE reconnect stall) that "Wi-Fi coexistence starvation" was
believed to fully explain. That theory may still be a contributing factor in other captures:
this file doesn't rule it out, it just proves at least one independent failure mode exists that
has nothing to do with Wi-Fi at all.

**Confirmed in the field, 2026-09-16:** a real unattended overnight/1-day wardriving run (with
autostart, see `docs/BACKLOG.md`'s "Add wardriving autostart and boot-button toggle") built up a
16,838-record backlog that did not drain on its own even with the Flipper back in range --
consistent with this bug's "permanently fight each other" description, not the milder
"sometimes clean, sometimes not" case. **A full ESP32 reboot (reset button) recovered it and the
drain proceeded normally afterward.** This is the practical workaround until a real fix lands:
if a backlog is not draining after the Flipper is confirmed in range and connected, reboot the
ESP32 rather than waiting further.

Note this happened despite `wardriving_ble_interval_cb()` already having a partial mitigation in
place (`esp32/main/main.c`, added in the 2026-09-16 autostart commit `4cd6c7d`): it backs off and
retries the same window instead of erroring out when `ble_gap_disc()` returns `BLE_HS_EBUSY`
(GAP master busy, e.g. a connect attempt in flight). That guard prevents the *immediate*
`wardriving_self_stop("internal_error")` teardown this function would otherwise hit, but does not
implement H01's actual proposed fix (a flag that skips *re-arming discovery* while a connect is
in flight) -- so the underlying race this section describes is still open. This field case is
evidence the `EBUSY` backoff alone is not sufficient to prevent the stall; the "connect attempt
in flight" guard proposed above is still the needed fix, not just a nice-to-have.

**Follow-up needed:** capture a live serial log (`tools/build_esp32.ps1 -CaptureBootLog` or an
`idf.py monitor` session) the next time this reproduces, before rebooting, to confirm this is
still the same H01 mechanism and not a new one, and to check whether the wardriving flash log's
capacity was also a factor (see the new BACKLOG.md item on flash-log capacity vs. multi-day
autostart accumulation, added the same day).

## H02 — Live concurrent-load test for the G30 wardriving-log race fix

**Status:** fix implemented and build-verified 2026-09-13 (see `docs/BACKLOG.md` G30), not yet
hardware-tested under the specific condition it was meant to fix.

**What G30 changed:** the wardriving Wi-Fi-source dedup/append loop moved from the `sys_evt`
task (`wifi_scan_done_handler()`) to the NimBLE host task (`wifi_scan_done_cb()`), eliminating a
cross-task race against `wardriving_send_next_batch()`'s log reads. The BLE-source path was
already safe; this made Wi-Fi symmetric with it.

**What still needs to happen:** a live test where the ESP32 is actively appending *new* Wi-Fi
wardriving records to the flash log **at the same moment** the Flipper is draining a backlog of
*previously buffered* records — the exact overlap the race depended on. The cleanest way to
force this overlap:

1. Start wardriving with both Wi-Fi and BLE sources.
2. Disconnect the Flipper for 30-60s so a backlog builds up while Wi-Fi/BLE capture keeps running.
3. Reconnect — the backlog drain will begin while live Wi-Fi captures are still landing.
4. Watch the ESP32 serial log for: no `wardriving_log_mark_drained(...) exceeds last peek's ...
   result; clamping` warnings, no duplicate/lost records in the Flipper's WiGLE CSV, and a
   backlog count that decreases monotonically rather than jumping erratically.

**Known complication:** this exact test scenario (forced disconnect during active wardriving) is
also where H01 currently causes a reconnect stall — H01 needs to be fixed first, or the test
needs to tolerate/work around it, before G30's fix can actually be exercised end-to-end this way.

## H03 — G07 reproduces on the reconnect handshake itself, not just `start`/`stop`

**Discovered:** 2026-09-13, same live-test session as H01. Confirmed via ESP32 serial log
(`logs/esp32_COM9_2026-09-13_10-17-34.log`). `docs/BACKLOG.md` G07 ("Any `send_protected*`
clobbers an in-flight wardriving backlog drain") was already an open, explicitly-deferred
finding; this is fresh evidence of a trigger path not previously documented, not a new bug.

**Evidence — full sequence from the live log:**

```
10:33:28.157  idle-timeout disconnect (normal -- 30s no traffic)
10:33:28.159  reconnect: found peer, connecting
10:33:28.495  connected, MTU negotiated: 256
10:33:29.089  notifications subscribed, hello sent
10:33:29.245  hello_ack received, client_auth sent
10:33:29.297  "runtime session authenticated"           <- clean auth
10:33:29.351  sending wardriving status(data), 1 backlog record
10:33:29.395  received 2 fragments from Flipper (auto wardriving-status query, per BL02)
10:33:29.395  "wardriving status query answered"        <- a SECOND protected record, sent
              almost simultaneously with the first
10:33:29.592  one more GATT write (27 bytes)
10:33:29.848  discovery re-arms (wardriving's own BLE window cycling -- unrelated)
10:33:29.851  "disconnected: reason=531"
```

`reason=531` decodes to NimBLE-base(512) + HCI 0x13 ("Remote User Terminated Connection") -- the
**Flipper** closed the connection, not the ESP32.

**Diagnosis:** right after auth completes, two protected records get queued almost back-to-back:
the ESP32's wardriving backlog-drain data, and its answer to the Flipper's automatic
wardriving-status query (a BL02 behavior -- the Flipper queries running-state on every fresh
session-auth). This is the shared single-in-flight `tx_fragment_*` state getting clobbered by
the second send racing the first (G07's known mechanism), corrupting one of the records. The
Flipper then detects a bad sequence/GCM tag on the corrupted record and -- correctly, per G11's
fix -- disconnects rather than silently accepting it.

**New trigger path this adds to G07's existing scope:** previously tracked as `start`-only, then
generalized to `stop`/`capability_query`. This adds **the reconnect handshake itself**
(wardriving-status-query-answer racing the backlog-drain data send) as a fourth confirmed
trigger -- meaning G07 can now cause a full disconnect/reconnect/immediate-re-disconnect loop
any time wardriving is running and the Flipper reconnects with backlog pending, independent of
any explicit `start`/`stop`/`capability_query` action.

**Status:** still deferred per the user's standing decision (see `docs/BACKLOG.md`'s "Deferred
by explicit product decision" section) -- this evidence does not change that decision, it just
means the deferred item's practical impact is broader than previously documented (routine
reconnect-with-pending-backlog, not just explicit commands). Re-confirm the deferral still
stands, now with this fuller picture, before it comes up again.

## H05 — RESOLVED 2026-09-26, see docs/PROJECT_HISTORY.md

Wardriving-record transfer to the Flipper was slow. Root-caused to a fixed 64-byte Flipper
Write-characteristic attribute length capping every fragment regardless of negotiated ATT MTU,
plus default (untuned) BLE connection parameters. Fixed via connection-parameter tuning (2M
PHY + tighter interval, ESP32-C5-only) and raising the fixed chunk size to 244 bytes
(cross-firmware, via a new shared `FEB_WRITE_CHAR_MAX_LEN` constant) — **not** via
write-without-response, which this project had already ruled out for this exact reason (see
`docs/LESSONS.md`'s "efficiency-fix-the-constraint-not-the-symptom"). Build/host-test verified
on both firmwares; hardware verification still pending. Full investigation and implementation
narrative in `docs/PROJECT_HISTORY.md`'s "2026-09-26: ESP32-C5-to-Flipper wardriving transfer
throughput fix" entry.

**Known follow-up, not yet decided:** `esp32/` (C6) and `heltec/` still hardcode the old
64-byte cap — only `esp32c5/` was ported this pass.

<details>
<summary>Original investigation (kept for the reasoning trail; superseded by the resolution above)</summary>

**Discovered:** 2026-09-26, from a user-reported symptom during a live wardriving session
("transfer to the flipper is very slow"). An external code-review agent audited
`esp32c5/main/main.c` and flagged four throughput bottlenecks; each was independently
re-verified against the actual code (not taken on the review's word) before acting on any of
it. Two turned out to be safe, connection-parameter-only tuning with no protocol impact
(implemented directly, not logged here — 2M PHY request + tighter connection interval on
connect). The other two are logged here because they are a real wire-protocol change, not a
quick patch, and one of them reproduces a bug class this project has already been bitten by
once.

**What the review got right:** the ESP32 currently pushes each wardriving-record fragment via
`ble_gattc_write_flat()` (GATT Write **with** response) in `send_next_tx_fragment()`
(`esp32c5/main/main.c`), blocking on the peer's ATT ack before sending the next fragment — one
fragment per round trip, capping throughput to roughly one fragment per connection interval.
Fragment size is also capped well below what the negotiated ATT MTU could carry.

**Why this isn't a quick fix — two compounding reasons:**

1. **Write-without-response needs real flow control, not a recursive fire-all.** The review's
   proposed replacement (`ble_gattc_write_no_rsp_flat()` called recursively for every fragment
   with no backpressure) will overrun NimBLE's host TX queue under real load — a full
   record-write burst has no acknowledgment to gate on, so the sender must check the return
   code and retry/back off when the host queue is full, not assume every call succeeds. It
   also breaks this file's existing per-fragment completion bookkeeping
   (`tx_dispatching_completion`, `wardriving_tx_in_flight`, `write_complete()`'s single call per
   real GATT event) if `write_complete()` is faked instead of driven by a real completion event.
   Switching the Write direction to write-without-response is also a **Flipper-side firmware
   change**, not ESP32-only: the Flipper's Write characteristic is currently declared
   `CHAR_PROP_WRITE` only (`flipper/flipper_esp32_over_ble.c:580`) — no
   `CHAR_PROP_WRITE_WITHOUT_RESP` bit. That property combination does exist and is used
   elsewhere in the pinned Unleashed checkout (`targets/f7/ble_glue/services/serial_service.c`
   combines both), so it's feasible, but it's a real change on both sides, not a config flip.
   `docs/PROTOCOL.md`'s Fragmentation section (`docs/PROTOCOL.md:25`) also explicitly documents
   the current one-byte fragment-header field sizing as relying on "write-with-response and a
   single active BLE connection mean only one message is ever being reassembled per direction at
   a time" — that reasoning needs to be re-examined against a no-response transport, not
   silently invalidated.

2. **The 64-byte write-chunk cap is not a client-side buffer size — it's the peer's declared
   GATT attribute length, and this exact bug class has already been hit once.**
   `FEB_FLIPPER_WRITE_CHAR_MAX_LEN` (`esp32c5/main/main.c`, currently 64) has to match
   `PAYLOAD_MAX` (`flipper/flipper_esp32_over_ble.c:33`, also 64), which is the Flipper's Write
   characteristic's own *fixed declared value length*
   (`.data.fixed.length = PAYLOAD_MAX`, `flipper/flipper_esp32_over_ble.c:576`). A write larger
   than the characteristic's own declared max fails with `ATT_ERR_INVALID_ATTR_VALUE_LEN`
   independent of how much headroom the negotiated ATT MTU has — this is precisely
   `docs/LESSONS.md`'s "att-mtu-vs-attribute-length" entry, which already documents this exact
   failure shooting live once and explicitly warns: *"don't 'improve' this back to the raw
   MTU."* Raising the chunk size for real throughput gain means raising `PAYLOAD_MAX` on the
   Flipper side by the same amount, in lockstep, and re-verifying on hardware — a one-file
   `#define` bump on the ESP32 side alone reproduces the exact regression that lesson warns
   against.

**Proposed fix (not yet implemented):**
- Re-derive `docs/PROTOCOL.md`'s fragmentation-layer ordering/multiplexing assumptions for a
  no-response transport before changing any code, since the one-byte fragment-header field
  sizing rationale is written against write-with-response specifically.
- Add real flow control to the ESP32 send path for write-without-response (queue depth limit,
  retry-on-`ENOMEM`-equivalent, driven by NimBLE's actual buffer-availability signal — not a
  recursive fire-all).
- Raise `PAYLOAD_MAX` (Flipper) and `FEB_FLIPPER_WRITE_CHAR_MAX_LEN`
  (`FEB_FLIPPER_WRITE_EFFECTIVE_MTU`) (ESP32) together, add `CHAR_PROP_WRITE_WITHOUT_RESP` to
  the Flipper's Write characteristic, and hardware-verify both directions at the new size
  before calling it done — per this project's two-firmwares-in-lockstep convention.

**Needs before implementing:**
- A decision on target chunk size (review suggested 244, comfortably inside standard 247-byte
  ATT MTU) and confirmation the Flipper's `FlipperGattCharacteristicDataFixed` path and BLE
  stack handle a value that size without other side effects (the Notify characteristic's own
  `FEB_NOTIFY_CHAR_EFFECTIVE_MTU` mirrors the same fixed-length constraint in the other
  direction — check whether it needs a matching change or can stay independent).
- Hardware verification plan for both the flow-control change and the attribute-length change,
  given the attribute-length change alone has a documented one-shot precedent for shipping
  clean and failing only once a real oversized write is exercised
  (`docs/LESSONS.md:117-121`) — a host test pinned to a conservative fragment size will not
  catch a regression here; the test suite needs to also exercise the new real chunk size.

**Severity:** P2 — a real, user-observed performance problem (not a correctness bug), but the
fix is a wire-protocol change with a documented precedent for shipping broken if rushed. Not
blocking correctness or safety; the ESP32-only connection-parameter tuning implemented
alongside this entry provides a lower-risk partial improvement in the meantime.

</details>

## H04 — Flipper app intermittently fails to launch with an OOM message (Flipper reboots)

**Discovered:** 2026-09-13, from a live user-reported symptom ("app often cannot start causing
flipper restart and an OOM message"), investigated by reading the actual Unleashed FAP loader
source and measuring the built artifact rather than guessing.

**Root cause mechanism (confirmed):** an external FAP is not linked into a fixed memory layout
like normal firmware -- it's an ELF loaded at runtime by
`lib/flipper_application/elf/elf_file.c`. Every allocatable section (`.text`/`.rodata`/`.data`/
`.bss`) gets its own `aligned_malloc()` from the *live Flipper system heap* at launch
(`elf_file.c` ~line 488), and the loader explicitly checks `memmgr_heap_get_max_free_block()`
against each section's size first (~line 482) -- a **contiguous free block** requirement, not
just total free bytes. So this app's static scratch buffers are not "free" the way they would be
in normal firmware; they're an unconditional contiguous-heap demand at every launch, competing
with whatever the rest of the firmware (GUI, BT stack, prior apps' heap fragmentation) already
holds. That explains the "often" (not "always") character of the failure -- it depends on
current heap fragmentation, not on anything this app does at runtime.

**Measured (this session, via `arm-none-eabi-size`/`arm-none-eabi-nm` against the real built
`flipper_esp32_over_ble_d.elf`):**

```
.text   56840
.rodata 10336
.data      56
.bss    44812   <- one contiguous malloc, checked against max-free-block at load
```

`.bss` breakdown, largest symbols:

| Symbol | Bytes | Note |
|---|---|---|
| `wardriving_dedup_table` | 8200 | see below |
| 16x duplicate `static AppEvent event` locals | 8320 total | fixed this session, see `docs/PROJECT_HISTORY.md` |
| `result.21` | 2832 | capability-response-shaped decode scratch, needs investigation |
| `wifi_scan_aps` | 2560 | scan-results display scratch |
| `ble_scan_devices` | 1664 | scan-results display scratch |
| `result.5` / `result.9` | 1544 / 1288 | per-capability decode scratch |
| 5x per-capability cmd buffer sets | ~4340 total | fixed this session, see `docs/PROJECT_HISTORY.md` |
| X25519 scratch (`fmonty`/`cmult`/`crecip`/scalarmult) | 3600 | deliberately excluded, see below |

**Fixed this session (mechanical, zero-behavior-change, safe by this file's own established
single-in-flight/synchronous-BLE-dispatch rationale):** the 16 duplicate `AppEvent` locals, the 5
duplicate per-capability `{payload,ciphertext,record}` buffer sets, and the 4 per-capability
decode-scratch `result` structs (wifi_scan/ble_scan/gps/wardriving status handlers), each
collapsed to one shared instance (the last of these via a `union`, since the four are different
struct types -- sized to the largest member, wardriving's, not the sum of all four). Measured
`.bss`: 44812 -> 35864 (AppEvent + cmd buffers) -> 32972 (+ result-struct union) -- a total
reduction of 11840 bytes, ~26%. See `docs/PROJECT_HISTORY.md` for the full verified numbers.

**Also fixed, same session:** `wardriving_dedup_table` split into independent Wi-Fi (48-slot) /
BLE (96-slot) sub-tables instead of one shared 256-slot ring (the ESP32's own independent dedup
layer, `esp32/main/wardriving_dedup.c`, already uses two separate tables at the same 1:2 ratio, for
the same reason: BLE's faster churn was evicting still-relevant Wi-Fi entries out of a shared
ring, causing avoidable duplicate CSV rows). This combined the split with a capacity shrink
(256 -> 144 total entries), cutting this table from 8200 to 4624 bytes. `.bss`: 32972 -> 29400.
Zero caller changes needed (same outer struct/function signatures); a new host test
(`test_wardriving_dedup_wifi_survives_ble_eviction`) proves Wi-Fi entries now survive BLE-table
churn. See `docs/PROJECT_HISTORY.md` for the full writeup.

**Total so far:** `.bss` 44812 -> 29400 (-15412 bytes, ~34%).

**Still open, needs its own design pass before fixing:**

1. **`wifi_scan_aps` / `ble_scan_devices` (~4.2 KB combined)** -- the scan-results screens' backing
   display arrays (`WifiScanApDisplay`/`BleScanDeviceDisplay`), main-thread-owned and long-lived
   for as long as a results screen is on-screen -- a different ownership/lifetime category from
   the decode-scratch `result` structs above (which were BLE-thread-only, single-call, already
   fixed). Not touched this session because the screen-transition/`pending_command_kind`
   busy-gating state machine needs to be traced first to confirm a new capability's incoming data
   can never land in the array while the *other* capability's results are still being displayed --
   get that wrong and the failure mode is a live UI glitch (wrong/garbage rows on screen), not a
   build error, so this is medium-to-higher risk and the smallest remaining win. Do this one last,
   if at all.

**Explicitly out of scope, decided this session, not just deferred:** the X25519 scratch
(`pairing_crypto.c`'s `fmonty`/`cmult`/`crecip`/`x25519_donna_scalarmult`, 3600 bytes total) and
`framing.c`'s `frag_buf` (772 bytes). Both are deliberately `static` to prevent a documented,
previously-hit-four-times stack-overflow bug class on the 1280-byte `BleEventWorker` thread (see
`docs/LESSONS.md`'s "any buffer >=100 bytes reachable from `BleEventWorker` must be `static`"
rule), and `pairing_crypto.c`'s own file header states that matching upstream curve25519-donna
line-by-line for audit purposes is a deliberate tradeoff worth more here than space savings. At
3600 bytes combined -- the smallest of the categories above, not the largest -- there's no case
for reopening either the stack-overflow risk or the audit-diffability tradeoff to chase it.

**Severity:** P1-equivalent -- this is a full app-unusable-until-reboot failure, not a cosmetic or
edge-case bug, and it's user-visible ("often"). The mechanical fixes this session reduced `.bss`
by 15412 bytes (~34%, confirmed via `arm-none-eabi-size`); item 1 above is the next lever if
launch failures are still observed after that -- the actual OOM-frequency improvement on real
hardware still needs to be observed in the field, this session's verification was build+static
only (no flashing).

**2026-09-24: investigated the sharper "especially on second launch" symptom -- no genuine
heap leak found; one real redundancy found and fixed; `.bss` re-measured (grew back somewhat
since the 2026-09-13 fix, as expected from new features added since).**

**Re-measured `.bss`** via `arm-none-eabi-size`/`arm-none-eabi-nm` against a fresh build:
29400 (2026-09-13 baseline) -> 32636 bytes before this session's fix, i.e. it had grown back
+3236 bytes from GPS/publish capabilities added since (their own new static event/path/buffer
fields) -- confirms the file's own warning that `.bss` "needs re-auditing on every growth, not
just when first written."

**Exit/cleanup path traced end-to-end, found correctly paired -- no leak located in this
FAP's own code:**
- Every `storage_file_alloc()` site (9 call sites) has a matching `storage_file_close()` +
  `storage_file_free()` on every path, including early-return/error paths (checked all 9
  individually: `pairing_storage_save/load`, `any_saved_pairing_exists`,
  `capability_storage_save/load`, `wardriving_settings_load/save`,
  `wardriving_csv_ensure_open`, `publish_try_read_result`).
- The one `malloc()` site in the whole file (`profile_start()`'s
  `malloc(sizeof(Esp32BleProfile))`) is freed on both its own error path and in `profile_stop()`.
- `stop_service()` (called both on explicit Back-exit and on `BtStatusUnavailable`, matching
  this file's own established rule) calls `bt_profile_restore_default()`, which -- traced into
  the pinned Unleashed checkout's `bt_api.c`/`bt.c`/`furi_hal_bt.c` -- always routes through
  `furi_hal_bt_reinit()`, which calls `current_profile->config->stop(current_profile)`
  (our `profile_stop()`) before restoring the default Serial profile. `profile_stop()` in turn
  unregisters the GATT event handler, calls `ble_gatt_characteristic_delete()` for all 6
  characteristics, `ble_gatt_service_delete()`, and `free(profile)` -- symmetric with
  `profile_start()`. `ble_gatt_characteristic_init()`/`_delete()` (`targets/f7/ble_glue/
  furi_ble/gatt.c`) each also malloc/free their own heap copy of the characteristic descriptor
  per characteristic -- also symmetric, not a leak, given clean teardown.
- No app-spawned `FuriThread`s to leak (grepped, none found -- this app is single-thread +
  BLE/timer/GUI callbacks only).
- The one GATT characteristic using the `FlipperGattCharacteristicDataCallback` path
  (`notify_data_callback`) always returns `release_data = false`; no `descriptor_params` are
  used anywhere -- rules out the other realistic per-characteristic leak shape (a callback that
  mallocs data for the GATT stack to send and forgets to signal ownership correctly).
- The publish flow's BadUSB USB-personality switch (`publish_trigger_badusb()`) is a stack-local
  pointer swap (`furi_hal_usb_set_config`), not a heap allocation; runs on the main thread only.

**Conclusion on the "especially second launch" mechanism:** given the above, this is not
explained by a code-level leak in this FAP or in the firmware's normal profile-teardown path.
The much more likely mechanism -- consistent with, not contradicting, H04's own existing
fragmentation framing above -- is heap **fragmentation** carried over from the first launch's
own churn: `bt_profile_restore_default()` does a full BLE-core reinit and re-starts the
default Serial profile (which re-adds its own GATT characteristics/heap copies), and the app's
own Storage/GUI/notification activity during the first run leaves the allocator's free-list
differently shaped than it was at boot, even with every individual allocation correctly freed.
A launch immediately after that first run's churn is competing for a contiguous block against a
*more fragmented* heap than a first launch fresh off a reboot would. **This remains an
unconfirmed hypothesis** -- this session had no hardware access; confirming it would need
instrumenting `memmgr_heap_get_max_free_block()` before/after a first and second launch on real
hardware (not done this session, flagged for whoever picks this up next).

**Fixed this session (mechanical, safe, zero behavior change):**
`gps_poll_timer_callback` and `publish_poll_timer_callback` each kept their own separate
`static AppEvent event` on the stated rationale that they "run on the Furi timer-service thread
... concurrently" -- but both in fact run on the *same single* FreeRTOS Timer Service task
(`configTIMER_TASK_STACK_DEPTH`), which processes one expired-timer callback at a time from its
own queue, so the two can never be in flight concurrently with *each other* (only with
BleEventWorker/Bt/GuiSrv, which they don't share a buffer with anyway). Merged into one shared
`timer_service_event`, same single-in-flight rationale as `shared_ble_event`. `.bss`: 32636 ->
32068 (-568 bytes). Build-verified via `fbt.cmd fap_flipper_esp32_over_ble`
(`flipper_esp32_over_ble.fap`, 133052 bytes); not hardware-tested.

**Deferred item (`wifi_scan_aps`/`ble_scan_devices` merge) -- traced as this file asked,
still not safe to do:** confirmed both arrays are already main-thread-owned (written only from
the main loop's `AppEventWifiScanAp`/`AppEventBleScanDevice` handlers, never directly from
BleEventWorker) and that `Left`/`Right` scan triggers are gated to the Home screen only, so a
second scan can't be started without first returning Home (which calls
`reset_scan_ui_state()`, itself zeroing both counts and `pending_command_kind` together). *But*
the `AppEventWifiScanAp`/`AppEventBleScanDevice` handlers themselves write into their array
unconditionally, with no `pending_command_kind`/screen check -- so a wifi_scan record already
in flight (queued by BleEventWorker before the user backs out) can still land after the user has
returned Home and started a *different* capability's scan. Today this is harmless only because
the two arrays are separate (a late wifi_scan write lands in `wifi_scan_aps`, not wherever
`ble_scan_devices` is being displayed). A union/shared-array merge would reintroduce exactly the
cross-capability corruption risk this file already flagged -- unsafe without first adding an
explicit `pending_command_kind` (or a scan-generation counter) check inside both event handlers
themselves, which is its own change with its own risk of dropping a still-legitimately-in-flight
record if the check is too strict. Left deferred, per this file's own "do this one last, if at
all" -- not attempted this session.

**2026-09-24: investigated a new user-reported correlation -- "crashes on launch more often
when plugged into USB, especially when a PC-side tool (qFlipper/a terminal/`scripts/storage.py`)
has the port actively open." Confirmed a plausible, source-level direct mechanism, not just a
coincidental correlation. Read-only investigation; no hardware access, no code change.**

Traced the pinned Unleashed checkout's USB CDC/CLI stack
(`applications/services/cli/cli_vcp.c`, `lib/toolbox/cli/shell/cli_shell.c`,
`lib/toolbox/pipe.c`, `targets/f7/furi_hal/furi_hal_usb_cdc.c`, `furi/core/thread.c`):

- **USB plugged in, no host tool has the port open:** `cdc_init()`
  (`furi_hal_usb_cdc.c`) mallocs two small USB string descriptors (product/serial name, each
  `strlen * 2 + 2` bytes, well under 100 bytes combined) whenever the CDC interface is the
  active USB personality -- which it is by default whenever USB is plugged in and no other
  app/profile has taken over the USB personality. Real, but small.
- **A PC-side tool actively has the port open (DTR asserted):** this is the qualifier the user
  singled out, and it maps directly onto a specific, much larger, code path.
  `cli_vcp_cdc_ctrl_line_callback` fires `CliVcpInternalEventConnected` when the CDC control line
  state's DTR bit goes active -- i.e. exactly when a terminal, qFlipper, or this project's own
  `scripts/storage.py` (drives the same VCP/RPC session over the port) opens the serial
  connection, not merely when USB power/charging is present. That event handler
  (`cli_vcp_internal_event_happened`, `cli_vcp.c`) allocates a `pipe_alloc(192, 1)` bidirectional
  stream-buffer pair (~500 bytes: 2x `FuriStreamBuffer`, one `PipeShared`, two `PipeSide`
  structs) and then calls `cli_shell_alloc()`, which allocates the `CliShell` struct plus a
  `FuriThread` via `furi_thread_alloc_ex("CliShell", CLI_SHELL_STACK_SIZE, ...)`.
  **`CLI_SHELL_STACK_SIZE` is 4096 bytes** (`lib/toolbox/cli/shell/cli_shell.h`), and critically
  `furi_thread_alloc_ex`/`furi_thread_set_stack_size` allocates that stack with a plain
  `malloc(stack_size)` from the ordinary system heap (`furi/core/thread.c` ~line 309) -- **not**
  a separate pool/arena (contrast `furi_thread_alloc_service`, which uses
  `memmgr_alloc_from_pool` instead, for the firmware's own always-on service threads). Once the
  shell thread actually starts (`cli_shell_init`, `cli_shell.c`), it further allocates its own
  `FuriEventLoop`, an `FuriEventFlag`, a storage pubsub subscription, and
  `CliShellLine`/`CliShellCompletions`/`CliAnsiParser` state plus a command-history `FuriString`
  -- individually small (tens to low hundreds of bytes each) but additive on top of the 4 KB
  stack.
- **All of this is held resident, not transient, for the entire time the host keeps the port
  open** -- freed only on `CliVcpInternalEventDisconnected` (DTR dropped / port closed), which
  calls `cli_shell_join()` + `cli_shell_free()` + `pipe_free()`. This directly matches the "per
  command" vs. "for the whole session" distinction this investigation was asked to resolve: it's
  the latter -- a PC tool merely having the port open (idle, no commands in flight) is enough to
  keep the ~4-5 KB resident.

**Mechanism confirmed as plausible and directly relevant to H04's existing framing:** this
~4-5 KB resident allocation (dominated by the 4096-byte `CliShell` thread stack, a plain
heap `malloc`) directly reduces both total free heap and -- matching H04's own already-confirmed
mechanism above -- the single largest contiguous free block available for the FAP loader's
`memmgr_heap_get_max_free_block()` check at launch. Against this app's own largest section
(`.text`, ~56.8 KB as of the last measurement in this file), a few-KB hole taken out of the heap
by a live CLI session is a meaningful bite out of an already-tight margin, not a rounding error.
This is a genuinely different, additive mechanism from the "fragmentation carried over from a
prior app run" hypothesis logged earlier in this entry -- both can be true simultaneously (a live
CLI session narrows the margin directly; prior-run fragmentation shapes the free-list this
session's allocation then has to fit inside).

**This app's own code was checked for any USB/CLI interaction and found clean:** the only
USB touchpoint in `flipper/flipper_esp32_over_ble.c` is the publish flow's
`furi_hal_usb_set_config` HID-personality swap for BadUSB (`publish_trigger_badusb`), which runs
at runtime during an explicit user-triggered publish, not at launch, and does not allocate or
otherwise interact with anything CLI/VCP-shaped. No storage-mutex contention path was found
either (this app's own `storage_file_alloc` sites are unrelated to the CLI shell's storage
pubsub subscription, which only reacts to mount/unmount events, not per-file access).

**No code-level mitigation exists in this app for the dominant mechanism** -- the 4096-byte
`CliShell` stack is firmware behavior in the pinned Unleashed checkout, entirely outside this
FAP's control; there is nothing to change in `flipper/flipper_esp32_over_ble.c` to prevent or
shrink it. No code change made this session.

**Practical user-facing workaround (until/unless upstream firmware changes):** close any
PC-side terminal, qFlipper window, or in-progress `scripts/storage.py` session (let it finish and
release the port) before launching this FAP, especially when retrying right after a previous
OOM/reboot. When reliability matters more than USB power, prefer launching on battery with USB
fully unplugged rather than just idle-but-connected.

**Diagnostic option identified, not implemented:** `furi_hal_cdc_get_ctrl_line_state(0)` (check
the `CdcCtrlLineDTR` bit) and `furi_hal_usb_get_config()` are both exported APIs
(`targets/f7/api_symbols.csv`) this app could call at its own init to detect "USB CDC active +
DTR asserted" and log it alongside a heap-margin measurement (e.g.
`memmgr_heap_get_max_free_block()`). This would only characterize a *successful* launch's
conditions, though -- a launch that fails via the loader's own OOM path never reaches this app's
init code to log anything, so it can confirm correlation across successive successful launches
(useful evidence for the still-unconfirmed "second launch is worse" fragmentation hypothesis
above) but can't directly instrument the failure event itself. Not implemented this session
(no hardware access to validate it works as expected, and out of scope for a read-only
investigation pass) -- flagged as a possible follow-up.

**2026-09-24 addendum: real hardware repro came in, sharper than anything above -- a
monotonic 1st-launch-fast / 2nd-launch-slower / 3rd-launch-**whole-device-reboot** pattern
within one boot session, no reboot between attempts. The 3rd-launch reboot's on-screen
message was `furi_check_failed`, not an OOM/loader-rejection message -- correcting the
"probably the loader's contiguous-block check" framing this file had been assuming.
`furi_check_failed` is a Furi assertion (`furi_check()` macro) tripping somewhere, a distinct
failure class from the ELF loader's `memmgr_heap_get_max_free_block()` rejection this file's
existing entries are about. This matches the already-open `docs/BACKLOG.md` BL05
(`furi_check_failed` on relaunch), whose title says "after wardriving" -- the user's repro
didn't call out wardriving, so BL05's trigger condition may be broader than its current title;
not yet confirmed with the user either way.**

**Traced the firmware's own crash-reporting path (pinned Unleashed checkout,
`furi/core/check.c`, `targets/f7/furi_hal/furi_hal_rtc.c`/`.h`,
`applications/services/desktop/scenes/desktop_scene_fault.c`, `desktop.c`) to see what
evidence already exists for a `furi_check_failed` reboot, before writing new instrumentation
for it:**

- `__furi_crash_implementation()` (`check.c`) runs on every `furi_check`/`furi_assert`/
  `furi_crash` trip: it logs the message, full `r0`-`r11`/`lr` register dump, stack watermark,
  and heap total/free/watermark over `furi_log_puts` (serial only -- lost if nothing is
  monitoring the port), then -- in a release (`FURI_NDEBUG`) build with no debugger attached,
  which is this project's normal case -- calls `furi_hal_rtc_set_fault_data(ptr)` with the
  message pointer (falling back to a literal `"Check serial logs"` pointer if the message
  isn't a valid internal-flash address) before `furi_hal_power_reset()`. This is a real,
  already-exported (`furi_hal_rtc_set_fault_data`/`furi_hal_rtc_get_fault_data`,
  `targets/f7/api_symbols.csv`) crash-log-across-reboot mechanism, backed by an RTC backup
  register (survives the reset).
- **The firmware already surfaces this to the user, unprompted, with zero code from this
  project:** `desktop.c` checks `furi_hal_rtc_get_fault_data()` at its own startup scene logic
  and, if non-zero, jumps straight to `DesktopSceneFault`
  (`desktop_scene_fault.c`), which shows a `"Flipper crashed\nand was rebooted"` popup with
  the stored message as body text, and clears the register (`furi_hal_rtc_set_fault_data(0)`)
  only when the user dismisses it. **Practical ask for whoever reproduces this next: read and
  report the exact text of that popup** -- it's already on the Flipper's own screen, before
  any new instrumentation is needed.
- **Two real limits on how useful that popup's text will be, both confirmed by reading the
  macro expansion (`furi/core/check.h`):** (1) the common `furi_check(condition)` call form
  (no explicit message argument) passes a sentinel flag, not a string, and `check.c`
  substitutes the generic literal `"furi_check failed"` -- zero localizing information, and
  this is the common form used throughout the firmware and in this app's own code (grepped:
  every `furi_check(...)` call site in `flipper_esp32_over_ble.c` is this argument-less form).
  (2) even for a `furi_check(condition, "some message")` call *with* an explicit message, the
  popup only shows it correctly if that string lives in **internal MCU flash**
  (`check.c`'s own range check, `FLASH_BASE`..`FLASH_BASE+FLASH_SIZE`) -- true for a string
  literal inside the main firmware image, but **not** true for a string literal inside this
  FAP's own compiled `.rodata`, since an external FAP's ELF sections are loaded into
  heap-allocated RAM at runtime, not linked into internal flash. A `furi_check(..., "message")`
  call inside this app's *own* code would have its message pointer rejected by that same range
  check and silently replaced with `"Check serial logs"` -- so even if this project starts
  passing explicit messages to its own `furi_check()` calls, the on-device popup still
  wouldn't show them; only a live serial capture at the moment of the crash would.
- **Net effect:** the RTC fault-data/popup mechanism is a genuinely useful confirmation
  signal (crash class, and that a crash occurred at all) but is very unlikely to localize
  *which* check failed or where, for either the common argument-less form or (if this app's
  own code is the culprit) any explicit-message form either. The highest-value next artifact
  for actually localizing this is a **live serial log spanning the crash** (`idf.py`-style
  monitor equivalent for the Flipper -- an active `idf.py monitor`-alike CDC session, or
  `tools/build_flipper.ps1`'s existing serial tooling) captured *during* a repro of the 3rd
  relaunch, since `__furi_crash_implementation()` logs the register dump and heap stats over
  serial unconditionally, before the RTC-register/reboot path -- richer than anything the RTC
  register alone can carry. Not attempted this session (would require live hardware access
  during an in-progress crash, coordinated with the user).

**A concrete, unifying candidate mechanism, not yet confirmed:** grepped every `furi_check(...)`
call site in `flipper_esp32_over_ble.c` (all 8 are the argument-less form, see above) --
five of them are alloc-result guards run during app init, in this order:
`furi_check(reassembly_mutex)`, `furi_check(wardriving_state_mutex)`,
`furi_check(reassembly_timeout_timer)`, `furi_check(gps_poll_timer)`,
`furi_check(publish_poll_timer)` (each immediately after its own `furi_mutex_alloc()`/
`furi_timer_alloc()` call). Any of these returning `NULL` -- plausible under exactly the kind
of heap fragmentation this file's fragmentation hypothesis already describes, even with total
free bytes nominally sufficient -- would trip `furi_check_failed` immediately, with no OOM
message, no loader involvement at all (this code runs *after* the loader already successfully
placed the FAP's own sections). **This would mean the fragmentation/OOM hypothesis and the
`furi_check_failed` hypothesis are not necessarily competing explanations for two different
symptoms -- fragmentation could be the common root cause of both, manifesting as a loader
rejection on some launches and a small runtime allocation's `furi_check()` trip on others,
depending on exactly which allocation loses the fragmentation race that particular time.**
Not confirmed -- would need either a live serial capture showing the crash happened at one of
these specific call sites, or (cheaper, already covered by this session's instrumentation
below) a heap-margin trend across launches consistent with the margin getting tight enough to
plausibly explain a small `furi_mutex_alloc`/`furi_timer_alloc` failure, not just the loader's
own much larger contiguous-block demand.

**Instrumentation added this session (build-verified, not hardware-tested) to gather one round
of real launch-sequence data -- this directly serves the fragmentation/OOM hypothesis, and,
via the candidate mechanism just above, may end up bearing on the `furi_check_failed`
hypothesis too depending on what the numbers show; it does not by itself identify *which*
`furi_check()` call trips, which still needs the serial-capture approach if this candidate
mechanism doesn't pan out:**

`flipper/flipper_esp32_over_ble.c` gained a temporary diagnostic, self-evidently named for
easy removal (`h04_heap_diag_log()`, `H04_HEAP_DIAG_TEMP_FILENAME`, `h04_entry_free_heap`/
`h04_entry_max_block` locals -- grep `h04_` to find and strip every site once field data is
in). It appends one line per app launch/exit to
`/ext/apps_data/flipper_esp32_over_ble/heap_diag_TEMP_H04.log` (same `build_app_data_path()`/
`storage_file_*` convention as the existing `wardriving_publish_result.txt`/settings files;
opened in `FSOM_OPEN_APPEND` mode, synced and closed immediately per call -- never
`FURI_LOG_*`, since an active CLI session is itself one of the two things this is trying to
isolate).

Line format (space-separated, one line per call):

```
YYYY-MM-DD HH:MM:SS <entry|exit> free=<bytes> max_block=<bytes> dtr=<0|1> fault=0x<hex>
```

- `free`/`max_block`: `memmgr_get_free_heap()`/`memmgr_heap_get_max_free_block()` -- the exact
  two metrics the ELF loader checks at launch. `entry` is measured before this app's own first
  allocation (the message queue, in `Esp32App`'s initializer) but logged once
  `app_data_root_path` resolves a few lines later (values captured early, write deferred only
  because Storage isn't open yet at the true first instant). `exit` is measured as late as
  possible: after every one of this app's own teardown calls (`stop_service()`, timer/mutex
  frees, view port removal) but before `furi_record_close(RECORD_STORAGE)`.
- `dtr`: `furi_hal_cdc_get_ctrl_line_state(0) & CdcCtrlLineDTR` -- 1 if a PC-side tool
  currently has the USB CDC port open, matching this file's own CLI-session finding above.
- `fault`: `furi_hal_rtc_get_fault_data()`, read (not cleared) by this app -- almost always
  `0x00000000` by the time this app's `entry` line runs, per the desktop-fault-screen race
  explained above (the firmware clears it on the user's dismissal before they can relaunch
  this app), but cheap to include and directly relevant if that race ever doesn't resolve in
  time.

Build-verified via `tools/build_flipper.ps1` (`flipper_esp32_over_ble.fap`, 134188 bytes, up
from 133052 before this change). `.bss` measured via `arm-none-eabi-size`: 32068 -> 32388
(+320 bytes, exactly the two new 160-byte `static` line/path scratch buffers this function
uses, both following this file's own "no locals >=100 bytes reachable from a tight thread"
rule even though neither is actually reachable from one -- `h04_heap_diag_log()` is called
only from `flipper_esp32_over_ble_app()` itself, the main thread, both call sites (grepped:
lines near app entry and near the final teardown block), never from `BleEventWorker`, `Bt`, or
the timer-service thread, so no stack-audit concern from this file's own tight-thread rule
applies here). Not hardware-tested -- field data collection is the explicit next step, not
done this session. **Temporary only: strip before this becomes a real feature branch**, per
the naming convention above.

**2026-09-24, first field data from the instrumentation above -- flashed, transferred over an
active `runfap.py`/CLI session, then relaunched several times with plain Back-exit between
launches (no device power-cycle), no crash this run.** Full log
(`/ext/apps_data/flipper_esp32_over_ble/heap_diag_TEMP_H04.log`, read back via
`scripts/storage.py -p COM8 read ...`):

```
entry free=20832 max_block=13288 dtr=0   <- right after runfap.py's transfer+auto-launch
entry free=28352 max_block=26880 dtr=0   <- (no matching exit logged for launch 1 above)
exit  free=23776 max_block=22240 dtr=0
entry free=28184 max_block=26880 dtr=0
exit  free=23600 max_block=22240 dtr=0
entry free=28048 max_block=26880 dtr=0
exit  free=23480 max_block=22240 dtr=0
entry free=27888 max_block=26112 dtr=0
exit  free=23312 max_block=21472 dtr=0
entry free=27736 max_block=26272 dtr=0
exit  free=23184 max_block=21632 dtr=0
```

**Reading this data:**

- **The very first entry (right after the CLI-driven transfer) is the sharpest data point in
  the log: `max_block` is roughly half** (13288) **of every subsequent launch's** (~26000-26880),
  even though `dtr` already reads 0 by the time this line was captured -- consistent with this
  file's own CLI-session finding above (the ~4-5 KB `CliShell` resident allocation plus
  transfer-time buffering hadn't fully unwound yet) and the strongest real evidence so far that
  a launch immediately following an active USB/CLI transfer is meaningfully more constrained
  than a steady-state relaunch, even without a crash resulting this time.
- **No `exit` line was logged for that first launch** -- `runfap.py` always force-launches
  after a transfer and this project's own tooling (`tools/flash_flipper.ps1`) does not wait for
  or drive an exit; the app was still on-screen when the user began their own manual
  relaunch-cycle testing, which produced the second `entry` without an intervening `exit` line.
  Expected, not a bug in the instrumentation.
- **Entry-to-exit within one launch always drops ~4400-4700 bytes free / ~4600-4800 bytes
  max_block, then jumps back up by roughly the same amount between one launch's `exit` and the
  next launch's `entry`.** This is exactly what's expected, not a leak: this app's own runtime
  allocations (mutexes/timers/GUI/storage buffers) are still held at the `exit` measurement
  point (deliberately placed *before* `furi_record_close(RECORD_STORAGE)`, not after full ELF
  teardown), and the loader only reclaims the app's ELF sections (`.text`/`.bss`/etc.) *after*
  this app's own exit code finishes -- which is exactly the gap between one `exit` line and the
  next `entry` line.
- **A real, small, monotonic downward drift across the four complete relaunch cycles**: entry
  `free` 28352 -> 28184 -> 28048 -> 27888 -> 27736 (-616 total, ~150-170/cycle); entry
  `max_block` 26880 -> 26880 -> 26880 -> 26112 -> 26272 (mostly flat, one step down); exit
  `free` and `max_block` show the same shape, offset by the constant in-app-footprint gap
  above. This is a real, reproducible instance of the "fragmentation compounds slightly with
  each relaunch" hypothesis from earlier in this entry -- but at only ~150-200 bytes/cycle
  against a ~26-28 KB starting margin, it would take on the order of 100+ back-to-back relaunches
  in one boot session to close that gap by drift alone. **This run's margin never got
  anywhere close to tight enough to threaten a crash** (consistent with no crash occurring this
  run) -- so slow steady-state drift alone does not explain a 3rd-launch crash; something
  sharper (matching the CLI-transfer-launch's halved `max_block`, or a fresh instance of heavier
  fragmentation than this run happened to hit) is still the more likely trigger for the earlier
  hardware repro.
- **Caveat on what these numbers actually measure:** `entry`/`exit` are read from *inside* the
  already-loaded app, i.e. *after* the loader has already carved out this app's own
  `.text`+`.rodata`+`.data`+`.bss` (~99 KB combined per this file's earlier measurements) from
  the heap. So a `max_block` of ~26-27 KB here characterizes headroom for this app's *own small
  runtime allocations* (the five `furi_mutex_alloc`/`furi_timer_alloc` calls this file's
  "unifying candidate mechanism" section above flags, each only tens to a few hundred bytes) --
  not the pre-load contiguous availability the loader itself needs for the ~99 KB ELF-section
  placement. With 26+ KB of headroom for a handful of sub-1KB allocations, this run's data does
  not support the runtime-`furi_check(...)`-trips-from-fragmentation theory being the active
  mechanism *this time* -- there was ample margin. It remains plausible for a launch that starts
  from a substantially worse pre-existing margin (e.g. the CLI-transfer case above, or whatever
  state preceded the earlier 3rd-launch crash) that a mutex/timer alloc could still fail even
  with generous *nominal* free bytes, if fragmentation is severe enough right at that moment --
  just not demonstrated by this particular clean run.

**Net effect on open questions:** the CLI-transfer-launch data point is genuinely new,
concrete evidence supporting this file's USB/CLI section above (real degradation, not just a
plausible-sounding mechanism). The steady-state relaunch drift is real but too slow by itself
to explain a 3rd-launch crash. **The 3rd-launch `furi_check_failed` repro still has not been
reproduced with this instrumentation active** -- next time it recurs with this build installed,
read back `heap_diag_TEMP_H04.log` immediately (numbers right before the crash are the ones
that matter) and report the exact text of the "Flipper crashed and was rebooted" popup before
dismissing it, per the ask earlier in this entry.

**2026-09-25: user confirmed the repro is specifically tied to an active wardriving session
-- corroborates BL05's original title, which this file's own 2026-09-24 addendum had cast
doubt on. Read-only trace of this app's own exit path plus the pinned firmware's BLE-profile
teardown internals found a concrete, previously-unflagged cross-thread race, and a matching
gap versus the firmware's own reference pattern for the exact same teardown sequence. Fixed
(build-verified, not hardware-tested).**

Re-grepped every `furi_check(...)`/`furi_assert(...)` site in `flipper/`: still exactly the
same 8, all in `flipper_esp32_over_ble.c`, no new ones added since the growth this file
already tracked. None of the wardriving record-handling path (`cbor_wardriving.c`,
`handle_wardriving_status()`, `wardriving_csv_write_record()`) uses `furi_check`/`furi_assert`
at all -- `feb_cbor_decode_wardriving_status_result_payload()` rejects (does not write past)
any `array_count > FEB_WARDRIVING_MAX_RECORDS_PER_BATCH` before the decode loop touches
`payload->records[i]` (`cbor_wardriving.c` ~line 678), so a corrupted/oversized backlog batch
from H03's failure shape cannot itself overrun a fixed array here -- this rules out "a
furi_check/bounds-check trip inside wardriving record decode" as this bug's mechanism, at
least for the record-count dimension.

**Traced this app's own exit path (`stop_service()`, called both on explicit Back-exit-while-
paired and `BtStatusUnavailable`) against the pinned Unleashed checkout's BLE-profile
lifecycle plumbing it calls into:**

- `stop_service()` calls `bt_disconnect(app->bt)` then immediately (no delay)
  `bt_profile_restore_default(app->bt)`. Traced `bt_disconnect()` into
  `applications/services/bt/bt_service/bt_api.c`/`bt.c`: it resolves to `bt_close_connection()`,
  which is only `bt_close_rpc_connection()` + `furi_hal_bt_stop_advertising()` -- **it does not
  itself force-drop an already-established GATT link.** The actual link teardown only happens
  inside `bt_profile_restore_default()`'s call chain
  (`bt_profile_start(bt, ble_profile_serial, NULL)` -> `bt_change_profile()` ->
  `furi_hal_bt_change_app()` -> `furi_hal_bt_reinit()`), and even there, `hci_reset()` (the
  actual radio-level reset that forcibly drops any live connection) runs *after*
  `current_profile->config->stop(current_profile)` -- i.e. after our own `profile_stop()` has
  already unregistered the BLE event handler and `free(profile)`d the `Esp32BleProfile*`
  (`furi_hal_bt.c`'s `furi_hal_bt_reinit()`, ~line 202-213).
- **The BLE event handler list this app registers into has no locking at all**
  (`targets/f7/ble_glue/furi_ble/event_dispatcher.c`): `ble_event_dispatcher_process_event()`
  (called synchronously on `BleEventWorker` for every inbound HCI/GATT event) iterates the
  same plain `m-list` (`handlers`) that `ble_event_dispatcher_unregister_svc_handler()` (called
  from `profile_stop()`, itself invoked from the **Bt service thread** during the reinit above)
  mutates via `GapSvcEventHandlerList_remove()`. Nothing serializes these two call sites
  against each other beyond whatever timing accident happens to keep them apart.
- **Net mechanism:** because the actual link is still fully live right up until `hci_reset()`,
  and our own handler-unregister + `free(profile)` happens *before* that reset, there is a real
  window in which `BleEventWorker` can still be mid-dispatch of an inbound GATT event for this
  app's profile (a notification-sent ack, a write completion -- exactly what a busy wardriving
  backlog drain generates continuously) at the same instant the Bt-service-thread's teardown
  unregisters the handler and frees the struct that dispatch is using. This is a genuine
  cross-thread teardown race, and it is close to unreachable on a quiet/idle connection (no
  event in flight to collide with) but much more likely to be hit while wardriving is actively
  streaming -- directly matching the user's confirmed correlation. A hit manifests as
  undefined behavior in the unsynchronized linked-list (`GapSvcEventHandlerList_next`/`_remove`
  racing each other) and/or a genuine use-after-free read/write through the freed
  `Esp32BleProfile*` -- both are classic silent-heap-corruption shapes, consistent with this
  file's own earlier fragmentation/heap-corruption framing: the corruption need not crash
  *this* launch's exit at all, and can instead surface as a `furi_check(...)` NULL-check trip
  on a *later* launch's early `furi_mutex_alloc`/`furi_timer_alloc`/`malloc(sizeof(Esp32BleProfile))`
  call once the allocator's metadata is disturbed -- exactly the shape of the monotonic
  1st-fast/2nd-slow/3rd-crashes repro already on record.
- **This app's own teardown sequence deviates from the pinned firmware's own reference
  pattern for this exact same `bt_disconnect()` -> `bt_profile_restore_default()` sequence**:
  `applications/system/hid_app/hid.c` (the only other BLE-profile-owning app in the checkout
  that tears down the same way) inserts `furi_delay_ms(200)` between the two calls (its own
  comment: "Wait 2nd core to update nvm storage") at both its own teardown site (~line 352-359)
  and its pairing-removal path (~line 27-32). This app's `stop_service()` had no equivalent
  delay at all.

**Fixed this session:** added the same `furi_delay_ms(200)` in `stop_service()`
(`flipper/flipper_esp32_over_ble.c`), between `bt_disconnect(app->bt)` and the
`bt_profile_restore_default(app->bt)` call, matching `hid_app.c`'s placement. This does not
eliminate the underlying firmware-level race (this app cannot patch
`event_dispatcher.c`'s missing lock), but it gives any BLE event already in flight on
`BleEventWorker` a wide settle window to finish dispatching before this app's own handler is
unregistered and its profile struct freed -- matching the only known-working reference
pattern in this exact firmware for this exact sequence. Build-verified via
`tools/build_flipper.ps1`: `flipper_esp32_over_ble.fap`, 133080 bytes (up from 133052 before
this change, matching a small code addition, no new `.bss`). **Not hardware-tested** -- this
session had no hardware access; confirming this actually prevents the crash (rather than just
narrowing the race window) needs a live repro attempt with wardriving actively running,
ideally several back-to-back relaunch cycles as in the original report.

**Confidence ranking of candidate mechanisms for this bug, given everything traced across
both sessions:**
1. **(Highest, this session)** the cross-thread `BleEventWorker`-vs-Bt-service-thread teardown
   race just described, exacerbated by a busy wardriving connection and this app's missing
   settle delay -- directly explains the wardriving-specific correlation, and is now
   mitigated (not proven eliminated) by the fix above.
2. **(Still open, prior session)** heap fragmentation from a live USB/CLI session's resident
   `CliShell` allocation, or ordinary cross-launch drift, causing one of the 5 alloc-guard
   `furi_check()` sites (reassembly_mutex/wardriving_state_mutex/reassembly_timeout_timer/
   gps_poll_timer/publish_poll_timer) to trip on `NULL`. Independent of mechanism 1 above --
   both can co-occur, and mechanism 1's heap corruption could itself be what tips mechanism 2
   over the edge on a subsequent launch.
3. **(Ruled down, this session)** a wardriving-record decode/bounds issue tripping a
   furi_check or overrunning a fixed array -- no `furi_check`/`furi_assert` exists anywhere in
   the wardriving decode/CSV-write path, and the record-count bound is enforced before any
   array write. Still theoretically possible for some *other* field's bound not audited this
   session, but no longer the leading theory.

**Still needs live hardware to fully close:** confirm the fix above actually prevents the
crash under the user's original repro shape (wardriving running, several relaunches, same
boot session) -- this session's build-only verification cannot distinguish "race window
narrowed enough in practice" from "race window merely made statistically rarer."

**2026-09-26: new field report -- publishing crashed the Flipper (OOM-shaped) while
`scripts/publish_wardriving.ps1` was pulling a large CSV over `storage read_chunks`. Confirmed
via `storage.py list` that `wardriving/wardriving_current.csv` is 3,768,352 bytes (~3.7 MB) and
still present, unrenamed** -- matches this app's own "only rename the CSV on a confirmed
successful upload" design (`docs/WARDRIVING_PUBLISH.md`), so no data was lost; the crash
happened during the read, before any archive/rename step.

This is the same CLI-session-narrows-heap-margin mechanism this entry already documents
(the DTR-triggered, ~4-5 KB resident `CliShell` allocation for as long as a PC tool holds the
port open), but at a scale not previously exercised: a ~3.7 MB transfer at `read_chunks`' 8192-byte
chunk size is ~460 chunk round-trips, each gated on the `Ready?`/single-byte-`y` handshake --
plausibly minutes, not the seconds-scale CLI sessions (`storage.py list`/`stat`, a `runfap.py`
transfer) this entry's existing field data was gathered from. That's a qualitatively longer
window for this app's own concurrent runtime activity (idle-timeout BLE reconnect/advertise
cycling with no ESP32 present, since publish is designed to run without one) to compete for
heap against the same narrowed margin, not just a bigger one-time bite.

**No heap-margin data was captured for this occurrence** -- `h04_heap_diag_log()` (confirmed via
`storage.py list`: no `heap_diag_TEMP_H04.log` on this SD card) was stripped from
`flipper/flipper_esp32_over_ble.c` after the 2026-09-25 wardriving-teardown-race fix landed, per
this entry's own "temporary only, strip once field data is in" note -- so this report has a
confirmed trigger condition (large CLI pull) and a confirmed safe outcome (no data loss) but no
new quantitative evidence beyond that.

**Crash-popup text captured this time (per this entry's own standing "practical ask"): "Flipper
crashed and was rebooted -- out of memory."** This resolves which failure class this occurrence
was, and it's a different one from the `furi_check_failed` shape the 2026-09-24/25 addenda above
focused on: grepped `furi/core/memmgr_heap.c` in the pinned Unleashed checkout --
`furi_check(pvReturn, xWantedSize ? "out of memory" : "malloc(0)")` (~line 466) is the *only*
`furi_check()` call site anywhere in the firmware or this app that carries the literal message
`"out of memory"`, and it fires inside the heap allocator itself, on **any** `malloc()`/
`pvPortMalloc()` in the whole system returning NULL -- not one of this app's own 8 argument-less
`furi_check(...)` sites (which, per `check.c`'s own logic already traced above, would show the
generic `"furi_check failed"`, not this message). Also distinct from the ELF loader's own launch-
time rejection path (`LoaderStatusErrorOutOfMemory`, `applications/services/loader/loader.c`),
which doesn't route through this same `furi_check()` call at all and isn't in play here anyway --
this app was already running, not launching, when the crash hit.

**Net effect: this is confirmed genuine, generic heap exhaustion -- some `malloc()` call,
anywhere in the running firmware, returned NULL at the moment of the crash** -- not a localized
bug in one of this app's own five alloc-guard sites (H04's earlier "unifying candidate
mechanism" section). This is squarely consistent with, and now the strongest evidence yet for,
this entry's CLI-session-narrows-heap-margin mechanism: a multi-minute `read_chunks` transfer
holding the ~4-5 KB `CliShell` allocation resident is exactly the kind of sustained margin
reduction that would make some *other*, otherwise-routine allocation (this app's own BLE
reconnect/advertise cycling while idle-waiting with no ESP32 present, a GATT-stack realloc, or
something else in the firmware entirely) fail outright rather than merely trip one of this app's
own named guards. **Which specific allocation failed is still unconfirmed** -- the popup message
identifies the failure *class*, not the call site; only a live serial capture spanning the crash
(`__furi_crash_implementation()` logs the failing allocation's context over serial, per this
entry's earlier tracing) would localize it further.

**Practical near-term implication for `docs/WARDRIVING_PUBLISH.md`:** that design's accepted
tradeoff ("no size cap... if the current file grows large enough that wdgwars rejects or chokes
on it, publishing just fails with a clear on-screen error... easy to avoid by publishing
regularly") assumed the downside of letting the CSV grow large was a slow or server-rejected
upload -- not a Flipper crash during the *pull* step, before the file ever reaches wdgwars. This
field report shows the crash risk scales with how long it's been since the last successful
publish, which is a sharper practical argument for publishing often than that doc currently
states. Not re-scoped or fixed here -- flagging the correction, not deciding the fix (re-adding
the diagnostic instrumentation for a targeted repro, or a chunk-size/timeout retune on the CLI
pull side, are both still open options, neither attempted this session).

**Mitigation implemented 2026-09-26, build-verified only, not yet hardware-verified:**
`publish_start()` (`flipper/flipper_esp32_over_ble.c`) now tears down this app's own BLE
profile (`stop_ble_profile()`, a new helper factored out of `stop_service()`) for the
duration of the publish transfer, freeing the GATT stack's heap, and restarts it
(`publish_finish_waiting()`) once the transfer concludes, is cancelled, or times out. This
reduces heap pressure during publish regardless of which exact allocation was losing the
race above -- it is a mitigation, not a confirmed root-cause fix; "which allocation failed"
remains open.

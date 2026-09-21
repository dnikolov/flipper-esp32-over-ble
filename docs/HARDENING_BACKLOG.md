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

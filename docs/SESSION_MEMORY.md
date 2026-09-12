# Session Memory

## Project and scope

Flipper Zero <-> ESP32-C6 over BLE. See [CLAUDE.md](../CLAUDE.md) for the project summary and
[docs/BASELINES.md](BASELINES.md) for pinned board/firmware/toolchain versions — not repeated
here.

## Current state (as of 2026-09-11, commit TBD)

**Phase 2 (core BLE transport through authenticated runtime sessions) is complete.** Steps 1-7 —
build baselines, BLE transport, record framing, radio-coexistence validation, trusted-environment
X25519 pairing, authenticated AES-256-GCM runtime sessions, and the board-identity/capability
registry — are implemented **and hardware-verified** on real devices (ESP32-C6-DevKitC-1-N4 +
Flipper Zero).

**Phase 3 (production-ready wardriving) is underway.** `wifi_scan` and `ble_scan` are implemented
and hardware-verified: both manual on-device scan triggers with results rendered in scrollable
views, each capped at the 32 strongest results by RSSI. **Reordered 2026-09-07**:
`ble_scan`/`wardriving` no longer wait on GPS hardware being wired up — they're implemented using
a fixed-coordinate GPS stub behind a swappable location-source interface, with the wardriving-log
half of step 8's hardened persistence (a checksummed circular log on raw flash) pulled forward and
built for real as part of this same work. See [docs/PLAN.md](PLAN.md)'s "`ble_scan`, `wardriving`,
and the GPS-stub reorder" section for the full design and decisions (wire protocol frozen in
[docs/PROTOCOL.md](PROTOCOL.md), capability behavior in [docs/CAPABILITIES.md](CAPABILITIES.md)).

**`wardriving` is now implemented on both firmwares as of 2026-09-09** (build- and host-test-
verified on each side; hardware verification still pending — see "Known open items" below).
ESP32 side: `handle_wardriving_command()` start/stop with full field-presence/bounds
validation, an autonomous Wi-Fi/BLE capture engine that shares its busy-guard flags with manual
`wifi_scan`/`ble_scan` (so the busy rule is bidirectional by construction rather than a separate
cross-check), a checksummed append-only circular log on a new dedicated `wardrive` raw-flash
partition (`esp32/main/wardriving_log.c`/`wardriving_record_format.c`), and unsolicited
backlog-drain-on-session-establish. Flipper side: a one-tap start/stop control/status screen
(`flipper/flipper_esp32_over_ble.c`, reachable via Up from the main screen), `status` dispatch
routing by decoded `state` text (so wardriving's `started`/`data`/`stopped` states and the
`request_id = 0` unsolicited-backlog-drain case are handled distinctly from wifi_scan/ble_scan's
`partial`/`complete`), and incremental WiGLE CSV export to SD card via a new pure/host-testable
module (`flipper/wardriving_csv.c`/`.h`). After both sides are hardware-verified: the
pairing-record/capability-file half of step 8 (still deferred), then step 9 (full-system
validation).

For the full roadmap, phase boundaries, and each step's "done when" criteria, see
[docs/PLAN.md](PLAN.md). For the complete dated history of how each step was designed,
implemented, and debugged — including every bug's root cause — see
[docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md).

## Known open items (check before starting related work)

**Active investigation — blocks wardriving's step-9 "done when" bar:**

- **Wardriving BLE reconnect stall is still open.** A live forced disconnect during wardriving's
  BLE capture never reconnects. The first candidate fix (switching wardriving's BLE re-arm from
  passive to active scanning) was flashed and retested live and **did not resolve it** — zero
  reconnects over 130+ discovery restarts across 70+ seconds. Several hypotheses were ruled out
  by reading source directly this session (stale `connection_handle`, a `scan_record_matches()`
  logic bug, a scan that never truly re-arms). Leading unconfirmed suspect: Wi-Fi/BLE radio
  coexistence starvation from wardriving's concurrent, gapless Wi-Fi source
  (`wifi_interval_ms` defaults to 0/continuous). **Next step**: reproduce with wardriving's
  BLE source only (no Wi-Fi) to isolate. A third capture (`esp32_monitor3.log`) was initially
  reported as a wardriving-free reproduction of the same stall on the plain reconnect path;
  the log itself refutes that — `wardriving started (... wifi=1 ble=1)` precedes the stalling
  disconnect by 66s and is never stopped, so `start_scan()`'s dedicated reconnect scan was
  never in play (it no-ops while `wardriving_ble_active`). Third data point consistent with,
  not against, the coexistence suspect; the plain `start_scan()` path remains unimplicated.
  Full investigation: `docs/LESSONS.md`'s "wardriving-passive-scan-reconnect-stall" entry and
  `docs/PROJECT_HISTORY.md`'s matching dated entries.

**Immediately next once the above is resolved:**

- **Live multi-minute wardriving run at the current BLE duty cycle** (`ble_window_ms=100`,
  `ble_interval_ms=500`, ~20% duty, raised 2026-09-10) to confirm no idle-timeout or
  duty-starvation-style disconnects, and that BLE records show up reliably. Flashed but not yet
  run. See `docs/PROJECT_HISTORY.md`'s "BLE duty-cycle fix left wardriving nearly blind" entry.
- **Remaining two of three wardriving hardware-acceptance items**: an extended unattended run
  validating the flash log's wraparound/power-loss behavior, and confirming the Flipper's WiGLE
  CSV export actually lands correctly on the SD card.
- **ESP32 wardriving dedup** (128-slot address hash table) and its 2026-09-11
  distance-threshold fix (`3111fa2`, see `docs/PROJECT_HISTORY.md`): build-verified, **not yet
  hardware-tested**.
- **CSV export dedup** (`feb_wardriving_dedup_should_write()`): build- and host-test-verified,
  flashed 2026-09-10, **pending the user's own manual SD-card check**.
- **BLE active scanning** in `ble_scan` (2026-09-11, `e92aad9`): build-verified, not yet
  hardware-tested.
- **Connection/flush LED indicators (both firmwares)**: ✅ done and hardware-confirmed
  2026-09-12, including two real regressions found+fixed along the way (a BLE-host-queue-
  blocking LED tick, and an unrelated G20 "fix" that had broken every outbound notify). See
  PROJECT_HISTORY.md's two matching 2026-09-12 entries.

For everything else — deferred fixes, known bugs not yet scheduled, disputed-severity items, and
cost/efficiency work — see the single consolidated list in [BACKLOG.md](BACKLOG.md). Add
genuinely new current-state facts here as they happen; file everything else there instead of
letting this section re-accumulate narrative (this section drifted into exactly that twice
before — see `CLAUDE.md`'s conventions).
- **Build-time stack-budget checking** (`-fstack-usage`/`-Wstack-usage=N` wired into the FAP build)
  is still just a proposal, not an actual standing check — every stack-overflow bug so far (four of
  them across steps 3, 5, 7, and wifi_scan) was found by crashing real hardware first. See
  `docs/PLAN.md`'s Backlog "Remediations proposed after the 2026-09-05 hardware pairing test."

## Working conventions worth remembering every session

- Reconfirm serial ports before any hardware work — `COM9` (ESP32) / `COM8` (Flipper) in recent
  sessions, not guaranteed stable across reboots.
- Check for concurrent peer Claude Code sessions on this repo before touching hardware (this
  project frequently has several running at once).
- Use the canonical scripts for build/flash instead of re-deriving environment setup:
  `tools/build_esp32.ps1` (build, optional `-Port`/`-SkipBuild`/`-CaptureBootLog`),
  `tools/build_flipper.ps1` (build, optional `-Port` to also transfer), `tools/flash_flipper.ps1`
  (transfer a built FAP to the Flipper's SD card via `runfap.py`). They already handle the
  Git-Bash/MSYS `export.ps1` pitfall and the Flipper's real (non-mass-storage) transfer method.
- Delegate mechanical doc sync (e.g. `docs/USER_GUIDE.md` updates) and known-procedure hardware
  flash/verify passes to the cheapest capable model (Haiku), per this project's own convention.
- This project has hit the same `BleEventWorker`/task-stack-overflow bug class repeatedly (steps
  3, 5, 7, and wifi_scan). Any new BLE-callback-path code on either firmware should default to
  file-scope `static` storage for non-trivial buffers, and get a real `-fstack-usage` check before
  being trusted at a tight budget.

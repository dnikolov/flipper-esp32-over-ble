# Session Memory

## Project and scope

Flipper Zero <-> ESP32-C6 over BLE. See [CLAUDE.md](../CLAUDE.md) for the project summary and
[docs/BASELINES.md](BASELINES.md) for pinned board/firmware/toolchain versions — not repeated
here.

## Current state (as of 2026-09-09, commit TBD)

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

- **`ble_scan` capability**: implemented and hardware-verified 2026-09-08 (manual on-device scan
  trigger via Right button on the main screen, results in a scrollable view, capped at 32 devices
  by RSSI). See [docs/USER_GUIDE.md](USER_GUIDE.md) "Scanning for BLE devices" section.
- **`wardriving` capability**: both firmwares implemented 2026-09-09, build- and host-test-verified.
  **Hardware-verified 2026-09-10** with two real bugs found and fixed during the first real-device
  test — see `docs/PROJECT_HISTORY.md`'s "wardriving hardware-verified" entry for the full
  narrative (a `nimble_host` stack overflow in `wardriving_send_next_batch()`, and a
  GATT-write-flood + reconnect-scan-restart collision). Both fixes build- and host-test-verified
  and hardware re-verified on the physical ESP32-C6 + Flipper. **Three items still open** before
  final acceptance: (1) a forced-disconnect test specifically exercising the merged-reconnect-scan
  mechanism under live wardriving BLE capture, (2) an extended unattended run validating the
  flash log's wraparound and power-loss behavior on real hardware, (3) confirming the Flipper's
  WiGLE CSV export actually lands correctly on the SD card (the on-device control/status screen
  and start/stop dispatch were confirmed working). **2026-09-10: a fourth real bug found** —
  wardriving's default 100% BLE observer duty starves the active connection under real traffic
  (a `stop` command couldn't land, connection dropped every ~30-40s); fixed by raising
  `ble_interval_ms`'s default to 500ms (~6% duty) — see `docs/PROJECT_HISTORY.md`'s "wardriving
  BLE duty-cycle starvation" entry. **Reflashed and hardware-re-verified same session** — storm
  gone, clean reconnects. WiFi-source duty cycle (`wifi_interval_ms` default still 0/continuous)
  remains unvalidated with an active connection — see `docs/PLAN.md`'s Backlog. **Also
  2026-09-10: CSV export now deduplicates repeated observations of the same address**
  (`flipper/wardriving_csv.c`'s `feb_wardriving_dedup_should_write()` — new-address/RSSI-
  improved-6dB/moved-30m OR-gate, no time-based trigger; see `docs/PROJECT_HISTORY.md`'s
  "wardriving CSV export writes a row per observation" entry and `docs/CAPABILITIES.md`'s
  wardriving bullet). Build- and host-test-verified (479/479 checks, plus a clean real-FBT
  build); **not yet hardware-verified.**
- **Idle-connection heartbeat/keep-alive redesign**: backlogged by explicit user choice. The
  current 30-second idle-timeout disconnect-and-reconnect cycle works correctly but causes a
  cosmetic LED/screen flicker roughly every 30 seconds during an otherwise-healthy idle session.
  Needs its own design session (it's a wire-protocol change, both firmwares) before
  implementation — see `docs/PLAN.md`'s Backlog.
- **`unsupported_version` handling is missing** on both the pairing-envelope and session-envelope
  paths — a bad `version` field is not currently rejected with the spec-mandated error + disconnect
  on either firmware.
- **`pairing_crypto.c`'s X25519 ladder is not constant-time** (`mbedtls_mpi_mod_mpi()`'s reduction
  loop is data-dependent). Accepted for the current threat model (one-shot pairing operation,
  physical possession of either device already accepted as fully compromising) — not fixed.
- **Multi-board UX gaps**: no manual "disconnect current board" action to free the BLE connection
  slot without powering one off, and no automatic arbitration between multiple paired boards
  (gated on an unresolved question: can the Flipper's peripheral role advertise while already
  connected?).
- **No host-test coverage** for `capability_query`/`capability_response` on the Flipper side.
- **Unsynchronized cross-thread access** to the Flipper's `session_key`/`session_seq_out`/
  `outgoing_message_id` state (the wifi_scan command send path, on the app's main thread, vs.
  BLE-thread senders) — currently safe only by a UI-gating invariant, not a lock.
- **No scrollable capability-list screen yet** on the Flipper — the single status screen just
  grows a line per capability; fine while `features` is short, will need a real list view once it
  grows.
- **Flipper FAP still uses a single-`ViewPort`/`AppEvent`-queue architecture**, not a real
  `ViewDispatcher`/scene manager — every screen so far (including wifi_scan's results view) has
  been bolted onto this; flagged as increasingly strained, not yet worth the rework.
- **GPS backfill-to-first-fix** is not implemented — once wardriving exists, results captured
  before GPS achieves a fix will be discarded outright, with no Flipper-settable override yet.
- **Non-ASCII SSID rendering** is untested on real hardware (host-native codec tests cover the
  encoding; no such network was available nearby during wifi_scan's hardware verification). Not a
  blocker.
- **Step 4's merged-reconnect-scan mechanism** (recovering a disconnect via the same BLE-observer
  scan pass rather than a dedicated reconnect scan) was implemented and reviewed but never actually
  exercised under test, since the step 4 sweep had zero disconnects at any duty cycle. Flagged for
  step 9's full-system validation, which already plans reconnect/replay testing.
- **Build-time stack-budget checking** (`-fstack-usage`/`-Wstack-usage=N` wired into the FAP build)
  is still just a proposal, not an actual standing check — every stack-overflow bug so far (four of
  them across steps 3, 5, 7, and wifi_scan) was found by crashing real hardware first. See
  `docs/PLAN.md`'s Backlog "Remediations proposed after the 2026-09-05 hardware pairing test."

## Working conventions worth remembering every session

- Reconfirm serial ports before any hardware work — `COM9` (ESP32) / `COM8` (Flipper) in recent
  sessions, not guaranteed stable across reboots.
- Check for concurrent peer Claude Code sessions on this repo before touching hardware (this
  project frequently has several running at once).
- Any `scripts/*.py`/`esptool` call with a Flipper path argument starting with `/` must run from
  PowerShell, not Git-Bash (MSYS path translation mangles the leading slash).
- `idf.py`/ESP-IDF's `export.ps1` refuses to run if `MSYSTEM` is set in the environment (Git-Bash
  sets it; clear it first if shelling out from a POSIX context).
- Delegate mechanical doc sync (e.g. `docs/USER_GUIDE.md` updates) and known-procedure hardware
  flash/verify passes to the cheapest capable model (Haiku), per this project's own convention.
- This project has hit the same `BleEventWorker`/task-stack-overflow bug class repeatedly (steps
  3, 5, 7, and wifi_scan). Any new BLE-callback-path code on either firmware should default to
  file-scope `static` storage for non-trivial buffers, and get a real `-fstack-usage` check before
  being trusted at a tight budget.

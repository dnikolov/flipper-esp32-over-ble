# Session Memory

## Project and scope

Flipper Zero <-> ESP32-C6 over BLE. See [CLAUDE.md](../CLAUDE.md) for the project summary and
[docs/BASELINES.md](BASELINES.md) for pinned board/firmware/toolchain versions — not repeated
here.

## Current state (as of 2026-09-07, commit `63ec936`)

**Phase 2 (core BLE transport through authenticated runtime sessions) is complete.** Steps 1-7 —
build baselines, BLE transport, record framing, radio-coexistence validation, trusted-environment
X25519 pairing, authenticated AES-256-GCM runtime sessions, and the board-identity/capability
registry — are implemented **and hardware-verified** on real devices (ESP32-C6-DevKitC-1-N4 +
Flipper Zero).

**Phase 3 (production-ready wardriving) is underway.** The first real capability, `wifi_scan`, is
implemented and hardware-verified: manual on-device scan trigger, results rendered in a scrollable
view, capped at the 32 strongest APs by RSSI. Still to come in Phase 3: a GPS module needs to be
physically wired to the board before `ble_scan`/`wardriving` can start (not yet begun), then step 8
(hardened persistence for the pairing record and the wardriving log), then step 9 (full-system
validation).

For the full roadmap, phase boundaries, and each step's "done when" criteria, see
[docs/PLAN.md](PLAN.md). For the complete dated history of how each step was designed,
implemented, and debugged — including every bug's root cause — see
[docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md).

## Known open items (check before starting related work)

- **GPS / `ble_scan` / `wardriving` capability**: not started. Needs a GY-NEO6MV2/NEO-6M GPS
  module physically wired to the C6 first.
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

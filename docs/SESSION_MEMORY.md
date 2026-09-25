# Session Memory

## Project and scope

Flipper Zero <-> ESP32-C6 over BLE. See [CLAUDE.md](../CLAUDE.md) for the project summary and
[docs/BASELINES.md](BASELINES.md) for pinned board/firmware/toolchain versions — not repeated
here.

## Current state (as of 2026-09-21)

**Phase 4 (Heltec WiFi LoRa 32 V2 board support) started 2026-09-16**, via an explicit
user decision to override `docs/PLAN.md`'s "does not start until Phase 3 backlog is cleared"
gate (Phase 3's backlog is not cleared — see [BACKLOG.md](BACKLOG.md); it stays fully deferred,
not interleaved with Phase 4). The shared-component architecture ((a) in `docs/PLAN.md`'s Phase
4 section) was confirmed over duplicating the protocol into a third tree. Step 1 (board
acquisition + baseline bring-up) is done: board confirmed as "WiFi LoRa 32 V2" silkscreen,
ESP32-D0WDQ6 rev v1.0, 8MB flash, MAC `a4:cf:12:03:ba:58`, on COM10 (Silicon Labs CP210x); the
missing classic-`esp32` Xtensa toolchain was installed and an unmodified `hello_world` baseline
built/flashed/booted cleanly. **Step 2 (project scaffolding) is also done** (2026-09-16): the
protocol/crypto files (`framing`, `pairing`/`pairing_crypto`, `session`/`session_crypto`, all
`cbor_*` codec files) moved into a new shared component (`components/feb_protocol/`), wired via
`EXTRA_COMPONENT_DIRS` into both `esp32/` and a new `heltec/` skeleton project (target `esp32`,
classic Xtensa); `esp32/`'s build and all host-native tests pass unchanged, and `heltec/`'s
`idf.py build` passes against a trivial proof-of-link `main.c`. See `docs/PLAN.md`'s "Phase 4:
Heltec WiFi LoRa 32 V2 board support" section for full detail and `docs/BASELINES.md`'s Heltec
entry for the pinned facts. **Step 3 (port BLE transport + pairing + session crypto onto classic
ESP32) is build-verified 2026-09-16, hardware-verification still pending.** `heltec/main/main.c`
now has the full NimBLE-central transport, pairing ceremony, and runtime session-auth state
machine (base protocol only — `handle_capability_query()` reports zero features,
`handle_command()` always answers `unsupported_capability`; no wifi_scan/ble_scan/wardriving/gps
ported). NimBLE central mode and mbedTLS X25519/HKDF/GCM all confirmed buildable against the
classic-`esp32` target (not assumed from the C6). New board-specific `status_led.c` (plain
GPIO25, blink-cadence-encoded state) and `factory_reset.c` (GPIO0 BOOT/PRG button, no
wardriving-toggle) modules. `heltec`'s `idf.py build` passes; `esp32`'s build and all five
host-native test suites pass unchanged. **Step 3 is now fully done, hardware-verified
2026-09-16:** flashed to the physical Heltec board, pairing ceremony and the runtime
`hello`/`hello_ack`/`client_auth` round-trip both completed successfully against the Flipper.
**Step 4 (`board_id`/multi-board-pairing implications) is also done, verified 2026-09-16:**
no code change was needed (the Heltec's `heltec-` `board_id` prefix and the Flipper's storage
code were already collision-safe); confirmed by inspecting the Flipper's SD card, which holds
distinct `esp32c6-*`/`heltec-*` pairing and capability files side by side. **Step 5 (radio/
coexistence sweep) was explicitly skipped by user decision 2026-09-16** — not attempted; see
`docs/PLAN.md`'s step 5 for the consequence (no validated coexistence bounds exist for this
board). **Step 7 (`wifi_scan`/`ble_scan` capability porting, added 2026-09-17 by explicit user
request) is build-verified, hardware-verification still pending:** both capabilities ported
from the C6's reference implementation into `heltec/main/main.c`; `heltec`'s `idf.py build` and
`esp32`'s own build both independently reconfirmed clean. A new `heltec-developer` subagent was
added (`.claude/agents/heltec-developer.md`) since this was the first substantial Heltec-only
firmware task. **Flashed to the physical board 2026-09-17** (COM10); boot log confirmed healthy.
The Flipper's stale cached capability record (`capabilities/heltec-a4cf1203ba58.dat`, zero
features from the step-3 test) has been **deleted**, so the next `capability_query` will reach
this firmware's real feature list. **Still untested: an actual paired `wifi_scan`/`ble_scan`
round-trip against the Flipper**, and this board's Wi-Fi+BLE radio coexistence generally (step 5
was skipped). Full detail: `docs/PROJECT_HISTORY.md`'s 2026-09-16/2026-09-17 entries.

**Step 8 (`gps` capability porting) is build-verified and flashed 2026-09-23, hardware
read-path verification still pending:** a GPS module was rewired from the earlier GPIO36
bench-test spot to GPIO17, and `gps` was ported onto `heltec/main/main.c` (reusing the C6's
frozen `location.c`/`nmea_parser.c`/`handle_gps_command()` unchanged, pins retargeted).
`feb_features[]` is now `{"wifi_scan", "ble_scan", "gps"}`. Flashed to the physical board
(COM10); boot log is clean with no UART-init errors, but no Flipper was paired during that
capture, so a real `gps` query round-trip is still unconfirmed. The Flipper's cached capability
record for this board again needs deleting before its next `capability_query` will see `gps`
(same gotcha as step 7, not yet done as of this writing). Heltec's flash partition is now 96%
full — see `docs/BACKLOG.md` BL13. Full narrative: `docs/PROJECT_HISTORY.md`'s 2026-09-23
entry. **Same day, confirmed by the user:** the stale capability cache was deleted and
`wifi_scan`/`ble_scan` work end-to-end on real Heltec hardware — the first real test of step
7's port. The `gps` NMEA read path itself is still unconfirmed against a live fix.

**Step 9 (`wardriving` capability porting) is build-verified and flashed 2026-09-23, hardware
capture/coexistence verification still pending:** ported wholesale from the C6 (structurally
diffed, zero logic deviations). `feb_features[]` is now
`{"wifi_scan", "ble_scan", "gps", "wardriving"}`. This step deliberately proceeds without the
radio-coexistence validation step 5 skipped — an explicit, informed user decision, not an
oversight. A new custom `heltec/partitions.csv` replaces the previously-stock, 96%-full
partition table (BL13, now resolved), sized for this board's confirmed 8 MB flash: 2 MB factory
app (51% free) + a 2800 KB `wardrive` data partition + ~3.2 MB unallocated headroom. Flashed to
the physical board (COM10); boot log confirmed healthy, new partition table and wardriving log
initialized cleanly against real flash. **Not yet done:** deleting the Flipper's cached
capability record (same gotcha as steps 7/8 — an automated agent can't do this, it needs the
Flipper physically connected via USB with its CLI serial port enumerated, which wasn't available
this session), a live multi-minute wardriving capture run against the paired Flipper, and any
real signal on whether this board's Wi-Fi+BLE combo radio holds up under wardriving's concurrent
load. A cosmetic judgment call — this board's plain on/off LED double-blinks during wardriving
instead of the C6's color swap — is tracked as `docs/BACKLOG.md` BL14 for the user to confirm or
override. Full narrative: `docs/PROJECT_HISTORY.md`'s 2026-09-23 entry.

**Phase 7 (Wardriving screen redesign) implemented and build-verified 2026-09-21, hardware-verification pending.** Design: [docs/WARDRIVING_REDESIGN.md](WARDRIVING_REDESIGN.md). The
Wardriving screen is now split into Stopped/Running (`AppScreenWardrivingStopped`/
`AppScreenWardrivingRunning`), with a new persisted (`wardriving_settings.txt`) settings list on
the Stopped screen: radio mode, WiFi scan-dwell "swelling" (Normal/Aggressive-85ms/Speed-based,
the last self-switching on the ESP32 from its own parsed GPS speed), WiFi cooldown, BLE
active/passive, and a new WiFi regulatory country-code toggle (`BG`/`RoW`). `HomeMenuPublish`
now sits right after `HomeMenuWardriving`, and the Home cursor force-jumps to Wardriving the
moment a wardriving-capable session goes active. The GPS screen now shows real speed (from a
newly-parsed `RMC` speed-over-ground field). `esp32/`, `heltec/`, and the Flipper FAP all build
clean; both host-native codec suites pass in full; `tools/check_shared_headers.py` is clean. Two
open gaps, both recorded in [BACKLOG.md](BACKLOG.md)/that design doc: ESP32-side
`wifi_swelling`/`country` aren't persisted across the button-toggle/boot-autostart wardriving
paths (only a Flipper `start` command carries them), and the GPS screen's speed landed in the
existing Alt row's placeholder rather than a dedicated row (no screen space left). No hardware
testing done yet.

**Phase 6 (wardriving-publish) is complete and hardware-verified end-to-end, 2026-09-18.** The
Flipper publishes its wardriving CSV to wdgwars.pl via a BadUSB-triggered host PowerShell script
(`scripts/publish_wardriving.ps1`), with no ESP32 needed at publish time. A real publish against
the live wdgwars.pl API succeeded, after five hardware bugs found and fixed during testing (a
stack-size MPU fault on the Wardriving screen, a CSV-path regression, DTR/port-discovery timing on
the host script's serial reconnect, and CLI response-echo handling). Design, decisions, and
implementation-status notes: [docs/WARDRIVING_PUBLISH.md](WARDRIVING_PUBLISH.md). Full narrative:
`docs/PROJECT_HISTORY.md`'s 2026-09-17/2026-09-18 Phase 6 entry.

**Phase 2 (core BLE transport through authenticated runtime sessions) is complete and hardware-verified.** Steps 1-7 are implemented and fully verified on real devices (ESP32-C6-DevKitC-1-N4 + Flipper Zero).

**Phase 3a (Flipper UI menu redesign) is complete and hardware-verified.** The Home screen is menu-driven (`HomeMenuItem`: Wardriving/Scan/GPS/Settings/About/Legacy; Up/Down move, OK selects), with Wardriving/Scan/GPS hidden unless a session is active and the board's capability registry supports them, and Settings/About/Legacy always visible. A `connection_lost` flag keeps the active screen in place on disconnect/session-fatal and shows a banner instead of snapping back to Home. All screens (Home, Scan, GPS, Wardriving, Settings, About) have been hardware-tested and work as designed.

**Design-vs-implementation notes:** The ViewDispatcher/scene-manager rewrite listed as a prerequisite in [docs/UI_REDESIGN.md](UI_REDESIGN.md) was deliberately skipped; the Home menu shell was built directly on the existing single `ViewPort`/`AppEvent`-queue pattern instead, and works reliably. The "Scan" menu item remains a Wi-Fi-scan/BLE-scan picker over the existing one-shot capabilities (not the five-mode BLE-active/passive live-view design), as this depends on a runtime BLE active/passive toggle still backlogged. Both limitations are tracked items, not regressions.

**Phase 3 (production-ready wardriving) is complete and hardware-verified.** `wifi_scan`, `ble_scan`, and `wardriving` are all implemented on both sides and hardware-verified:
- `wifi_scan` and `ble_scan`: manual on-device scan triggers with scrollable results views, each capped at 32 strongest results by RSSI.
- `wardriving`: autonomous Wi-Fi/BLE capture engine with checksummed circular log on raw flash, per-record GPS fix-dependency, incremental WiGLE CSV export to SD card.
- **Real GPS driver** (2026-09-12, hardware-verified 2026-09-13): UART1/NMEA GGA+RMC parser, three-state fix tracking (no_signal/acquiring/fix), per-record timestamps, live status polling for display.
- **LED indicators** (both firmwares): connection/session/flush-state visual feedback, hardware-confirmed working.
- **BLE active scanning** in `ble_scan` and within wardriving's capture engine.
- **Wardriving dedup** (separate Wi-Fi/BLE address tables, 48/96 slots, RSSI-improve gate and distance threshold).
- **CSV export dedup**: per-calendar-day files, no duplicate rows for the same address on the same day.

**All Phase 3 hardware-acceptance items complete:**
- ✅ Live multi-minute wardriving run at balanced duty cycle (`wifi_interval_ms=5000`, `ble_window_ms=100`, `ble_interval_ms=500`, ~12 WiFi scans/min + 20% BLE duty) — stable reconnects, reliable backlog drain, 6x denser WiFi coverage than prior 30s interval.
- ✅ Extended unattended flash-log wraparound/power-loss run — circular log correctly evicts by sector and survives interruptions.
- ✅ Flipper's WiGLE CSV export lands correctly on SD card with real timestamps and proper dedup.
- ✅ BLE active scanning effective for BLE-only wardriving isolation test (7/7 reconnects successful).
- ✅ Real GPS module cold-start-to-fix cycle, fix-dependent record discard/resume, real wardriving record timestamps.
- ✅ Stale wardriving log replay fixed (2026-09-13, commit b23aec0): old format records are now properly detected/cleared on boot instead of appearing as stuck backlog.

For the full roadmap, phase boundaries, and each step's "done when" criteria, see [docs/PLAN.md](PLAN.md). For the complete dated history of how each step was designed, implemented, and debugged — including every bug's root cause — see [docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md).

## 2026-09-13 fix batch: G30, G12, G19, G21, BL07

Five items fixed and code-reviewed this session (`102a50f`, `6481400`), on top of the earlier
wardriving WiFi-duty-cycle tuning (5s default) and BL07's throttle-removal fix from the same day:

- **G12** (Flipper never used the negotiated ATT MTU) and **BL07** (ESP32 wouldn't reconnect
  without a FAP restart) — both **hardware-confirmed via live serial log**: MTU negotiates to
  256, `hello_ack` arrives in 2 fragments instead of 6+, plain disconnect/reconnect completes
  cleanly with session re-auth.
- **G30** (wardriving log cross-thread race) — build-verified, all host tests pass; live
  concurrent-load test still needed, tracked as [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H02.
- **G19** (reconnect task churn) and **G21** (path buffer sizing) — build-verified, no
  user-visible behavior change expected; nothing further to test.

**Found during live testing, not caused by today's fixes:** a forced disconnect while wardriving
was actively running (Wi-Fi+BLE sources both on) hit a *separate* reconnect stall —
`wardriving_ble_interval_cb()`'s periodic BLE re-arm colliding with its own in-flight connect
attempt. Tracked as [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H01. **This means the wardriving
BLE reconnect stall (G36) is not fully resolved** — the earlier "RESOLVED" note based on the
BLE-only isolation test's 7/7 result explained *a* cause, not the only one. Treat G36 as open.

**2026-09-13: Flipper app OOM-on-launch root-caused, `.bss` reduced ~34%** (44812 → 29400 bytes) by
consolidating duplicate static scratch (`AppEvent` locals, per-capability command buffers,
per-capability decode-scratch structs) and splitting/shrinking the wardriving dedup table into
separate Wi-Fi/BLE sub-tables — ✅ done, see `docs/PROJECT_HISTORY.md`. Remaining `.bss`-reduction
item (`wifi_scan_aps`/`ble_scan_devices`) tracked in [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H04.

## Known backlog (other open items)

Step 8 (hardened persistent state, pairing-record/capability-file atomicity) and Step 9 (full
negative-security-test suite) remain future work. See [BACKLOG.md](BACKLOG.md) for the complete
list of open items by priority, and [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) for deeper
structural issues (H01, H02) that need their own investigation/design pass before fixing.

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

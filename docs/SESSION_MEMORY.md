# Session Memory

## Project and scope

Flipper Zero <-> ESP32-C6 over BLE. See [CLAUDE.md](../CLAUDE.md) for the project summary and
[docs/BASELINES.md](BASELINES.md) for pinned board/firmware/toolchain versions — not repeated
here.

## Current state (as of 2026-09-13, commit b23aec0)

**Phase 2 (core BLE transport through authenticated runtime sessions) is complete and hardware-verified.** Steps 1-7 are implemented and fully verified on real devices (ESP32-C6-DevKitC-1-N4 + Flipper Zero).

**Phase 3a (Flipper UI menu redesign) is complete and hardware-verified.** The Home screen is menu-driven (`HomeMenuItem`: Wardriving/Scan/GPS/Settings/About/Legacy; Up/Down move, OK selects), with Wardriving/Scan/GPS hidden unless a session is active and the board's capability registry supports them, and Settings/About/Legacy always visible. A `connection_lost` flag keeps the active screen in place on disconnect/session-fatal and shows a banner instead of snapping back to Home. All screens (Home, Scan, GPS, Wardriving, Settings, About) have been hardware-tested and work as designed.

**Design-vs-implementation notes:** The ViewDispatcher/scene-manager rewrite listed as a prerequisite in [docs/UI_REDESIGN.md](UI_REDESIGN.md) was deliberately skipped; the Home menu shell was built directly on the existing single `ViewPort`/`AppEvent`-queue pattern instead, and works reliably. The "Scan" menu item remains a Wi-Fi-scan/BLE-scan picker over the existing one-shot capabilities (not the five-mode BLE-active/passive live-view design), as this depends on a runtime BLE active/passive toggle still backlogged. Both limitations are tracked items, not regressions.

**Phase 3 (production-ready wardriving) is complete and hardware-verified.** `wifi_scan`, `ble_scan`, and `wardriving` are all implemented on both sides and hardware-verified:
- `wifi_scan` and `ble_scan`: manual on-device scan triggers with scrollable results views, each capped at 32 strongest results by RSSI.
- `wardriving`: autonomous Wi-Fi/BLE capture engine with checksummed circular log on raw flash, per-record GPS fix-dependency, incremental WiGLE CSV export to SD card.
- **Real GPS driver** (2026-09-12, hardware-verified 2026-09-13): UART1/NMEA GGA+RMC parser, three-state fix tracking (no_signal/acquiring/fix), per-record timestamps, live status polling for display.
- **LED indicators** (both firmwares): connection/session/flush-state visual feedback, hardware-confirmed working.
- **BLE active scanning** in `ble_scan` and within wardriving's capture engine.
- **Wardriving dedup** (128-slot address hash table with RSSI-improve gate and distance threshold).
- **CSV export dedup**: per-calendar-day files, no duplicate rows for the same address on the same day.

**All Phase 3 hardware-acceptance items complete:**
- ✅ Live multi-minute wardriving run at balanced duty cycle (`wifi_interval_ms=5000`, `ble_window_ms=100`, `ble_interval_ms=500`, ~12 WiFi scans/min + 20% BLE duty) — stable reconnects, reliable backlog drain, 6x denser WiFi coverage than prior 30s interval.
- ✅ Extended unattended flash-log wraparound/power-loss run — circular log correctly evicts by sector and survives interruptions.
- ✅ Flipper's WiGLE CSV export lands correctly on SD card with real timestamps and proper dedup.
- ✅ BLE active scanning effective for BLE-only wardriving isolation test (7/7 reconnects successful).
- ✅ Real GPS module cold-start-to-fix cycle, fix-dependent record discard/resume, real wardriving record timestamps.
- ✅ Stale wardriving log replay fixed (2026-09-13, commit b23aec0): old format records are now properly detected/cleared on boot instead of appearing as stuck backlog.

For the full roadmap, phase boundaries, and each step's "done when" criteria, see [docs/PLAN.md](PLAN.md). For the complete dated history of how each step was designed, implemented, and debugged — including every bug's root cause — see [docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md).

## Active investigation — needs hardware re-verification

**ESP32 disconnect without reconnect (BL07) — fix applied 2026-09-13, not yet hardware-retested.** After flashing `wifi_interval_ms=5000`, the ESP32 would go silent after 5-15 minutes and never reconnect until the Flipper FAP was restarted. Root cause (found via code review, no usable serial log was captured): an undocumented reconnect-time throttle forced wardriving's Wi-Fi source to scan *more* aggressively (every 400ms) during a reconnect attempt than the steady-state default — starving the BLE reconnect scan exactly when it needed radio time. Removed the entire throttle mechanism; wardriving's Wi-Fi source now scans at its configured interval (5s) unthrottled through reconnect. Build-verified clean, flashed to hardware; needs a live extended wardriving run with a forced disconnect to confirm reconnection now works reliably. See BACKLOG.md BL07 for full detail.

## Known backlog (other open items)

Step 8 (hardened persistent state, pairing-record/capability-file atomicity) and Step 9 (full negative-security-test suite) remain future work. See [BACKLOG.md](BACKLOG.md) for the complete list of open items by priority.

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

# Session Memory

## Project and scope

Flipper Zero <-> ESP32-C6 over BLE. See [CLAUDE.md](../CLAUDE.md) for the project summary and
[docs/BASELINES.md](BASELINES.md) for pinned board/firmware/toolchain versions — not repeated
here. Full dated narrative for anything marked done below lives in
[docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md); this file is current-state-only and is pruned
back whenever it re-bloats (most recently 2026-09-28, see PROJECT_HISTORY.md's
"SESSION_MEMORY.md prune" entry for everything moved out).

## Current state per board (as of 2026-09-28)

- **ESP32-C6 (`esp32c6-`, primary/reference board, Phase 1-3):** `wifi_scan`, `ble_scan`,
  `wardriving`, `gps` all implemented and hardware-verified end-to-end (pairing, sessions,
  capability registry, wardriving log/CSV export, real GPS driver). ✅ done — see
  PROJECT_HISTORY.md's early entries (2026-09-01 through 2026-09-13).
- **Heltec WiFi LoRa 32 V2 (`heltec-`, Phase 4, first unit `a4cf1203ba58`):** `wifi_scan`,
  `ble_scan`, `gps`, `wardriving` ported and hardware-verified (steps 1-9, 2026-09-16 to
  2026-09-23) except the `gps` NMEA read path and a live `wardriving` capture run, both still
  unconfirmed. `meshcore_scan`/`meshtastic_scan` (passive LoRa mesh detection, shared SX1276
  time-multiplexed 60s/side) and `mesh_log` (flash-backed sighting capture feeding
  wdgwars.pl) are build/host-test-verified only — no MeshCore/Meshtastic node has ever been
  available to test against. GPIO2 touch-pad Wi-Fi/BLE kill-switch and onboard SSD1306 OLED
  status display are both implemented; the OLED is hardware-verified, the kill-switch is
  build-verified/flashed with boot health unconfirmed. This board's DRAM/IRAM headroom is
  razor-thin (BL23/BL25/BL27 in BACKLOG.md — as low as single-digit bytes on some builds);
  treat any new `.bss`/`.data` addition on this board as needing a real `idf.py size` check.
- **Second physical Heltec unit (`a4cf1203b174`, Phase 9 bring-up):** flashed 2026-09-27; after
  that flash's own reset, COM10 stopped enumerating entirely (not just renumbering) and never
  came back within two polling windows — no boot log captured, runtime health on this specific
  build unconfirmed. Reconfirm the port and capture a fresh boot log before trusting it.
- **OLIMEX MOD-ESP32-C5 (`esp32c5-`, Phase 8):** `wifi_scan` (dual-band 2.4/5GHz, with a
  configurable `wifi_band` speed/coverage tradeoff), `ble_scan`, `gps`, `wardriving` all
  build-verified; pairing is hardware-verified, but a live `wifi_scan`(5GHz)/`wardriving`
  round-trip and this board's own radio-coexistence sweep are not (BL16). No factory-reset
  support by design (no onboard button, BL15). An earlier, unreproduced apparent reboot-loop
  after a `wifi_scan` trigger is tracked as BL18, not chased further.
- **Phase 9 (wired cluster, C6+C5+Heltec, design frozen — see [CLUSTER.md](CLUSTER.md)):**
  inter-board framing component and the Heltec+C6 worker/coordinator half are build-verified
  and flashed (2026-09-26); C5's 5GHz worker, the wardriving composite, and degraded-mode
  behavior are not started. The Phase 9 bring-up unit is the *second* physical Heltec above,
  not the Phase 4 unit.
- **Flipper FAP:** menu-driven Home/Scan/GPS/Wardriving/Settings/About UI (Phase 3a),
  Wardriving Stopped/Running screen split with persisted settings (Phase 7), and
  wardriving-CSV + mesh-node publishing to wdgwars.pl via BadUSB (Phase 6 + mesh_log
  follow-on) — all hardware-verified except the mesh_log publish path and the Mesh Log screen
  (build-verified only). Built from the **release** config (`fbt.cmd DEBUG=0`, not FBT's
  `-Og` default) since 2026-09-28 — see CLAUDE.md's build-commands section for why this is
  load-bearing, not a speed choice.
- **2026-09-28 full-codebase hardening pass:** every ESP target rebuilt at `-Os` with stack
  canary + WDT panic on; Flipper FAP memory footprint cut ~20% after a real "out of memory"
  reboot during a wardriving CSV flush was root-caused. Build- and host-test-verified only,
  **nothing hardware-flashed this pass** — [HARDENING_PLAN.md](HARDENING_PLAN.md) §0 has the
  full status table and is next session's hardware-test priority before trusting any board.

## In progress / next

- Reconfirm the second Heltec unit's serial port and capture a clean boot log (blocks trusting
  the kill-switch/OLED build on that unit).
- Hardware-verify: Heltec `gps` read path and `wardriving` capture; C5 5GHz `wifi_scan` +
  `wardriving` + coexistence sweep (BL16); the full 2026-09-28 hardening pass end-to-end.
- Mirror `cbor_meshcore.h`/`cbor_meshtastic.h`/`cbor_mesh_log.h` into `flipper/` where not
  already done (check `tools/check_shared_headers.py` for current status).
- Phase 9: wire in the C5 5GHz worker, then the wardriving composite and degraded-mode step.

## Open decisions

- `display`/`lora` capability wire design (Phase 4 step 6) — deliberately not designed yet,
  needs its own grill-me pass with the physical board in hand.
- Cluster degraded-mode behavior (Phase 9 step 6) — a product decision CLUSTER.md leaves open.
- Heltec's plain-LED "wardriving active" visual (BL14) and the C5's missing third LED state
  (BL17) are both judgment calls flagged for the user to confirm or override.

## Known live hazards

- Heltec DRAM/IRAM headroom is at or near single-digit bytes on several current builds
  (BL23/BL24/BL25/BL27) — treat this board as unable to absorb new `static`/`.bss` growth
  without a real `idf.py size` measurement first.
- This project has repeatedly hit the same `BleEventWorker`/task-stack-overflow bug class
  (steps 3, 5, 7, `wifi_scan`, and the Phase 9 cluster decoder) — default new BLE/radio
  callback-path buffers to file-scope `static`, and get a real `-fstack-usage` check before
  trusting one at a tight budget.
- Reconfirm serial ports before any hardware work — not guaranteed stable across reboots or
  sessions (COM9/COM8/COM10/COM11 in recent sessions, board-dependent).
- Check for concurrent peer Claude Code sessions on this repo before touching hardware or
  shared docs (this project frequently has several running at once) — use `git add -p` per
  file when staging alongside a peer's in-flight edits.

## Known backlog (other open items)

See [BACKLOG.md](BACKLOG.md) for the complete open-item list by priority, and
[HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) for deeper structural issues (H01, H02) needing
their own investigation/design pass before fixing. Persistent-state hardening (pairing-record/
capability-file atomicity, PLAN.md step 8's second half) and the full negative-security-test
suite (PLAN.md step 9's remaining bullet) are both still future work.

## Working conventions worth remembering every session

- Use the canonical scripts for build/flash instead of re-deriving environment setup:
  `tools/build_esp32.ps1` (build, optional `-Port`/`-SkipBuild`/`-CaptureBootLog`),
  `tools/build_flipper.ps1` (build, optional `-Port` to also transfer), `tools/flash_flipper.ps1`
  (transfer a built FAP to the Flipper's SD card via `runfap.py`). They already handle the
  Git-Bash/MSYS `export.ps1` pitfall and the Flipper's real (non-mass-storage) transfer method.
- Delegate mechanical doc sync (e.g. `docs/USER_GUIDE.md` updates via the `sync-user-guide`
  skill) and known-procedure hardware flash/verify passes to the cheapest capable model
  (Haiku), per this project's own convention.

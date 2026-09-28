# Implementation Plan

This plan implements the trusted-environment BLE pairing decision in [DECISIONS.md](DECISIONS.md) and protocol v2 in [PROTOCOL.md](PROTOCOL.md). The target board is the ESP32-C6 DevKitC-1-N4.

## Roadmap phases

- **Phase 1 (✅ done):** board/SDK/firmware/build baselines — see `docs/BASELINES.md`.
- **Phase 2 (✅ done):** core BLE transport, record framing, trusted-environment pairing, and authenticated runtime sessions on the ESP32-C6 — steps 1-7 implemented and hardware-verified.
- **Phase 3a (✅ done, hardware-verified 2026-09-13):** Flipper UI menu redesign — Home/menu shell with capability-aware routing, reconnect-stays-put behavior, GPS/Settings/About screens all working end-to-end. (Note: ViewDispatcher/scene-manager architecture prerequisite was skipped; built directly on existing ViewPort/AppEvent-queue pattern and works reliably. Five-mode BLE-active/passive Scan screen remains backlogged, pending runtime toggle.)
- **Phase 3 (✅ done, hardware-verified 2026-09-13):** Production-ready wardriving on the ESP32-C6. Includes `wifi_scan`, `ble_scan`, `wardriving` with real GPS driver, hardened flash log, per-record timestamps, WiGLE CSV export, LED indicators, and BLE active scanning. Field-usable unattended for hours, survives power loss, backlog drains reliably.
- **Phase 4 (in progress, started 2026-09-16):** Heltec WiFi LoRa 32 V2 board support — a second target (classic ESP32/Xtensa) adding display and LoRa. **Gate overridden by explicit user decision 2026-09-16** — Phase 3's backlog is not cleared and stays fully deferred, not interleaved with Phase 4 work; see the Phase 4 section below for the override rationale and current step status.
- **Phase 5 (scheduled later):** Zigbee/Thread and `gpio_control` — later-phase capabilities pending Phase 3/4 completion.
- **Phase 6 (implemented and hardware-verified end-to-end 2026-09-18):** wardriving log publishing to wdgwars.pl via a Flipper-triggered BadUSB/host-script flow. **Explicit user decision 2026-09-17: proceeds in parallel with Phase 4**, not gated on Phase 4 or Phase 5 completion — same kind of gate override Phase 4 itself carries for the Phase 3 backlog. A real publish against the live wdgwars.pl API succeeded end to end after five hardware bugs (stack-size MPU fault, CSV-path regression, DTR/port-discovery, CLI echo handling) were found and fixed. Full design and implementation-status notes: [docs/WARDRIVING_PUBLISH.md](WARDRIVING_PUBLISH.md).
- **Phase 7 (implemented and hardware-verified 2026-09-21):** Wardriving screen redesign — Stopped/Running screen split, persisted per-run settings (mode, WiFi scan-dwell "swelling", WiFi cooldown, BLE active/passive, WiFi country code), and GPS speed display/input. **Proceeded in parallel with Phase 4/6**, same gate-override pattern. Full design: [docs/WARDRIVING_REDESIGN.md](WARDRIVING_REDESIGN.md).
- **Phase 8 (started 2026-09-25):** OLIMEX MOD-ESP32-C5 board support — a third target (RISC-V,
  like the C6, but with a native dual-band Wi-Fi 6 radio) plus an ATGM336H GPS module wired to
  GPIO4/GPIO5. **Same gate-override pattern as Phase 4/6/7**: proceeds without waiting on any
  other phase's backlog. **Explicit scope limit: 2.4 GHz Wi-Fi only for this phase** — no 5 GHz
  scan/join code. **Explicit scope cut: no factory-reset support** — this board has no onboard
  pushbutton (see `docs/BACKLOG.md` BL15). See the "Phase 8: OLIMEX MOD-ESP32-C5 board support"
  section below for step tracking.
- **Phase 9 (design frozen 2026-09-26):** wired cluster — C6, C5, and Heltec wired together over
  UART, each dedicated to one scanning job (C6: 2.4GHz `wifi_scan`, C5: 5GHz `wifi_scan`, Heltec:
  coordinator/`ble_scan`/`meshcore_scan`/GPS/wardriving aggregation) to eliminate radio
  coexistence by construction and share one physical GPS module. Full design:
  [docs/CLUSTER.md](CLUSTER.md). See the "Phase 9" section below for step tracking.

For the full dated narrative of how each phase/step was designed, implemented, and debugged, see [docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md). For current state, see [docs/SESSION_MEMORY.md](SESSION_MEMORY.md); for the open backlog, see [docs/BACKLOG.md](BACKLOG.md).

## Confirmed setup choices

- Target board: ESP32-C6-DevKitC-1-N4, connected by USB for flash-size verification.
- Flipper build: standalone external FAP against Unleashed stable `unlshd-092` (API 88.4), pinned to commit `3c9be0fdd9d301a9436765099a2d1780b36a1795`.
- FAP compatibility: target the pinned Unleashed stable API only. A future firmware-version adapter is deferred to phase 2.
- ESP-IDF: use ESP-IDF `v5.5.2` for the initial C6 baseline; record the installed toolchain revision when setup completes.
- Initial phase 1 scope: ESP-IDF firmware skeleton and standalone FAP skeleton, both built before adding project code.

✅ Step 1 (establish the build baselines) — done, see [PLAN_ARCHIVE.md#1-establish-the-build-baselines](PLAN_ARCHIVE.md#1-establish-the-build-baselines).

✅ Step 2 (prove the BLE transport, incl. the long-run reconnect policy) — done, hardware-verified, see [PLAN_ARCHIVE.md#2-prove-the-ble-transport](PLAN_ARCHIVE.md#2-prove-the-ble-transport).

✅ Step 3 (define and implement record framing) — done, see [PLAN_ARCHIVE.md#3-define-and-implement-record-framing](PLAN_ARCHIVE.md#3-define-and-implement-record-framing).

✅ Step 4 (validate BLE / Wi-Fi radio coexistence, incl. the interval-bounds results) — done, see [PLAN_ARCHIVE.md#4-validate-ble--wi-fi-radio-coexistence](PLAN_ARCHIVE.md#4-validate-ble--wi-fi-radio-coexistence).

✅ Step 5 (implement trusted-environment pairing) — done, see [PLAN_ARCHIVE.md#5-implement-trusted-environment-pairing](PLAN_ARCHIVE.md#5-implement-trusted-environment-pairing).

✅ Step 6 (add authenticated runtime sessions) — done, hardware-verified, see [PLAN_ARCHIVE.md#6-add-authenticated-runtime-sessions](PLAN_ARCHIVE.md#6-add-authenticated-runtime-sessions).

## 7. Implement board identity and capability registry

- Generate and persist an immutable printable ASCII `board_id` during controlled provisioning or first boot before pairing. Derive it from the chip's factory-programmed MAC address to avoid collisions across multiple boards without a separate provisioning step.
- Implement `capability_query` and `capability_response` after runtime authentication only.
- Add commands incrementally, with input validation, request IDs, bounded output, and explicit `unsupported_capability` errors.
- **Scope split:** this step covers only the board-identity/capability-registry plumbing (`capability_query`/`capability_response` reporting `board`/`firmware`/`features`). The `command` message type and any real capability command handler (starting with `wifi_scan`) are a separate follow-on step — see below.
- **`capability_query` lifecycle:** sent exactly once per `board_id`, on the first successful runtime auth when no locally persisted capability file exists yet. The result is cached and never automatically re-queried — a deliberate exception to the project's usual re-verify-every-session pattern, since a capability list is non-sensitive cached metadata, not a security credential. The only refresh path is a full unpair + re-pair. See [CAPABILITIES.md](CAPABILITIES.md) for the full storage/lifecycle policy and the registry format.
- **Storage:** each board's capability record lives in its own file, separate from its pairing-secret file, so the pairing file can get step 8's atomic-write/versioning hardening without forcing the same constraints onto the non-sensitive capability cache. Unpairing a board deletes both files together.
- `board`/`firmware` are hand-maintained constant strings (not build-injected); `features` is a hardcoded compile-time list — see [CAPABILITIES.md](CAPABILITIES.md).

### Capability roadmap

Capabilities ship incrementally, gated on hardware actually present on a given board — see [CAPABILITIES.md](CAPABILITIES.md) for the registry format and the full, current capability list. See "Roadmap phases" above for how these map onto Phase 3/4/5.

1. **`wifi_scan`** (Phase 3) — first capability, needs no extra hardware. ✅ Implemented and hardware-verified 2026-09-07.
2. **`ble_scan` + `wardriving`** (Phase 3): add `ble_scan` and the composite `wardriving` capability. **Reordered 2026-09-07 to no longer wait on GPS hardware** — see "`ble_scan`, `wardriving`, and the GPS-stub reorder" below for why and how. **`ble_scan` implemented and hardware-verified 2026-09-08** (manual on-device scan trigger via Right button on the main screen, results in a scrollable view, capped at 32 devices by RSSI); **`wardriving` implemented on both firmwares and build/host-test-verified 2026-09-09, hardware verification pending** — ESP32 side: autonomous capture engine, power-loss-safe on-device flash log. Flipper side: a one-tap start/stop control/status screen (reachable via Up from the main screen), status/backlog-drain dispatch (including the `request_id = 0` unsolicited-backlog-drain case), and incremental WiGLE CSV export to SD card (one timestamped file per connected session, written record-by-record, never buffered in RAM). See [CAPABILITIES.md](CAPABILITIES.md) for the full design and this step's own "Done when" note above for implementation detail.
3. **Heltec board support** (Phase 4, separate baseline, starts after Phase 3 completes): display and LoRa capabilities on a second, structurally different board — see `docs/BASELINES.md`.
4. **Zigbee/Thread recon** (Phase 5a): passive `zigbee`/`thread` scanning/sniffing capabilities, matching the `wifi_scan`/`ble_scan` pattern — no network joining or commissioning.
5. **Zigbee/Thread participation** (Phase 5b, much later, separately scoped): active stack participation — an order of magnitude larger effort; not committed to a timeline.
6. **`gpio_control`** (Phase 5): generic GPIO control, reserving strapping/JTAG pins (GPIO0, 4, 5, 8, 9, 15) from generic control actions.
7. **`mesh_log`** (Heltec, Phase 6 follow-on, design frozen 2026-09-27, ESP32-side implemented 2026-09-27, build/host-test-verified only, `flipper/` mirroring not yet done): flash-backed capture/drain pipeline for MeshCore/Meshtastic node sightings, feeding wdgwars.pl's mesh-node upload — depends on `meshcore_scan`/`meshtastic_scan`. Full design: [WARDRIVING_PUBLISH.md](WARDRIVING_PUBLISH.md#mesh-node-publishing-design-frozen-2026-09-27-not-yet-implemented); implementation status: [SESSION_MEMORY.md](SESSION_MEMORY.md)'s 2026-09-27 entry.

### Multi-board pairing

- Multiple boards (e.g. a C6 and a Heltec) may be paired to one Flipper at a time, each with its own stored pairing record (step 5).
- The Flipper is the BLE peripheral, so only one BLE connection is active at a time: whichever paired board connects first occupies the slot. Switching boards today means powering down the currently-connected one so another can connect.
- **Backlog item:** a manual "disconnect current board" Flipper UI action to free the connection slot without physically powering off a board. Automatic arbitration is gated on an unresolved BLE-HAL feasibility question — whether the Flipper's peripheral role can advertise while already serving a connection.

**Done when:** Flipper renders only the authenticated capability list reported by the paired board, correctly reflecting that board's hardware. ✅ Met, and hardware-verified, on 2026-09-07 — see `docs/PROJECT_HISTORY.md` for the design decisions and implementation narrative (including a same-bug-class `BleEventWorker` stack regression found and fixed during implementation).

## 8. Harden persistent state and release configuration

- Store the C6 pairing record in a dedicated encrypted NVS namespace with a version, validity marker, and atomic replacement procedure. Cross-referenced from `docs/archive/CODE_REVIEW_FINDINGS.md` finding #17: today's ESP32 storage (`persist_pairing_secret`/`load_pairing_secret`) is a bare `nvs_set_blob()` with none of version/validity-marker/atomicity — a real, currently-uncosted gap against the written contract that this step owns closing.
- Implement explicit local unpair/factory-reset behavior and define which pairing record is removed on each side (unpairing one board must not disturb other stored pairing records — see step 7).
- Store the Flipper pairing record through an atomic app-owned storage update (temporary file, exact write verification, `storage_file_sync()`, close, rename) and do not log it. Treat local SD-card, debug, and modified-firmware access as outside the standalone FAP protection boundary (see [PROTOCOL.md](PROTOCOL.md) "Implementation security requirements" — this is an accepted limitation, not a gap to close in this phase).
- The wardriving buffer uses the same atomic-persistence philosophy: a hand-rolled, checksummed, append-only log on raw flash (not a FAT-based wear-levelling filesystem), so an unclean power loss (e.g. car ignition cut) loses at most the single record being written at that instant, never the rest of the log. Circular — when full, evicts the oldest **erase-sector's worth** of records at once (raw NOR flash only erases a whole sector at a time; true single-record eviction would need a wear-levelling translation layer, which this bullet's own "not a FAT-based wear-levelling filesystem" already rules out) — not literally the single oldest record. See [PROTOCOL.md](PROTOCOL.md)'s "Flash log eviction" note.
- **Scope note (added 2026-09-07):** the wardriving-log half of this step is being built now, ahead of the rest of Phase 3, as part of "`ble_scan`, `wardriving`, and the GPS-stub reorder" above — not deferred to a later pass through step 8. The pairing-record/capability-file persistence hardening (the first two bullets above) remains deferred; this step isn't "done" until those land too.

**Status:**
- **Wardriving-log persistence:** ✅ done (checksummed circular flash log, `esp32/main/wardriving_log.c`), hardware-verified 2026-09-13 (extended wraparound/power-loss runs, stale record cleanup on boot/replay).
- **Pairing-record/capability-file hardening:** not yet started — future work after Phase 3 completion.

**Done when (future):** interrupted writes, reboot during pairing, unpair, and factory reset leave no ambiguous paired state for pairing records and capability cache files (matching the robustness already achieved for wardriving log).

**See also:** "Deferred: hardware hardening" below — Secure Boot, flash encryption, and related eFuse-dependent work are explicitly out of scope for this phase and are not part of this step's "done when" bar.

## 9. Validate the complete system

System-level validation covering codec/cryptography, error handling, reconnection, and production workloads.

**Completed sub-items:**
- ✅ **Codec/crypto test vectors:** all host-native test suites pass on both targets (481 Flipper checks, 200+ ESP32 checks covering HMAC, AES-GCM, X25519, CBOR, framing).
- ✅ **Error handling:** fragment/CBOR malformed-input rejection tested; session auth failure/replay/sequence-gap tested.
- ✅ **Reconnection flows:** reset-and-repair (2026-09-06), forced-disconnect-during-wardriving (2026-09-11), idle-timeout reconnect (ongoing), all tested on hardware.
- ✅ **Radio-coexistence under real load:** 20% BLE duty cycle (`ble_window_ms=100`, `ble_interval_ms=500`), continuous Wi-Fi scanning, live wardriving streaming tested for hours without idle-timeout spurious disconnect.
- ✅ **Merged-reconnect-scan mechanism:** BLE-only isolation (7/7 reconnects), BLE+Wi-Fi coexistence (reconnect stalls when Wi-Fi source runs concurrently — accepted as Wi-Fi duty-cycle tuning issue, not a protocol bug).
- ✅ **Power loss resilience:** wardriving log circular wraparound tested; old format records gracefully skipped on boot.
- ✅ **GPS cold-start-to-fix cycle:** real NMEA module hardware-verified; fix-dependent record discard/resume working.
- ✅ **WiGLE CSV export:** real SD card, proper timestamping, dedup validation.

**Remaining (future phase work):**
- Full negative-security-test suite (ciphertext tampering, replay, bad confirmation flows).
- Fuzz testing with truncated/oversized/invalid CBOR/framing inputs.
- Multi-day unattended operation at production duty cycles.

**Done when:** ✅ Phase 3 acceptance bar met 2026-09-13 — full pairing-to-wardriving-export flow verified end-to-end on real hardware with production workloads.

## Deferred: hardware hardening (explicitly out of scope for this phase)

Physical possession of either paired device (Flipper or ESP32) is accepted as fully compromising to that device's stored secrets and data for the current phase — see [PROTOCOL.md](PROTOCOL.md) "Implementation security requirements." Treat a lost or stolen paired device as game over for that pairing relationship: reset (ESP32) or unpair (Flipper) immediately. Other stored pairings are unaffected.

Because of that accepted threat model, none of the following are required for any "done when" bar in this plan, and none of them are scheduled:

- Secure Boot, flash encryption, and NVS encryption on the ESP32.
- Signed firmware updates and production debug/download restrictions.
- Any irreversible eFuse configuration.

**No irreversible hardware operations are to be performed on any board in this phase.** If this work is ever picked up, it requires a dedicated sacrificial board for eFuse validation before any irreversible setting is applied to a board actually in use — do not attempt it on the primary development board.

✅ Wi-Fi scan capability (Phase 3 follow-on step) — done, hardware-verified 2026-09-07, see [PLAN_ARCHIVE.md#wi-fi-scan-capability-phase-3-follow-on-step](PLAN_ARCHIVE.md#wi-fi-scan-capability-phase-3-follow-on-step).

✅ `ble_scan`/`wardriving` + the GPS-stub reorder (Phase 3) — both done and hardware-verified (`ble_scan` 2026-09-08, `wardriving` 2026-09-10), see PLAN_ARCHIVE.md for the full design/implementation record.

✅ Real GPS driver, wardriving fix-dependency, and real wardriving-record timestamps (Phase 3) — done, hardware-verified 2026-09-13 (see Phase 3's status line above), see PLAN_ARCHIVE.md for the full design/implementation record.

## Phase 4: Heltec WiFi LoRa 32 V2 board support

**Status: in progress, started 2026-09-16.** Phase 3's backlog was **not** cleared when this
phase started — `docs/BACKLOG.md` still had open P0 items (G03, G06, G07, G09, BL01) and P1
items (G08, G10, BL05, BL06, G36, BL04, BL10, BL11, BL12), and `docs/HARDENING_BACKLOG.md` had
H01–H04 open. **The user explicitly overrode this phase's own gate ("does not start until
Phase 3 backlog is cleared") on 2026-09-16**, via a grill-me design session, choosing to start
Phase 4 now rather than clear the backlog first. Backlog items stay fully deferred, not
interleaved with Phase 4 work — see `docs/BACKLOG.md`'s note. The step breakdown below,
originally written as a frozen plan for a future session, is now the actual in-progress step
list.

**No physical Heltec board has been acquired.** Hardware facts are recorded in
[docs/hardware/heltec-wifi-lora-32-v2/README.md](hardware/heltec-wifi-lora-32-v2/README.md),
sourced from Heltec's/Espressif's published documentation and clearly marked
per-vendor-documentation-only, not locally verified — mirroring the precision discipline of
[docs/hardware/esp32-c6-devkitc-1/README.md](hardware/esp32-c6-devkitc-1/README.md). See
`docs/BASELINES.md`'s Heltec stub for the pinned-baseline framing of this board.

✅ Architecture decision: shared component — confirmed 2026-09-16 (option (a), one shared `components/feb_protocol/` component consumed via `EXTRA_COMPONENT_DIRS`), see [PLAN_ARCHIVE.md#architecture-decision-shared-component-confirmed-2026-09-16](PLAN_ARCHIVE.md#architecture-decision-shared-component-confirmed-2026-09-16).

### Step breakdown

Mirrors how Phase 1–3 steps are written above (numbered steps, each with a "Done when" line).
Step 1 is done (2026-09-16); steps 2–6 have not started.

✅ Step 1 (board acquisition + baseline bring-up) — done, hardware-verified 2026-09-16, see PLAN_ARCHIVE.md ("Phase 4 -- Step 1").

✅ Step 2 (project scaffolding) — done 2026-09-16, see PLAN_ARCHIVE.md ("Phase 4 -- Step 2").

✅ Step 3 (port BLE transport + pairing + session crypto onto classic ESP32) — done, hardware-verified 2026-09-16, see PLAN_ARCHIVE.md ("Phase 4 -- Step 3").

✅ Step 4 (`board_id` / multi-board-pairing implications) — done, verified 2026-09-16, see PLAN_ARCHIVE.md ("Phase 4 -- Step 4").

**5. Radio/coexistence note.** Unlike the C6 (one 2.4 GHz radio shared by Wi-Fi, BLE, and
802.15.4), the Heltec's LoRa radio is a separate SPI-attached chip (SX1276/SX1278) on its own
antenna and sub-GHz frequency band — it does not compete with the classic ESP32's Wi-Fi/BLE
radio the way `wifi_scan`/`ble_scan`/`wardriving` compete for the C6's single radio today. That
said, the classic ESP32's Wi-Fi+BT combo radio still needs its own from-scratch coexistence
validation (mirroring Phase 3/step 4's methodology) before `wifi_scan`/`ble_scan`/`wardriving`
are trusted on this board — it is a different SoC with a different radio implementation; none
of the C6's measured bounds are assumed to transfer.

**Done when:** an equivalent of step 4's coexistence sweep (Wi-Fi scan + active BLE connection +
BLE observer scan running together) passes on the Heltec board, with its own recorded interval
bounds — not borrowed from the C6's. **Skipped by explicit user decision, 2026-09-16** — not
done, not attempted. No coexistence bounds exist for this board's Wi-Fi+BT combo radio. Whoever
later ports `wifi_scan`/`ble_scan`/`wardriving` onto the Heltec (no such step is currently
written into this Phase 4 plan) must not assume the C6's bounds transfer and must do this
validation first, or as part of that work.

**6. `display`/`lora` capability design — explicitly out of scope for this document.** Sensible
scope for a `lora` capability (recon/sniff only vs. TX capability, frequency/region regulatory
constraints, antenna-presence assumptions) and a `display` capability (what it renders, whether
it's push or poll, ESP32-local vs. Flipper-driven) cannot be responsibly designed without the
physical board in hand and steps 1–5 done first. This needs its own dedicated grill-me design
pass, the same way `gps` got one before implementation (see the "Real GPS driver..." section
above) — do not design or implement `display`/`lora` wire formats as a side effect of this
phase's earlier steps.

✅ Step 7 (`wifi_scan`/`ble_scan` capability porting) — done, build-verified 2026-09-17, real hardware round-trip confirmed the same day (see `docs/SESSION_MEMORY.md`), see PLAN_ARCHIVE.md ("Phase 4 -- Step 7").

✅ Step 8 (`gps` capability porting) — done, build-verified and flashed 2026-09-23; GPS read-path hardware verification status tracked in `docs/SESSION_MEMORY.md`, see PLAN_ARCHIVE.md ("Phase 4 -- Step 8").

✅ Step 9 (`wardriving` capability porting) — done, build-verified and flashed 2026-09-23; hardware capture/coexistence verification status tracked in `docs/SESSION_MEMORY.md`, see PLAN_ARCHIVE.md ("Phase 4 -- Step 9").

### Cross-references

- Board/pin facts: [docs/hardware/heltec-wifi-lora-32-v2/README.md](hardware/heltec-wifi-lora-32-v2/README.md).
- Baseline framing: `docs/BASELINES.md`'s Heltec stub section.
- Capability roadmap placement: `docs/CAPABILITIES.md`'s closing note on `display`/`lora`.
- Do not carry forward C6 pin mappings or radio-coexistence bounds to this board, or vice versa
  — both hardware docs state this caution independently.

## Phase 8: OLIMEX MOD-ESP32-C5 board support

**Status: in progress, started 2026-09-25.** Same gate-override pattern as Phase 4/6/7 — does
not wait on any other phase's backlog. Reuses Phase 4's already-confirmed architecture decision
(shared `components/feb_protocol/` component, consumed via `EXTRA_COMPONENT_DIRS`) rather than
re-litigating it — this board is a fourth consumer of that component, not a new structure.

**Scope reversal 2026-09-25:** the initial "2.4 GHz only" and "no wardriving this phase" cuts
below were lifted the same day, after step 2 (2.4GHz-only `wifi_scan`/`ble_scan`/`gps`) was
already hardware-verified — explicit user request to add 5 GHz scanning and port `wardriving`.
Kept here, struck through in spirit but not in text, because the reasoning still matters: this
was a genuine scope expansion, not a course-correction of a mistake.

**Original scope limits for step 2 (both user-confirmed 2026-09-25, now superseded — see
step 3 below):**
- ~~2.4 GHz Wi-Fi only.~~ Superseded: this board now scans both bands.
- **No factory-reset support** (unchanged, still in effect). This board has no onboard
  pushbutton (schematic-confirmed — see the hardware doc below) to bind the C6/Heltec's
  BOOT-hold-5s gesture to. Tracked as `docs/BACKLOG.md` BL15; revisit only if the user wires an
  external button or requests a different mechanism.

✅ Step 3 (dual-band Wi-Fi + `wardriving` port) — done, build-verified 2026-09-25; hardware verification and the BL16/BL18 open items tracked in `docs/BACKLOG.md`/`docs/SESSION_MEMORY.md`, see PLAN_ARCHIVE.md ("Phase 8 -- Step 3").

✅ Step 4 (`wifi_band` — configurable dual-band scan speed/coverage) — done, build-verified 2026-09-26; hardware verification tracked in `docs/SESSION_MEMORY.md`, see PLAN_ARCHIVE.md ("Phase 8 -- Step 4").

Hardware facts, pin map, and the ATGM336H GPS wiring are recorded in
[docs/hardware/olimex-mod-esp32-c5/README.md](hardware/olimex-mod-esp32-c5/README.md); pinned
baseline framing in `docs/BASELINES.md`'s MOD-ESP32-C5 entry.

### Step breakdown

Mirrors Phase 4's step numbering/shape.

✅ Step 1 (board acquisition + baseline bring-up) — done, hardware-verified 2026-09-25, see PLAN_ARCHIVE.md ("Phase 8 -- Step breakdown Step 1").

✅ Step 2 (port BLE transport + pairing + session crypto, then `wifi_scan` 2.4 GHz-only/`ble_scan`/`gps`) — done, build-verified and pairing-hardware-verified 2026-09-25, see PLAN_ARCHIVE.md ("Phase 8 -- Step breakdown Step 2").

### Cross-references

- Board/pin facts: [docs/hardware/olimex-mod-esp32-c5/README.md](hardware/olimex-mod-esp32-c5/README.md).
- Baseline framing: `docs/BASELINES.md`'s MOD-ESP32-C5 entry.
- Do not carry forward C6/Heltec pin mappings, LED wiring, or radio-coexistence bounds to this
  board, or vice versa — this board's radio (dual-band Wi-Fi 6 + BLE 5 + 802.15.4) has no
  validated coexistence sweep of its own yet, same caution as Phase 4 step 5's gap for Heltec.

## Phase 9: wired cluster (C6 + C5 + Heltec, distributed scanning)

**Status: design frozen 2026-09-26, no code written yet.** Full design (motivation, role
assignment and why, physical wiring, inter-board protocol sketch, composite behaviors, open
questions): [docs/CLUSTER.md](CLUSTER.md) — read that first, this section is step tracking only.

**Same gate-override pattern as Phase 4/6/7/8**: does not wait on any other phase's backlog.

**Roles** (forced/derived, not arbitrary — see CLUSTER.md's reasoning): Heltec is the
coordinator (sole Flipper BLE link, `ble_scan`, `meshcore_scan`, GPS, wardriving
aggregation/flash-log/CSV); C6 does `wifi_scan` 2.4GHz-only; C5 does `wifi_scan` 5GHz-only.
Neither C6 nor C5 talks BLE to the Flipper in cluster mode, and their standalone
BLE-to-Flipper builds are kept as a separate, unaffected build variant — confirmed with the
user, no regression to solo operation for either board.

### Step breakdown

**1. Physical wiring + inter-board link bring-up.** Wire Heltec↔C6 and Heltec↔C5 UART links
(TX/RX/GND only, no shared bus). Pick GPIO pins per board avoiding strapping/JTAG pins
(GPIO0/4/5/8/9/15) and record them in each board's `docs/hardware/*/README.md` once chosen —
this needs the physical boards in hand, not guessed ahead of time.

**Done when:** a raw byte round-trips over both links on real hardware, with no protocol logic
yet — just confirming the wiring and UART peripherals work.

**2. Inter-board framing.** Design and implement the minimal checksummed, unencrypted
length-prefixed frame format from CLUSTER.md as a new shared component (mirroring
`components/feb_protocol/`'s pattern), so coordinator and worker sides implement it identically.
No third-party library — hand-rolled, matching this project's existing codec convention.

**Done when:** host-native tests exercise encode/decode plus rejection of malformed/corrupted
frames (checksum mismatch, truncated length), the same bar `framing.c`'s existing tests already
meet for the BLE-facing protocol.

**3. Cluster-mode worker firmware (C6, C5).** A new build variant, single-band Wi-Fi-scan-only,
with no BLE stack, no pairing/session/crypto code compiled in at all. Reports `scan_result`
frames continuously over its UART link. Built alongside each board's existing standalone
`main.c` — not a replacement.

**Done when:** each board's cluster-mode build boots and streams recognizable frames out its
UART, verified with a host-side (PC) serial capture, before any coordinator-side code exists to
consume them.

**C6 half: ✅ build-verified 2026-09-26**, hardware-verification pending. New permanent project
`esp32/cluster_worker/` (sibling to `esp32/main/`, untouched), wired to the new
`components/feb_cluster_link/` shared component. `idf.py build` clean, zero warnings. 2.4GHz-only
Wi-Fi STA scan, no BLE/crypto; sends `WORKER_HELLO` every 1000ms; a single-owner
`scan_ctl_task`/FreeRTOS-queue design (a plain-FreeRTOS analogue of the standalone firmware's
`ble_npl_callout` handoff pattern, since there's no NimBLE host task here) reacts to
`SCAN_CONFIG_SET`'s `mode` (idle/continuous/manual-one-shot) and `dwell_mode`, reusing the
standalone firmware's phy-generation-collapsing/auth-mapping/swelling-timing logic directly
rather than re-deriving it. One real gap found and flagged, not solved:
`dwell_mode=speed_based` has no GPS on this board and no speed field in the frame to carry one —
see CLUSTER.md's "Open questions". A stack-overflow risk found during the coordinator-dispatch
pass (a ~4KB multi-frame array as a stack-local in a 4096-byte task) is **fixed** — switched to
byte-at-a-time decoding via `feb_cluster_decoder_feed_byte()`, task stack bumped to 6144 for
headroom; rebuilt clean, host tests still 25/25. C5 half not started — follows once this pair
(Heltec+C6) is confirmed working end-to-end, per the staged rollout above.

**4. Coordinator dispatch (Heltec).** Worker presence detection (`worker_hello`), config
forwarding (`scan_config_set`), and a manual-scan request/merge/reply path wired into the
existing `capability_query`/`command` dispatch — a real Flipper `wifi_scan` request now merges
results from two physically separate radios into one reply.

**Done when:** a real Flipper's manual `wifi_scan` against Heltec returns a single merged
top-32 list sourced from both C6 (2.4GHz) and C5 (5GHz) hardware, hardware-verified end-to-end.

**Heltec↔C6 half: ✅ build-verified 2026-09-26**, hardware-verification pending. Additive change
to the existing standalone `heltec/main/main.c` (no separate build variant — presence detection
happens at runtime, so an unwired board behaves exactly as before). UART2 (TX=GPIO32/RX=GPIO33
— UART1 was already GPS's). `wifi_scan` now proxies to the C6 worker when a `WORKER_HELLO`
arrived within the last 3000ms, else falls back to today's local-radio scan unchanged;
`wardriving`'s Wi-Fi source is untouched (step 5 scope, not this one). Existing top-32-by-RSSI
selection logic reused verbatim for both sourcing paths, not duplicated. A real stack-overflow
risk was found and is being fixed in the C6 worker side during this same pass (a ~4KB frame
array as a stack-local in a 4096-byte task — same recurring bug class this project has hit four
times before). The numeric→text `phy`/`auth` conversion stays coordinator-only for now (only
consumer that exists) — not promoted to a shared header, revisit if a second consumer appears.

**5. Wardriving composite.** Coordinator forwards config at `start`, continuously ingests both
workers' streamed scan hits, geotags/dedups/logs them alongside its own `ble_scan` hits into the
same flash-backed circular log, exactly as today's single-board wardriving engine already does
for one source.

**Done when:** a live multi-minute wardriving run produces one merged flash log/CSV export
containing all three scan sources (C6's 2.4GHz, C5's 5GHz, Heltec's own BLE) with GPS
timestamps, hardware-verified.

**6. Worker-absence / degraded-mode behavior.** Define and implement what the coordinator
reports to the Flipper when a worker's UART link is down or never sent `worker_hello` — a
product decision CLUSTER.md deliberately leaves open, not an engineering default to assume.

**Done when:** unplugging a worker mid-session produces defined, tested behavior (not a hang or
crash), and the decision made is recorded here or in CLUSTER.md.

**Staged rollout, confirmed 2026-09-26**: implementation proceeds with Heltec+C6 wired and
validated first (steps 1-6 exercised end-to-end as a two-board cluster: coordinator +
2.4GHz-only worker), before C5 is wired in at all. C5's 5GHz `wifi_scan` worker (repeating
steps 1/3/4 for the second link) is a follow-on once the Heltec↔C6 pair is confirmed working —
not built/wired simultaneously with C6 from the start.

**Explicitly out of this phase's "done when" bar** (carried-forward, pre-existing, unrelated
gaps — see CLUSTER.md's "Open questions"): a coexistence sweep for Heltec's LoRa radio running
concurrently with its own combo-radio BLE activity (`docs/BACKLOG.md` BL19-class gap, unchanged
by this design).

## Backlog

Every open, not-yet-scheduled item (defects, deferred product decisions, cost/efficiency work)
now lives in the single centralized [docs/BACKLOG.md](BACKLOG.md) — that file explains how to
use it and links to full detail per item. Resolved items' full narrative is in
[docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md); this file does not keep a shadow "resolved" log.

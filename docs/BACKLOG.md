# Backlog

**2026-09-16: Phase 4 (Heltec board support) started despite this backlog not being cleared** —
an explicit user decision overriding `docs/PLAN.md`'s "does not start until Phase 3 backlog is
cleared" gate (see `docs/PLAN.md`'s Phase 4 section). Everything below stays fully deferred, not
interleaved with Phase 4 work, per that same decision.

The single, centralized list of every open, actionable item that isn't part of the current
roadmap step's own scope: code defects, robustness gaps, deferred product decisions, and
cost/efficiency work. This is the "check before starting related work" list for anything not
already blocking the phase in progress.

- **Current in-flight state** (what's being worked on right now) lives in
  [SESSION_MEMORY.md](SESSION_MEMORY.md), not here.
- **Roadmap steps and their "done when" bars** live in [PLAN.md](PLAN.md), not here.
- **Finished work** (bug found, root-caused, fixed, verified) belongs in
  [BACKLOG_COMPLETED.md](BACKLOG_COMPLETED.md) (a scannable one-line-per-item archive) and
  [PROJECT_HISTORY.md](PROJECT_HISTORY.md) (the full narrative) — mark an item `DONE
  YYYY-MM-DD (commit)` below only long enough for the next session to notice, then move the
  row to BACKLOG_COMPLETED.md once the PROJECT_HISTORY.md entry exists.

Each row is a one-line pointer, not the full write-up — follow the link for exact file/line
evidence, a suggested fix, and tests to add. Do not re-derive detail that already lives
elsewhere:

- **G-numbered items** → [archive/grok-4.6-findings-2026-09-11.md](archive/grok-4.6-findings-2026-09-11.md)
  (evidence, exact fix, tests, dependencies).
- **BL-numbered items** → detail inline below (no separate appendix exists for these).
- **Roadmap-gated items** → the named `PLAN.md` step. Do not fix them out of order or sneak
  them into an unrelated PR — see `CLAUDE.md`'s conventions.
- **H-numbered items** → [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md). Deeper structural/robustness
  issues found during live testing that need their own investigation/design pass before being
  fixed, as distinct from the ready-to-fix bugs and deferred product decisions here.

Severity scale (matches the grok appendix): **P0** spec/security correctness · **P1** real bug
in normal use · **P2** robustness/defense-in-depth/cost · **P3** style/docs drift.

## P0 — correctness / security

| ID | Title | Status |
| --- | --- | --- |
| G03 | ESP32 marks the session `AUTHENTICATED` on its own GATT write-complete, not on peer confirmation | Open — needs a product decision on the `capability_query`-caveat (see appendix) |
| G06 | Neither firmware sends the spec-mandated `unsupported_version` error + close | Open |
| G07 | Any `send_protected*` clobbers an in-flight wardriving backlog drain (generalizes past `start`) | Implemented 2026-09-14 with a bounded ESP32 protected-TX FIFO; hardware reconnect/wardriving retest pending. See [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H03. |
| G09 | Flipper advances `session_seq_out` even when notify delivery is unknown | Open |
| BL01 | Flipper's `handle_pair_init()` runs unconditionally on any incoming `pair_init` — no local user-gesture/authorization-state check, contradicting [PAIRING.md](PAIRING.md) step 3's "user selects Add ESP32 board" | Open — low severity, works in practice; needs design clarification |

## P1 — real bugs in normal use

| ID | Title | Status |
| --- | --- | --- |
| G08 | SD-card I/O (pairing, capability cache, CSV) runs on `BleEventWorker`, the BLE-pump thread | **Implemented 2026-09-29, build- and host-test-verified; hardware pending.** BleEventWorker now only decrypts/decodes and hands work to the main thread through static buffers plus existing AppEvents:<br>• **Wardriving CSV:** a 32-slot record ring (`wardriving_rx.c`), one full batch. There's no Flipper→ESP storage ack, so overflow logs and drops.<br>• **mesh_log:** an 8-slot ring.<br>• **Pairing save:** the secret is copied under `protocol_mutex` and zeroized on every path; Done/Failed is still posted only after the save.<br>• **Capability cache:** a 512 B hand-off buffer.<br>• **Exit:** pending saves are drained before teardown.<br>`profile_event_handler` frame 96 → 88 B. **Cost / decision for the user:** resident FAP heap **+6,703 B** (`.bss` +4,364, mostly the 3,328 B CSV ring; `.text` +1,672), over the ~1.5 KB soft budget and relevant to H04's OOM history. It can shrink by accepting tail-drops on full batches, or by adding a storage-completion ack to the protocol. Hardware: pair, reconnect, full backlog drain (CSV row count), mesh_log push. |
| G10 | Flipper `session_key`/`session_seq_out`/`outgoing_message_id` accessed from two threads with no lock | **Implemented 2026-09-28** (HARDENING_PLAN.md HP-06/HP-07). `protocol_mutex` serializes every encrypt/decrypt and all pairing/session-state mutation, and a connection-generation counter stops a stale main-thread `BtStatus` reset from wiping a session BleEventWorker already advanced. Build- and host-test-verified; hardware pending. |
| G13 | ESP32 NVS pairing blob has no version, validity marker, or atomic replacement | **Roadmap-gated → PLAN.md step 8.** Do not fix as a drive-by. |
| BL05 | Flipper reboots with `furi_check_failed` on app relaunch after wardriving | Open — 2026-09-25: user confirmed the trigger is specifically an active wardriving session, corroborating this row's original title. Found and fixed a concrete cross-thread BLE-profile-teardown race (`stop_service()` freed the profile/unregistered its BLE event handler before the connection was actually severed, with no settle delay — unlike the pinned firmware's own `hid_app.c` reference pattern for the same sequence, which waits 200ms). Build-verified, not yet hardware-tested. See `docs/HARDENING_BACKLOG.md` H04's 2026-09-25 entry for full mechanism, the confidence-ranked alternative theories, and what still needs a live repro. |
| BL06 | Flipper does not reconnect when coming back in range of ESP32 while on wardriving screen during wardriving | Open |
| G36 | Wardriving BLE reconnect can stall permanently | **Partially explained, not fully resolved.** BLE-only isolation test (7/7 successful reconnects) confirmed Wi-Fi coexistence starvation is *a* cause. But a live retest 2026-09-13 (after removing BL07's throttle) found a stall with a *different* mechanism — `wardriving_ble_interval_cb()`'s periodic re-arm colliding with its own in-flight connect attempt, independent of Wi-Fi entirely. See [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H01 for the full evidence and proposed fix. Do not treat this as closed. |
| BL04 | Wardriving CSV dedup table resets on disconnect, but file lifetime is per-calendar-day — same-day reconnect can re-log an address already written earlier that day | Open — lower priority (correctness is preserved, just allows edge-case duplicate rows within a day); candidate fix is to seed dedup table from existing file on reopen. |
| BL10 | Wardriving screen shows `Start (delayed)` regardless of whether the ESP32 currently has a GPS fix | Open |
| BL11 | GPS fix indicator on wardriving screen does not reflect the ESP32's actual GPS status; avoid reintroducing polling on this screen without resolving the prior polling-related issues | Open |
| BL28 | Heltec `gap_event` DISC_COMPLETE (~L3987) restarts the reconnect scan without the `!wardriving_ble_active` guard that C6 (L3518) and C5 have (C6 commit `ffdd20e`, missed by the 2026-09-23 Heltec wardriving port), so a reconnect scan can start alongside the wardriving BLE source | **Fixed 2026-09-29, build-verified.** C6's condition was ported verbatim ([SOURCE_SPLIT.md](SOURCE_SPLIT.md) DR1). Hardware pending: Heltec wardriving + disconnect/reconnect check. |
| BL29 | Flipper LED stays green after a wardriving backlog flush finishes instead of returning to its normal state | **Candidate fix 2026-09-29, root cause not confirmed; build-verified, not reproduced.** Found one path that leaves the LED green: `handle_hello()` (`session_flow.c`) calls `session_reset_state()`, which clears `app_wardriving_flush_led_active` but, unlike every other reset site, never resets the physical LED. A `hello` that fails on an already-flushing connection without a disconnect would leave it green. That path now also does `sequence_blink_stop` + `sequence_set_only_blue_255`. This may not be the trigger you hit. A live repro should show whether a second `hello` landed on the same connection, or whether `backlog_remaining` reached 0 before the LED stuck. Keep open until confirmed on hardware. |
| BL30 | Wardriving with the BLE scan mode set to "passive" produces no BLE results | **Implemented 2026-09-29, build-verified; hardware pending (not reproduced live).** Root cause, all three ESP boards: a passive-only start sends `sources=["ble_passive"]`, which sets `want_ble=false`, and `wardriving_start_internal()` only opened BLE discovery inside `if (want_ble)`, so BLE never started (command, autostart and button-toggle paths alike). Second facet: `wardriving_persist.c`'s load validation rejected `!want_wifi && !want_ble`, so a saved passive-only run was reset to defaults with autostart off. Both fixed (`feb_cap_wardriving*.c`, `heltec/main/main.c`, `components/feb_wardriving/wardriving_persist.c`). Hardware: passive-only BLE wardriving on C6, C5, Heltec produces BLE rows. |
| BL31 | MOD-ESP32-C5's status LEDs need a new behavior mapping | Open — reported by user 2026-09-29; the desired mapping isn't specified yet, so confirm it with the user before implementing. Related: BL17 (C5 has no wardriving-active LED indication) |

## P2 — robustness / cost / defense-in-depth

| ID | Title | Status |
| --- | --- | --- |
| G18 | Flipper X25519 donna static ladder scratch (~3-4 KB) never zeroized, resident for the app's lifetime | **Fixed 2026-09-28**, part of the H04 memory pass. `donna_sc` is `malloc`'d per `x25519_donna_scalarmult()` call, then `feb_secure_zero()`'d and freed (`flipper/pairing_crypto.c:~827-855`). Build-verified. |
| G23 | Flipper reassembly-complete buffer read after mutex release; `profile_start()` resets it unlocked | Open |
| G25 | 256-byte stack buffer in the ESP32's NimBLE notify-RX path (same class as 4 prior stack-overflow bugs) | **Fixed 2026-09-29, build-verified; hardware pending.** The buffer was on all three ESP boards, not just the C6. Now `static` in `components/feb_app_core/feb_central.c` (C6 + C5) and `heltec/main/main.c`. Costs 256 B of `.bss` per board; on the Heltec that leaves **92 B DRAM free** (was 356 B, see BASELINES.md). |
| HP-44 | ESP-side X25519 (`components/feb_protocol/pairing_crypto.c`) uses `mbedtls_mpi` `exp_mod`, which is not constant-time (self-flagged in the source comment). Low risk: the ceremony is a one-time, reset-gated event. | Open. Consider mbedTLS's ECP Curve25519 (`mbedtls_ecp_mul`). See HARDENING_PLAN.md. |
| BL07 | ESP32 status LED turns off instead of green when the Flipper app is closed while wardriving continues, then reopened and reconnected while the ESP32 flushes its backlog | Open |
| BL09 | Filter the paired Flipper's BLE address out of scan and wardriving results | **Implemented 2026-09-29, build-verified; hardware pending.** User decision: wardriving capture only; manual `ble_scan` still lists it. Each board remembers the Flipper's over-the-air address (`peer_ota_addr`) on connect, keeps it across disconnects, and skips matching records at BLE window close (`feb_central.c`/`feb_cap_scan.c` for C6+C5, `heltec/main/main.c`). Hardware: the Flipper never appears in the wardriving CSV but still shows in a manual BLE scan. |
| BL12 | Wardriving flash-log capacity (`esp32/partitions.csv`'s "wardrive" partition, ~14,000-21,000 records depending on average record size) can be reached within about a day of unattended autostart capture, with no wire-visible warning as the backlog approaches capacity — a real risk now that autostart (`4cd6c7d`, 2026-09-16) makes multi-day unattended runs realistic rather than requiring a Flipper to have started the session | Open — surfaced 2026-09-16 by a real 16,838-record backlog that needed a manual ESP32 reboot to drain (see [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H01); candidate fixes: report `backlog_remaining` against capacity so the Flipper can warn near-full, or size the partition against a longer target duration |
| BL13 | Heltec's flash partition was 96% full after porting `gps` (2026-09-23) | **DONE 2026-09-23** — resolved as part of the `wardriving` port: `heltec/partitions.csv` is now a custom table (`CONFIG_PARTITION_TABLE_CUSTOM`, matching `esp32/`'s mechanism) sized for this board's confirmed 8 MB flash — 2 MB `factory` app (51% free) + 2800 KB `wardrive` data partition + ~3.2 MB unallocated headroom. |
| BL14 | Heltec's wardriving-active LED indicator is a cosmetic judgment call, not a spec'd behavior: this board's LED is plain on/off (no color, unlike the C6's RGB), so the ported firmware blinks the CONNECTING state twice as fast while wardriving is active instead of the C6's blue→purple color swap | Open — flag for user to confirm or override; see `docs/PROJECT_HISTORY.md`'s 2026-09-23 wardriving-port entry |
| BL15 | OLIMEX MOD-ESP32-C5 has no onboard pushbutton, so this board has no hardware input for the BOOT-hold-5s factory-reset gesture the C6/Heltec both use | Open — explicit user decision 2026-09-25: defer rather than guess at a substitute mechanism (e.g. wiring an external button, or a BLE-triggered reset command); see `docs/hardware/olimex-mod-esp32-c5/README.md` |
| BL16 | MOD-ESP32-C5's Wi-Fi 6 (now dual-band, 2.4G+5G) + BLE 5 + 802.15.4 single-radio combo has never had a coexistence sweep run (same gap class as Heltec's BL13-adjacent skipped step 5) — `wifi_scan`/`ble_scan`/`wardriving` were all ported 2026-09-25 (wardriving being the first sustained concurrent-load capability on this radio) with no validated concurrent-scan/active-connection interval bounds for this specific chip | Open — flagged at port time, not discovered by live testing; do not assume the C6's step-4 bounds transfer without running this board's own sweep |
| BL17 | This board's two-LED status mapping (`docs/hardware/olimex-mod-esp32-c5/README.md`) reserves only two states (connection status on green, backlog-flushing on red) — unlike the C6/Heltec, `feb_status_led_set_wardriving_active()` is a deliberate no-op here (recorded, not applied) rather than rendering a third "wardriving capture active" visual state, since the two-LED design doc never specified one | Open — flag for user to confirm whether a third indication (e.g. a green blink-rate change, mirroring Heltec's BL14 judgment call) is wanted, or the no-op is fine as-is |
| BL18 | A single hardware session (2026-09-25) saw this board apparently reboot-loop at some point after a `wifi_scan` was triggered (recovered only by physical unplug/replug); a later, longer capture through pairing+GPS+disconnect+reconnect ran clean, but `wifi_scan` itself was never re-triggered during that clean run | Open — **not yet root-caused or confirmed reproducible**; might be `wifi_scan`-specific, might be an artifact of a since-reverted diagnostic `ESP_LOG_BUFFER_HEXDUMP` in the GPS task, or unrelated — needs a dedicated repro attempt (flash + repeated manual `wifi_scan` triggers) before being trusted as fixed or dismissed; do not assume BL16's coexistence gap is the cause without evidence |
| BL19 | Heltec's SX1276 LoRa radio (SPI-attached, physically separate chip) has never had a coexistence sweep run against this board's own Wi-Fi 4 + BT Classic/BLE 4.2 combo radio (same gap class as BL16's C5 item) — `meshcore_scan` was added 2026-09-26 as this board's first-ever user of the LoRa radio, running a continuous background receive task from boot alongside the existing wifi_scan/ble_scan/gps/wardriving capabilities, with no validated concurrent-radio-activity bounds for this specific pairing of chips | Open — flagged at port time, not discovered by live testing; passive-listen-only (no LoRa TX) is lower risk than a duty-cycled/bidirectional radio, per docs/LESSONS.md's coexistence writeups, but the gap is real and unmeasured, not just theoretical — do not assume the BLE connection surviving a scan in flight on this board extends to "surviving indefinitely with the LoRa RX task also running" without running this board's own sweep |
| BL20 | `meshcore_scan`'s `status` reply cannot list arbitrarily many nodes at once (`FEB_MESHCORE_MAX_NODES_PER_RESULT` = 3, no partial/complete streaming — see cbor_meshcore.h), so the Flipper screen (added 2026-09-26) only ever shows the top-3 nodes from the latest reply plus a "+N more known" count line, never a full scrollable list of every node the board has ever heard | Open — explicit user decision this session: ship "top-3 + count" now, not a scrollable list; real pagination (an offset/cursor argument on the `status` command, both firmwares) is a known future improvement once real-world MeshCore node density is observed, not built this pass |
| BL21 | `heltec/main/lora_shared_radio.cpp`'s RX task (renamed 2026-09-27 from `meshcore_radio.cpp` when `meshtastic_scan` was added to share the same physical radio) carries temporary `ESP_LOGI` diagnostic logging (IRQ-fired frame length + listen mode, raw hex + RSSI on successful `readData()`, `meshcore_proto_parse()`/`meshtastic_proto_parse()` success/failure, table-upsert node_id) added 2026-09-26/2026-09-27 to distinguish "no MeshCore/Meshtastic RF traffic in range" from "a bug in the RX/parse pipeline is swallowing frames" | Open — remove once real-world MeshCore and Meshtastic reception are each confirmed working (or confirmed absent) via field observation |
| BL22 | `heltec/main/lora_shared_radio.cpp`'s time-multiplexing between `meshcore_scan`'s and `meshtastic_scan`'s listen modes (added 2026-09-27, `LORA_SHARED_RADIO_DWELL_MS` = 60000) has no coexistence validation of its own, layered on top of BL19's existing unvalidated LoRa-vs-Wi-Fi/BT-combo-radio gap: (1) the 60-second-per-side dwell interval is an engineering guess with no measured MeshCore/Meshtastic real-world broadcast-interval data behind it — either protocol's broadcasts could be missed more often than this number implies; (2) switching sync words to change listen mode forces the SX1276 briefly into standby, an interruption to continuous receive with no measured effect on either protocol's own detection reliability; (3) BL19's original scope ("passive-listen-only is lower risk than bidirectional/duty-cycled traffic") still applies, but was written before a second LoRa protocol shared the same radio via active mode-switching, which is a real behavioral difference from a single fixed listen configuration | Open — flagged at port time, not discovered by live testing; needs a dedicated repro/measurement session (or real-world node density data for both protocols) before treating 60000ms as validated rather than a placeholder |
| BL23 | `meshtastic_scan`'s node table/name-length/max-nodes-per-reply constants (`heltec/main/meshtastic_table.h`'s `MESHTASTIC_TABLE_MAX_ENTRIES`=3, `meshtastic_proto.h`'s `MESHTASTIC_NAME_MAX_LEN`=8, `components/feb_protocol/cbor_meshtastic.h`'s `FEB_MESHTASTIC_MAX_NODES_PER_RESULT`=2) were all sized by this board's severe classic-ESP32 DRAM ceiling (only ~120 bytes of `.dram0.bss` headroom left after both mesh-scan capabilities' tables/scratch coexist — see `docs/SESSION_MEMORY.md`'s 2026-09-27 entry), not by a product judgment about how many Meshtastic nodes are actually useful to track at once | Open — flag for the user to confirm whether this tight a cap is acceptable long-term, or whether a future pass should reduce `meshcore_scan`'s own footprint (or find another DRAM saving) to give `meshtastic_scan` more room; do not silently raise any of these three constants without re-measuring the real `idf.py build` DRAM margin first (same "measured hard constraint" discipline as meshcore_scan's own sizing) |
| BL24 | `mesh_log` (added 2026-09-27) landed with **only 72 bytes of `.dram0.bss` headroom remaining** on the Heltec target (measured via a real `idf.py build`, not estimated) — getting a clean build at all required `FEB_MESH_LOG_MAX_RECORDS_PER_BATCH`=1 (a permanent wire-format cap, not just an ESP32-side choice — see `docs/PROTOCOL.md`'s `mesh_log` section), a 4-sector (16KB) flash partition (`heltec/partitions.csv`'s `meshlog`), narrowed `uint8_t`/`uint16_t` sector-bookkeeping fields | **Resolved 2026-09-27 (same day, two passes)** — a real hardware session (second physical unit, `heltec-a4cf1203b174`, COM10) first reconfirmed the build was still this tight (`idf.py size`: 80 bytes DRAM headroom, a small unreconciled drift from the 72 bytes originally reported) and captured the real `esp_get_free_heap_size()` number this row was waiting on: **121808 bytes free** (logged before Wi-Fi/BLE stack init, so an upper bound, not the true steady-state figure — NimBLE/esp_wifi were not yet up at that log point despite the code comment beside it claiming otherwise). Given that number, the dedup mechanism was switched from the flash-scan fallback to the frozen design's originally-preferred **heap-allocated table**: 128 entries (`ml_dedup_entry_t`, 17 bytes each, ~2.2 KB total) `malloc()`'d once in `mesh_log_init()`, never `static` — only a pointer + count + one-time-warned flag (8 bytes total) were added to `.bss`. Re-measured `idf.py size` after the switch: **72 bytes DRAM headroom** (down from 80, exactly the predicted 8-byte cost of those three new statics — the bulk table storage itself lives on the heap, outside this budget). Re-flashed and re-verified: clean ~60s boot capture, no crash/reset loop, `mesh_log_init()`/`lora_shared_radio_init()` both still succeed. **Eviction policy**: none — once the table fills, further new node_ids simply aren't deduped (a one-time warning logs this); no LRU, since mesh nodes are expected sparse (128 is a generous ceiling) and wdgwars.pl already tolerates duplicate uploads server-side. **Reboot caveat closed, same day (third pass), not just accepted**: `mesh_log_init()` now seeds `ml_dedup_entries` from the existing flash log once at boot — decoding every still-present record (drained or not, so a node logged once, drained, and later re-heard still won't re-append) and inserting each distinct `node_id` up to the same 128-entry capacity, before `ml_mutex` is created. This reuses the record walk that already existed there for `ml_undrained_in_sector`/oldest-cursor bookkeeping rather than adding a second pass over the same sectors — a one-time boot-time cost bounded by this partition's small size (4 sectors), no recurring cost, and no new `.bss`: re-measured `idf.py size` after this change still reports **72 bytes** DRAM headroom, unchanged. A new boot log line ("mesh log dedup table seeded with N entries from existing log") reports how many entries were seeded, for future hardware sessions to sanity-check. Reflashed and re-verified a third time: clean boot, no crash/reset loop, log correctly reported "seeded 0 entries" (this unit's `meshlog` partition is still empty — expected, not a bug, since no real MeshCore/Meshtastic sighting has ever been captured on this unit). Host-native tests (`tests/esp32/build.ps1`) reconfirmed passing, unaffected. Documented in `mesh_log.c`'s/`mesh_log.h`'s top comments and `docs/WARDRIVING_PUBLISH.md`'s "Dedup" bullet. Still open, unrelated to this dedup work: (1) the `-fstack-usage`/hardware high-water-mark check for `mesh_log_record_sighting()`'s stack-local CBOR-encode scratch buffer (still not run); (2) a true post-Wi-Fi/BLE-init `esp_get_free_heap_size()` reading, since 121808/119500 both remain pre-Wi-Fi/BLE upper bounds. See `docs/BASELINES.md`'s 2026-09-27 Heltec entries for the full boot-log narrative across all three passes. **Update 2026-09-28**: the table itself was shrunk further, from the 17-byte `ml_dedup_entry_t` (full node-id storage, ~2.2 KB) to a 4-byte FNV-1a hash per entry (128 × 4 B = **512 B** heap, same 128-node capacity), see `heltec/main/mesh_log.c`'s top comment for the collision-tradeoff rationale — heap-only change, `.dram0.bss` unaffected (still just the pointer/count/flag), build-verified, hardware-verification pending. |

| BL25 | Heltec's onboard SSD1306 OLED status display (`heltec/main/status_display.c`, added 2026-09-27, shows BLE connect/pairing/auth state, MeshCore/Meshtastic node summaries, and GPS time/speed when there's a fix) newly links `esp_driver_i2c` into this build for the first time, which overflowed `iram0_0_seg` by 664 bytes on the first attempt (the I2C master ISR handler is placed in IRAM by Kconfig default, `CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=y`, for cache-miss performance, not cache-disabled safety — `CONFIG_I2C_ISR_IRAM_SAFE`, the option that would actually require that, was and stays off). Fixed by setting `CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=n` in `heltec/sdkconfig.defaults` (this firmware's only I2C use is a best-effort, once-a-second, non-latency-critical OLED refresh from ordinary task context, so the cache-miss cost of leaving the ISR handler in flash is irrelevant) | **Fixed and hardware-verified 2026-09-27**: at the time, a clean `idf.py build` left near-zero headroom in both IRAM and DRAM — **see [BASELINES.md](BASELINES.md)'s Heltec headroom entry for the current figures** (this row is not the source of truth for that number and is not kept in sync with it). Any future capability/feature work on this board should re-check that entry before assuming how much room exists, and budget for a code-size or footprint reduction elsewhere before adding anything new if it's still tight. The screen was also found mounted 180° from the code's original assumption; fixed by swapping the SSD1306's segment-remap/COM-scan-direction init bytes (`0xA1`/`0xC8` → `0xA0`/`0xC0`), a controller-level flip needing no framebuffer changes. Flashed to the physical unit (COM10) and confirmed by the user: correct orientation, BLE state text, and node summaries all render correctly. |
| BL26 | `scripts/publish_wardriving.ps1` (extended 2026-09-27 to also publish `mesh_log`'s `mesh/mesh_nodes_current.txt` to wdgwars.pl, Method 2/HMAC, per `docs/WARDRIVING_PUBLISH.md`'s frozen design) writes its outcome as `mesh_`-prefixed keys (`mesh_status`, `mesh_imported`, `mesh_reject_<reason>`) into the same `wardriving_publish_result.txt` the CSV upload already uses, but `flipper/flipper_esp32_over_ble.c`'s `publish_parse_result()`/`draw_publish_screen()` (confirmed by reading both directly) only ever parse/display the unprefixed CSV-side keys — the frozen design's own "Publish screen shows both outcomes" line is not actually true yet | Open — not fixed in the same pass because `draw_publish_screen()`'s `AppScreenPublish` layout is already at capacity (4 fixed text rows, all used by the CSV-side imported/duplicates/captured/updated/no_gps/bad_rows numbers); adding a second outcome block needs a real layout decision (scroll, a second screen, or a compact combined line), not a squeeze-in — until fixed, a mesh upload's own success/failure is only visible via the result file's raw text or the host script's own console output, never on the Flipper's screen |
| BL27 | The GPIO2 touch-pad radio kill-switch (`heltec/main/radio_killswitch.c`, added 2026-09-27) landed with the tightest DRAM/`.data` headroom of this board's progression to that point (`idf.py size`, measured on a real `idf.py build`; see [BASELINES.md](BASELINES.md)'s Heltec headroom entry for the current figure rather than this row's now-superseded numbers, noted below). Getting a clean build at all required avoiding `driver/touch_pad.h`'s IIR-filter subsystem entirely (`touch_pad_filter_start()`/`touch_pad_read_filtered()`, which pulled in two `IRAM_ATTR`-placed functions this firmware would otherwise never reference, overflowing IRAM by 256 bytes on the first attempt) in favor of a plain synchronous `touch_pad_read()` in the poll loop, and running the touch-triggered Wi-Fi/BLE shutdown cleanup (cancel GAP discovery, terminate any live connection, stop a manual/wardriving scan in flight) directly on the touch task instead of handing it off to the NimBLE host task the way `wardriving_button_toggle_ev` does its equivalent work (an 8-byte raw `ble_npl_event` + a `SemaphoreHandle_t` for that handoff was the difference between a build that fit and one that didn't) | Open. **Update 2026-09-28 (HARDENING_PLAN.md HP-14/HP-12/HP-04/HP-05):** the Heltec now builds `-Os` with the stack canary, stack-overflow canary and task-WDT panic on, and headroom is much larger than this row's original numbers — see [BASELINES.md](BASELINES.md)'s Heltec headroom entry for the current figure; the ceiling this row originally described is superseded. Point (2)'s "only plain flags are raced" was wrong: the kill-switch path freed `wardriving_cluster_flush_records` under a host-task reader (fixed with a lock plus a deferred-free handoff). Kill-switch OFF→ON also never restarted Wi-Fi (fixed). Both fixes are build-verified; hardware pending. The original note follows (its specific byte counts are superseded by BASELINES.md, per the update above). — flag for the user: (1) before this update, this was the tightest DRAM margin of any board/pass to date — check [BASELINES.md](BASELINES.md)'s current figure before assuming how much room exists, and budget for footprint savings elsewhere before adding anything new if it's still tight; (2) the touch-task-direct cleanup above is an accepted, explicitly-flagged trade-off (a rare, operator-paced gesture racing this file's own plain bool/uint32 globals against the host task, not a NimBLE-thread-safety issue) — flashed to the second physical unit 2026-09-27 (COM10, MAC `a4:cf:12:03:b1:74`; build/flash both succeeded, esptool hash-verified all writes) but **boot health is still unconfirmed**: COM10 dropped off Windows' port enumeration right after the flash-triggered reset and never came back within a combined 160s of waiting, so no boot log was captured this pass — a future session needs to reconfirm the port (may need a manual USB reseat) and capture a clean boot log before trusting this build's runtime behavior, and the touch/toggle gesture itself still needs a human physically touching the pad; if a future session recovers enough DRAM margin to reintroduce the host-task handoff safely, that would close this gap in kind with wardriving's own toggle mechanism |

## Codebase & agent cost-efficiency

- **Extract a capability-dispatch layer** from both `main.c`/`flipper_esp32_over_ble.c` — pays
  off on every future capability. Hold until a natural roadmap boundary; decide the
  static-buffer-arena question first (see
  [LESSONS.md#static-buffer-pattern-trades-ram-for-stack-safety](LESSONS.md#static-buffer-pattern-trades-ram-for-stack-safety)).
- **Split monolithic sources** — `flipper/flipper_esp32_over_ble.c` (326 KB) by scene/UI,
  wardriving, capability dispatch, and settings; the three ESP `main.c` files likewise, taking
  the chance to pull still-duplicated code into `components/`. Scoped in
  [docs/TOOLING_PLAN.md](TOOLING_PLAN.md) TP-15 (its D3 decision backlogs this rather than
  starting it inside that plan). **Design draft: [SOURCE_SPLIT.md](SOURCE_SPLIT.md)
  (2026-09-29): implemented the same day and build-verified on all five targets; hardware
  regression passes pending (see its §10).**
  Binary and heap footprint ([HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H04) must not regress;
  measure `.text`/`.bss` before and after. Done-when (per TOOLING_PLAN.md): design note approved,
  no source file >60 KB, all five targets build, FAP section sizes within noise.
- Still open: a **runtime active/passive BLE scanning toggle for manual `ble_scan`** (wardriving
  already has one, the `ble_passive` source; a manual scan is always active — a prerequisite for [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s "Scan" menu's
  two passive modes), and measuring the real-world name-discovery improvement once
  hardware-tested. (Active scanning itself is already enabled — see BACKLOG_COMPLETED.md.)
- Do **not** split `flipper/pairing_crypto.c` (kept diffable against upstream curve25519-donna
  for auditability) or `tests/vectors/vectors.h` (98KB, generated — never `Read` it whole).
- `tests/flipper/build.ps1` fails out-of-the-box on a machine where Visual Studio's
  `vcvars64.bat` shells out to `vswhere.exe` by bare name and the VS Installer directory isn't
  already on `PATH` (surfaced 2026-09-12 while verifying the LED-indicator feature). Needs a
  one-line `PATH` prepend in that script; not yet fixed.

## Other open items (not covered by the cross-model review)

- **BL08 — Home screen space optimization:** consolidate display by showing "Pairing:Y/N" instead of "saved pairing" and "ESP:waiting"/"ESP:session" instead of "Waiting for ESP"; move all current status data to settings screen in a scrollable view for full visibility.
- **DONE 2026-09-26 (esp32c5/ + flipper/ only, see BACKLOG_COMPLETED.md):** Promote implicit
  cross-firmware constants into the shared contract — the Flipper's `PAYLOAD_MAX` and the
  ESP32-C5's `FEB_FLIPPER_WRITE_CHAR_MAX_LEN` now both reference a new `FEB_WRITE_CHAR_MAX_LEN`
  in `framing.h` instead of independently hardcoding the same literal. **Not yet done for
  `esp32/` or `heltec/`**, which still hardcode their own `64u` copy — reopen or file a fresh
  row if those get ported too.
- Step 9 must exercise the real negotiated ATT MTU, not only step 3's forced-small fragments —
  the oversized-write path has never been tested this way (how a later real ATT-length bug
  stayed latent through step 3).
- **Build-time stack-budget check** (`-fstack-usage`/`-Wstack-usage=N` in the FAP build).
  Highest-value tooling gap in this list: the same `BleEventWorker`-stack-overflow bug class has
  been found by crashing real hardware four times now (steps 3, 5, 7, wifi_scan).
- **Single cross-implementation CBOR/framing vector-runner tool** — the structural fix for the
  bug class behind the six pre-step-7 convergence findings
  ([archive/CODE_REVIEW_FIX_PLAN.md](archive/CODE_REVIEW_FIX_PLAN.md)'s explicit non-goal). Still not built;
  highest-value missing piece of test tooling.
- Real scrollable capability-list screen on the Flipper, once `features` grows past what the
  single status screen can show.
- Host-test coverage for `capability_query`/`capability_response` on the Flipper side —
  currently zero.
- Manual "disconnect current board" Flipper UI action, to free the BLE connection slot without
  powering a board off (step 7 scope).
- **No unpair UI action exists at all yet** (a broader gap than the disconnect item above) —
  confirmed a real practical cost 2026-09-12: after adding the `gps` capability to the ESP32,
  an already-paired board's Flipper-side capability cache (`docs/CAPABILITIES.md`'s "queried
  exactly once, never auto-refreshed" rule) kept it permanently invisible until the cached
  `capabilities/<board_id>.dat` file was deleted by hand via the Unleashed checkout's
  `scripts/storage.py -p COM8 remove ...` over the Flipper's CLI port — the only available
  workaround today. Any future capability added to an already-paired board will hit the exact
  same silent staleness until a real unpair action exists in the app.
- Automatic BLE arbitration between multiple paired boards — gated on an unresolved BLE-HAL
  question: can the Flipper's peripheral role advertise while already connected?
- Follow-on items deliberately left backlogged from the real-GPS-driver design (implemented and
  hardware-verified 2026-09-13 — see [PLAN.md](PLAN.md)'s "Real GPS driver..." section):
  - GPS backfill-to-first-fix (buffer and retroactively backfill pre-fix records instead of
    discarding them) — considered as an alternative to the chosen continuous-discard behavior,
    not built.
  - A user-configurable fix-quality/HDOP acceptance threshold, as a board setting (the frozen
    design uses "any non-zero fix quality," no threshold).
  - Research into improving on-board GPS accuracy (antenna choice, SBAS/WAAS config, update
    rate, etc.) — raised during the design session, not investigated yet.
  - Real speed on the GPS screen: **done 2026-09-21**, see
    [docs/WARDRIVING_REDESIGN.md](WARDRIVING_REDESIGN.md) (`speed_e1_kmh`, parsed from `RMC`'s
    speed-over-ground field). Real heading, from `RMC`'s course field, remains open — not part
    of that pass.
  - `wardriving_csv.c`'s WigleWifi-1.4 `AltitudeMeters`/`AccuracyMeters` columns are still
    hardcoded `"0,0"` — GGA `altitude_dm` exists in `feb_location_t` and the `gps` capability's
    own `altitude_dm_offset` result field, but wardriving records carry no altitude field of
    their own yet, so the CSV exporter has nothing to read; `AccuracyMeters` has no real source
    at all (the GPS module reports HDOP, not a meters-based error estimate). Not folded into the
    GPS-capability addition since it requires a wardriving-record wire-format change on both
    firmwares.
  - **Per-board wardriving autostart setting** — add persistent board configuration so
    wardriving can start autonomously after ESP32 boot, independent of the Flipper initiating
    the session; define the Flipper settings UI and get/set wire surface, boot-time interaction
    with GPS fix availability, and how an active capture is stopped or disabled. Unscoped.
  - **Persistent GPS GNSS configuration setting** — expose GPS constellation selection (GPS,
    GLONASS, BeiDou) and related receiver configuration through board settings; define the
    Flipper settings UI, authenticated get/set wire surface, ESP32 persistence, and safe
    one-time UBX provisioning without sending configuration commands on every boot. Unscoped.
  - Runtime-configurable GPS UART GPIO pins via a Flipper Settings screen — deliberately split
    out of the frozen design (see PLAN.md's "Scope boundary" note) because it needs a
    form/pin-entry widget this project doesn't have yet and a new get/set wire config surface.
    Defaults stay compile-time constants for now.
  - Per-channel Wi-Fi scan dwell time ("WiFi Swelling") configurable from the Flipper UI: **done
    2026-09-21**, see [docs/WARDRIVING_REDESIGN.md](WARDRIVING_REDESIGN.md) — landed on the new
    Wardriving Stopped screen's settings list rather than a dedicated Settings screen (which
    didn't exist as more than a placeholder at the time, and was removed entirely 2026-09-27 —
    see [docs/UI_REDESIGN.md](UI_REDESIGN.md)),
    with the regulatory country-code item below implemented alongside it as that doc's own
    scoping decision required. Scoped to wardriving's own WiFi scan calls only, not the manual
    `wifi_scan` capability — see that doc's "Scope boundaries."
- Non-ASCII SSID rendering is untested on real hardware (host-native codec tests cover the
  encoding; no such network was available during `wifi_scan` verification). Not a blocker.
- Adopt a real `ViewDispatcher`/scene-manager architecture on the Flipper FAP instead of the
  single-`ViewPort`/`AppEvent`-queue pattern every screen has been bolted onto. Structural,
  no deadline, not a blocker for anything currently in flight (the Phase 3a menu redesign
  shipped directly on the existing pattern instead — see [docs/UI_REDESIGN.md](UI_REDESIGN.md)).
- The Flipper's "Scan" menu screen is only a placeholder-level Wi-Fi-scan/BLE-scan picker, not
  [docs/UI_REDESIGN.md](UI_REDESIGN.md)'s actual five-mode BLE-active/passive live-view design
  (reusing Wardriving's capture engine without persistence). Needs its own implementation pass
  once the runtime BLE active/passive toggle above exists.
- **ESP32-side `wifi_swelling`/`country` (Phase 7, [docs/WARDRIVING_REDESIGN.md](WARDRIVING_REDESIGN.md)) are not persisted across the button-toggle or boot-autostart wardriving-start paths** — only a Flipper-sent `start` command carries them; autostart/button-toggle always run at Normal/RoW. See that doc's "Open items" for full detail. **Implemented 2026-09-29, build-verified; hardware pending.** `wardriving_persist.h` is now version 2 and stores `wifi_swelling`/`country`/`wifi_band`. A v1 blob is migrated, not discarded, so an existing autostart survives the upgrade. The C6, C5 and Heltec autostart paths and the C6/Heltec boot-button paths reuse the saved values. **Decision for the user:** the C5 autostart still forces 2.4 GHz (a deliberate safety default until BL16's coexistence sweep), even though the band is saved. Hardware: start with Aggressive/BG, reboot, and confirm autostart logs and uses them.
- **Heltec: a wardriving run active before a GPIO2 kill-switch OFF is not resumed on the next ON** (found 2026-09-29 while fixing the item above). The ON branch re-inits Wi-Fi/BLE, but `host_synced()`'s autostart block is gated by `wardriving_autostart_attempted`, already true after first boot. Pre-existing; not fixed. Needs a decision on whether a kill-switch cycle should resume capture.
- **Cosmetic, needs a hardware/visual check:** the Home menu's "Connection lost" banner and each
  non-Home screen's own title may visually overlap — both are drawn at nearly the same canvas
  position (banner at y=12 `FontSecondary`, titles at y=11 `FontPrimary`). Found while reading
  `draw_callback`; not confirmed on a real screen.
- **Consolidating/grouping wardriving records on the Flipper side** (e.g. de-duplicating or
  rolling up repeated/nearby sightings for display, as distinct from the ESP32-side capture-time
  dedup that already exists). Not scoped yet — needs its own planning/grill-me session before
  implementation, not a drive-by design call.
- **Reconsider the RSSI-improve dedup gate's comparison basis: last-written vs. best-ever.**
  Both `components/feb_wardriving/wardriving_dedup.c`'s `should_log_record()` and
  `flipper/wardriving_csv.c`'s `feb_wardriving_dedup_should_write()` re-trigger a write when RSSI
  improves ≥6dB versus the *last-written* observation for that address — which lets a signal that
  is merely fluctuating (not trending stronger), especially BLE's noisier RSSI, repeatedly clear
  the gate. Investigated against WiGLE's own reference Android app
  (`wigle-wifi-wardriving`, `DatabaseHelper.java`'s `addObservation()`): its gate is a hybrid — a
  64-entry in-memory LRU cache makes it compare against last-written for addresses still warm in
  cache (same behavior as this project), falling back to the DB's true best-ever
  `network.bestlevel` column only on a cache miss. Worth deciding whether to switch to (or
  approximate) a best-ever comparison here. Related to the "Consolidating/grouping" item above
  but narrower. Not scoped/decided yet.
- **Explore throttling the Wi-Fi source specifically during wardriving results-flush to the
  Flipper** — the flush was observed getting stuck before the recent dedup/sequence-cap fixes
  (`e616d81`, `3111fa2`). Possibly the same BLE/Wi-Fi coexistence starvation as G36, just
  triggered by the drain instead of by a reconnect; not yet isolated whether it still reproduces
  post-fix. Related to WiFi-source duty cycle optimization below, but narrower — only during the
  flush window, not a general default change.
- **Optimize WiFi scan interval beyond 5 seconds** (2026-09-13) — current validated production
  default is `wifi_interval_ms=5000` (5 seconds, ~12 scans/minute), set after research confirmed
  ESP32 WiFi scans take ~1.4-2 seconds per full 2.4 GHz channel sweep (per ESP-IDF WiFi driver
  documentation). Further optimization to 2-3 second intervals (`wifi_interval_ms=2000` or
  `wifi_interval_ms=3000`) is candidate for future testing to improve wardriving capture density
  while maintaining BLE stability. **Research baseline:** Full 2.4 GHz WiFi scan baseline is
  ~2040ms; optimized channel timing (85ms active for channels 1-11, 255ms passive for 12-13)
  reduces this to ~1445ms, achieving 0.69 Hz scan frequency (see ESP-IDF WiFi driver docs and
  WiFi performance analysis). **Test plan:** Run extended wardriving sessions at 2s, 3s, and 5s
  intervals; measure: (1) BLE reconnect latency if connection drops, (2) backlog drain reliability,
  (3) WiFi capture density (networks/minute), (4) subjective coverage quality. Document tradeoffs
  and settle on production default accordingly.
- Set an explicit Wi-Fi regulatory country code instead of relying on ESP-IDF's implicit
  default: **done 2026-09-21**, see [docs/WARDRIVING_REDESIGN.md](WARDRIVING_REDESIGN.md) — a
  `BG`/`RoW` toggle on the new Wardriving Stopped screen calls `esp_wifi_set_country_code()`
  once at wardriving start (`"BG"` unlocks channels 12-13, active-only; `"RoW"` keeps today's
  implicit `"01"` world-safe-mode default, channels 1-11). Applied once per wardriving start,
  not at boot — a later manual `wifi_scan` observes whatever `country` wardriving last set,
  since it's a global radio setting (documented, not solved, in that doc's "Scope boundaries").

## Deferred by explicit product decision — confirm with the user before touching

- **Idle-connection heartbeat/keep-alive redesign.** The 30s idle-timeout disconnect/reconnect
  cycle works but causes a cosmetic LED/screen flicker. This is a wire-protocol change (both
  firmwares) needing its own design session — backlogged at the user's explicit request, not a
  quick patch.
- **G07** (TX single-flight clobbers wardriving drain) — deferral lifted 2026-09-14; a bounded
  ESP32 protected-TX FIFO now serializes backlog, handshake, capability, status, and error
  records. Hardware reconnect/wardriving retest remains before closing H03.

## Accepted, not a bug — do not "fix"

- Pairing X25519 is unauthenticated by design ([DECISIONS.md](DECISIONS.md)).
- The Flipper cannot be a BLE central/GATT client (standalone FAP ABI constraint).
- The 30-second idle disconnect is specified behavior; only a heartbeat *redesign* (above) is
  backlogged, not the current mechanism itself.
- `capability_query`'s `requested` field is intentionally unimplemented (full registry only).
- GPS is real UART/NMEA driver, hardware-verified 2026-09-13 — see PLAN.md's "Real GPS driver, wardriving fix-dependency, and real wardriving-record timestamps". Discarding captures made without a real fix is specified behavior.
- Old on-flash wardriving records failing to decode (and being silently skipped) once the new
  mandatory `utc_timestamp_s` field ships is an accepted one-time cost of that format upgrade, not
  a bug — the decode-failure path handles it safely, and the circular log self-heals as it rotates (see commit b23aec0: stale record flags are now cleared on boot/replay instead of appearing as stuck backlog). Accepted by the user 2026-09-12; see PLAN.md's GPS section.
- `pairing_crypto.c`'s X25519 ladder is not constant-time (`mbedtls_mpi_mod_mpi()`). Accepted for
  the current threat model — physical possession of either device is already fully compromising.
- Step 4's radio-coexistence sweep is not trustworthy evidence for a wardriving duty-cycle
  default (it used synthetic load and missed the real starvation bug found later) — don't cite it
  to justify reverting `ble_interval_ms`/`ble_window_ms`'s current defaults.
- Do not flash, erase, or write either physical board while acting on anything in this file
  unless the user explicitly asks.

## Roadmap-gated (belongs to a specific PLAN.md step — do not steal into an unrelated PR)

- **Step 8:** ESP32 NVS pairing-blob version/validity-marker/atomicity (G13, finding #17);
  Flipper-side pairing/capability persistence hardening; explicit local unpair/factory-reset
  behavior definition.
- **Step 9:** full negative-security-test suite; real-negotiated-MTU exercise (see "Other open
  items" above); re-confirming step 4's coexistence bounds under live authenticated wardriving
  traffic (partially done — see SESSION_MEMORY.md's current-state section for what's left).

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

**`meshcore_scan` capability (Heltec-only, Phase 1 "detection + display") added 2026-09-26,
build-verified only, no hardware to test against.** Passively listens for MeshCore (a
third-party open LoRa mesh-network protocol, unrelated to this project's own BLE protocol)
`ADVERT` node-broadcast packets via the Heltec's previously-unused onboard SX1276, using the
`jgromes/radiolib` managed component. `feb_features[]` is now `{"wifi_scan", "ble_scan",
"gps", "wardriving", "meshcore_scan"}`. New files: `heltec/main/meshcore_proto.c/.h` (pure-C
packet parser, host-tested against hand-built synthetic ADVERT frames — no real MeshCore
node exists to test against — `tests/esp32/test_meshcore_proto.c`), `meshcore_table.c/.h`
(12-entry in-RAM node table, RAM-only, cleared on reboot), `meshcore_radio.cpp/.h` (the one
C++ file in this firmware, since RadioLib has no C API) plus a vendored `meshcore_esp_hal.c/
.h`-equivalent (`meshcore_esp_hal.cpp/.h`) HAL adapter — the pinned `jgromes/radiolib`
7.7.1 release turned out not to ship an ESP-IDF HAL at all (only added to RadioLib's
unreleased master branch after that tag), so this project vendors RadioLib's own
MIT-licensed adapter directly. A real, measured hard constraint (not a guess) shaped this:
the original design's 64-entry table overflowed this classic-ESP32 board's DRAM by 4536
bytes on a real `idf.py build`; fixed via `heltec/CMakeLists.txt` excluding every RadioLib
modem family/protocol client this capability doesn't use (`RADIOLIB_EXCLUDE_*`) and shrinking
the table to 12 tightly-packed entries — `docs/PLAN.md`'s design plan's original "64" sizing
guess is superseded by this measured ceiling. New shared codec `components/feb_protocol/
cbor_meshcore.c/.h` (host-tested, `tests/esp32/test_framing_cbor.c`) — **not yet mirrored
into `flipper/`**, left for a follow-up agent (frozen field layout: see this session's
handback report or `docs/PROTOCOL.md`'s new `meshcore_scan` section). `esp32`/`esp32c5`
builds and all host-native test suites reconfirmed unaffected. **Not done this pass**: no
flashing, no hardware verification of any kind (no MeshCore node available), no
radio-coexistence sweep against this board's Wi-Fi/BT combo radio (new `docs/BACKLOG.md`
BL19). **Capability-cache gotcha applies again**: this board's Flipper-side cached capability
record was already populated (from steps 7/8/9) before `meshcore_scan` existed in
`feb_features[]` — it must be deleted (same procedure as before) before the next
`capability_query` will see the new feature, once the Flipper side is implemented.

**Phase 9 (wired cluster), Heltec+C6 pair build-verified and flashed 2026-09-26, pairing/
hardware round-trip still pending.** C6 + C5 + Heltec wired together over UART to eliminate
radio coexistence by giving each board exactly one scanning job. Full design:
[docs/CLUSTER.md](CLUSTER.md); step tracking: `docs/PLAN.md`'s "Phase 9" section. Shared
`components/feb_cluster_link/` framing component (host-tested, 25/25), `esp32/cluster_worker/`
(new, C6 2.4GHz-only worker), and `heltec/main/main.c` (additively updated — `wifi_scan` proxies
to a present worker, falls back to local scan otherwise) are all flashed to physical boards and
booting clean. **Real finding**: Phase 9 bring-up is on a *second* physical Heltec unit (MAC
`a4:cf:12:03:b1:74`, confirmed with the user) than the Phase 4 board (`a4:cf:12:03:ba:58`) — this
one has no stored pairing_secret yet. A fresh Flipper pairing against this specific unit is
needed before a real end-to-end `wifi_scan` proxy test is possible; see
`docs/hardware/heltec-wifi-lora-32-v2/README.md`'s new note.

**`meshtastic_scan` capability (Heltec-only, Phase 1 "detection + display") added
2026-09-27, build-verified only, no hardware to test against.** Passively detects nearby
Meshtastic (a different, unrelated open LoRa mesh firmware/protocol from MeshCore) nodes over
the same onboard SX1276 `meshcore_scan` already uses. `feb_features[]` is now `{"wifi_scan",
"ble_scan", "gps", "wardriving", "meshcore_scan", "meshtastic_scan"}`. **Radio-sharing decision
(the one open design question this task raised):** MeshCore's own fixed EU-868 preset and
Meshtastic's EU_868 "LongFast" default preset turned out (this session's own research into
Meshtastic's public radio-settings docs) to use *identical* RF modem parameters (869.525 MHz,
250 kHz BW, SF11, CR4/5) — the only per-protocol radio setting that differs is the SX1276's
one-byte sync-word register (MeshCore: RadioLib's default 0x12; Meshtastic: 0x2B, confirmed
against meshtastic/firmware's own RadioInterface config). This made time-multiplexing (option
1 of the three the task offered) clearly the best fit: switching listen mode is a cheap
`setSyncWord()` + `startReceive()` call, not a full re-tune, so no new wire-protocol
mode-select command (option 2) was needed for either capability. The old `meshcore_radio.cpp/
.h` (sole owner of the physical SX1276) was renamed to `lora_shared_radio.cpp/.h` and extended
to alternate listen modes on a fixed, **unvalidated** 60-second-per-side interval
(`LORA_SHARED_RADIO_DWELL_MS`) — an engineering guess, since neither protocol's real-world
broadcast interval has been observed by this project (new `docs/BACKLOG.md` BL22). New files:
`heltec/main/meshtastic_proto.h/.c` (pure-C header decode + a researched, **unverified-against-
real-hardware** AES-128-CTR decrypt of Meshtastic's public default "LongFast" channel's
NODEINFO_APP `short_name` field, using mbedTLS's AES module — the one board-local protocol
parser in this project with an mbedtls dependency, host-tested via a dedicated
`build_meshtastic_proto.ps1` that also links mbedtls's `aes.c`), `meshtastic_table.c/.h`
(3-entry in-RAM node table, RAM-only, cleared on reboot), new shared codec
`components/feb_protocol/cbor_meshtastic.c/.h` (host-tested, `tests/esp32/test_framing_cbor.c`
+ new vectors in `tests/vectors/generate_vectors.py`) — **not yet mirrored into `flipper/`**,
same follow-up-agent deferral as `cbor_meshcore.h` got. Presence detection (`node_id`, RSSI,
last-seen) works for *any* heard Meshtastic packet regardless of channel, since Meshtastic's
raw 16-byte packet header is always sent in cleartext by design; only the optional `name`
field requires a successful default-channel decrypt. **A real, measured hard DRAM constraint**
(same class as meshcore_scan's own 64-to-12-entry cut, but tighter): this board's classic-ESP32
DRAM was already mostly consumed by `meshcore_scan`'s own tables/scratch, so fitting a clean
`idf.py build` required (1) sharing one 512-byte CBOR result-encode scratch buffer between
`handle_meshcore_command()`/`handle_meshtastic_command()` instead of each having its own
(`main.c`'s new `lora_capability_result_buf`), (2) decoding Meshtastic's compact `short_name`
field instead of the longer `long_name`, and (3) cutting `meshtastic_table.h`'s
`MESHTASTIC_TABLE_MAX_ENTRIES` to 3, `meshtastic_proto.h`'s `MESHTASTIC_NAME_MAX_LEN` to 8, and
`cbor_meshtastic.h`'s `FEB_MESHTASTIC_MAX_NODES_PER_RESULT` to 2 — all well below
`meshcore_scan`'s own equivalents (12/24/3). The final build leaves only **~120 bytes of
`.dram0.bss` headroom** (measured via `idf.py size` on the real `heltec/` target, not a guess)
— new `docs/BACKLOG.md` BL23 flags this as a hard ceiling any future capability work on this
board needs to plan around. `esp32`/`esp32c5` builds and all host-native test suites
reconfirmed unaffected (`tests/esp32/build.ps1`, `build_pairing.ps1`, `build_session.ps1`,
`build_location.ps1`, `build_wardriving.ps1`, `build_cluster_link.ps1`,
`build_meshcore_proto.ps1`, new `build_meshtastic_proto.ps1` — all pass); `check_shared_
headers.py` clean. **Not done this pass**: no flashing, no hardware verification of any kind
(no Meshtastic node available), no radio-coexistence sweep against this board's Wi-Fi/BT combo
radio or against `meshcore_scan`'s own listen windows (BL22). **Capability-cache gotcha applies
again**: this board's Flipper-side cached capability record was already populated before
`meshtastic_scan` existed in `feb_features[]` — it must be deleted (same procedure as before)
before the next `capability_query` will see the new feature, once the Flipper side implements
this capability.

**`mesh_log` capability (Heltec-only, ESP32 side) added 2026-09-27, build/host-test-verified
only, no hardware to test against.** The missing capture/accumulate/drain layer feeding
wdgwars.pl's mesh-node upload (`docs/WARDRIVING_PUBLISH.md`'s "Mesh node publishing", design
frozen the same day) — hooks into `meshcore_table_upsert()`'s/`meshtastic_table_upsert()`'s own
call sites in `lora_shared_radio.cpp`'s RX task, recording any sighting that carries a position
to a new dedicated checksummed circular flash log (`heltec/main/mesh_log.c/.h`,
`mesh_log_record_format.c/.h`, mirroring `wardriving_log.c`'s architecture), independent of
`wardriving`'s start/stop lifecycle. `feb_features[]` is now `{"wifi_scan", "ble_scan", "gps",
"wardriving", "meshcore_scan", "meshtastic_scan", "mesh_log"}`. **Confirmed by reading
`meshtastic_proto.h` directly this session (not assumed): Meshtastic never contributes a
sighting here today** — its Phase 1 parser doesn't decode `POSITION_APP` at all, so every
recorded sighting is currently MeshCore-only; this is a real, current gap, flagged explicitly
per the task's own request, not silently papered over.

**Dedup decision (superseded same day, see below):** the frozen design's preferred
heap-allocated table (sized against a real `esp_get_free_heap_size()` boot-time reading) could
not initially be built — **no physical Heltec board was available this session**, and free
heap is a runtime quantity that `idf.py size`/`idf.py build` cannot report the way it can for
static `.dram0.bss` (BL23's own measurement stayed valid; this one simply couldn't be taken at
all). `mesh_log.c` therefore first used the frozen design's own documented fallback: a flash
scan of the log itself before every append, no RAM table. `main.c`'s `app_main()` logged
`esp_get_free_heap_size()` right after `mesh_log_init()` so a future hardware session could read
the real number and reconsider a heap-table upgrade.

**Dedup switched to the heap-table approach, same day (2026-09-27), once real hardware was
connected.** A hardware-verification pass (second physical unit, `heltec-a4cf1203b174`, COM10)
measured `esp_get_free_heap_size()` at **121808 bytes free** (logged before Wi-Fi/BLE stack
init, so an upper bound rather than the true steady-state figure) and reconfirmed `idf.py size`
still showed a razor-thin 80 bytes of `.dram0.bss` headroom. Given that free-heap number, the
dedup mechanism was switched from the flash-scan fallback to a **heap-allocated 128-entry
table** (`ml_dedup_entry_t`, 17 bytes each, ~2.2 KB total, `malloc()`'d once in
`mesh_log_init()`, never `static`) — the frozen design's originally-preferred option. Only a
pointer + count + one-time-warned-flag (8 bytes) were added to `.bss`; `idf.py size`
re-confirmed **72 bytes DRAM headroom** after the switch (down from 80, exactly the predicted
8-byte cost). **Eviction policy**: none — once full, further new node_ids simply aren't
deduped (a one-time warning logs this), accepted given mesh nodes are expected sparse and
wdgwars.pl already tolerates duplicate uploads server-side. `ml_scan_contains_node_id()` (the old
flash-scan function) was deleted as dead code. Re-flashed and re-verified: clean ~60s boot
capture, no crash/reset loop, `mesh_log_init()`/`lora_shared_radio_init()` both still succeed
(same COM10 unit). Host-native test suite (`tests/esp32/build.ps1`) reconfirmed passing
unaffected (no shared-codec files touched — this was ESP32-side-only logic).

**Reboot caveat closed same day (third pass), not just accepted as a trade-off.** The heap
table above started empty every boot, reopening the persistence property the flash-scan
fallback it replaced had incidentally provided — the user asked for this actually closed, not
documented as accepted. Fix: `mesh_log_init()` now seeds `ml_dedup_entries` from the existing
flash log once at boot, decoding every still-present record (drained or not — a node logged
once, drained, and later re-heard must still not re-append) and inserting each distinct
`node_id`, reusing the same record-walking loop that already existed there for
`ml_undrained_in_sector`/oldest-cursor bookkeeping rather than adding a second pass over the
same sectors. This is a one-time boot-time cost (bounded by the partition's small 4-sector
size), not recurring, and adds no new `.bss`: `idf.py size` reported the same **72 bytes** DRAM
headroom before and after. A new boot log line ("mesh log dedup table seeded with N entries
from existing log") reports the seeded count. Reflashed and re-verified a third time: clean
boot, no crash/reset loop, log correctly showed "seeded 0 entries" (this unit's `meshlog`
partition is still empty — expected, not a bug, since no real MeshCore/Meshtastic sighting has
ever been captured on it). Host-native tests reconfirmed passing again. Net effect: this heap
table now has both the fast runtime lookup the original switch to heap storage was for, and the
flash log's own cross-reboot persistence the flash-scan fallback had — not a trade-off between
the two. Full detail: `docs/BACKLOG.md` BL24, `docs/WARDRIVING_PUBLISH.md`'s "Dedup" bullet,
`heltec/main/mesh_log.c`'s top comment.

**A second real, measured hard DRAM constraint, tighter than either mesh-scan capability's
own** (docs/BACKLOG.md BL23's ~120-byte headroom had nothing left for this capability at all):
a straightforward first implementation overflowed `idf.py build`'s `.dram0.bss` region by 360
bytes. Clearing it required, in order of impact: (1) capping the wire format itself at
**exactly one `<mesh-log-record>` per `status` reply** (`FEB_MESH_LOG_MAX_RECORDS_PER_BATCH` =
1, `components/feb_protocol/cbor_mesh_log.h` — a permanent constraint on both firmwares, not
just this build's sending choice, since `wardriving`'s own dynamic multi-record batching would
have needed a batch-sized scratch buffer this board cannot spare); (2) narrowing every sector/
offset bookkeeping field to `uint8_t`/`uint16_t` and removing several fields entirely in favor
of recomputing them on demand (a summed pending-count instead of a stored running total; a
fixed-at-compile-time sector count instead of a discovered-and-stored one; a per-record-decode
instead of a cached "next position" pointer); (3) a 4-sector (16KB) `meshlog` flash partition
(`heltec/partitions.csv`) rather than `wardriving_log.c`'s own headroom-above-actual sizing
convention; and (4) moving `mesh_log_record_sighting()`'s own CBOR encode/decode scratch buffer
(the single largest remaining item, ~83 bytes) onto the LoRa RX task's stack instead of
`.bss` — a deliberate, explicitly-documented exception to this codebase's usual "non-trivial
buffers on a BLE/radio-callback path default to `static`" convention, justified by that task's
4096-byte stack budget having ample room and no real `-fstack-usage`/hardware check being
possible this session (flagged, not silently assumed safe). The final build leaves **72 bytes**
of `.dram0.bss` headroom (measured via a real `idf.py build`, matching BL23's own discipline) —
new `docs/BACKLOG.md` BL24 flags this as an even tighter ceiling than BL23's own for any future
work on this board. New shared codec `components/feb_protocol/cbor_mesh_log.c/.h` (host-tested,
`tests/esp32/test_framing_cbor.c` + new vectors in `tests/vectors/generate_vectors.py`) — **not
yet mirrored into `flipper/`**, same follow-up-agent deferral as `cbor_meshtastic.h` got.

**Hardware verification pass 2026-09-27 (second physical unit, `heltec-a4cf1203b174`, COM10):**
flashed the current `mesh_log`/`meshtastic_scan` build and captured ~90s of boot log — clean
boot, no crash/reset loop, `mesh_log_init()`/`lora_shared_radio_init()`/wardriving_log/NimBLE
scan all started successfully, the 60s MeshCore/Meshtastic listen-mode multiplexer fired on
schedule, no MeshCore/Meshtastic sighting logged (expected, no real node in range). **The real
`esp_get_free_heap_size()` number BL24 was waiting on: 121808 bytes free** — but that log line
runs *before* Wi-Fi/BLE stack init (a discrepancy from its own code comment), so it's an upper
bound, not the true steady-state figure. `idf.py size` reconfirmed the DRAM ceiling is still
razor-thin (80 bytes free, vs. 72 previously reported — small unexplained drift). Also found:
this unit's `load_pairing_secret()` now returns true, contradicting the 2026-09-26 hardware doc
note that it had none — not chased further (no Flipper connected this session). Full detail:
`docs/BASELINES.md`'s 2026-09-27 Heltec entry, `docs/BACKLOG.md` BL24,
`docs/hardware/heltec-wifi-lora-32-v2/README.md`'s "Second physical unit" section. No code
changes made this pass — measurement only, per explicit instruction.

**A real wire-shape gap this design hadn't spelled out, found by reading the Flipper's actual
routing code (`flipper/flipper_esp32_over_ble.c`), not assumed:** the Flipper dispatches an
inbound `status` record purely by peeking its `state` text — there is no `capability`
discriminator field on `status` at all — so every capability's `state` value(s) must be unique
across the whole protocol, not merely within that capability. `wardriving` already owns
`"data"`; `mesh_log` uses `"mesh_data"` instead. Corrected inline in
`docs/WARDRIVING_PUBLISH.md` and specified in `docs/PROTOCOL.md`'s new `mesh_log` section.

`esp32`/`esp32c5` builds and all host-native test suites reconfirmed unaffected
(`tests/esp32/build.ps1`, `build_pairing.ps1`, `build_session.ps1`, `build_location.ps1`,
`build_wardriving.ps1`, `build_cluster_link.ps1`, `build_meshcore_proto.ps1`,
`build_meshtastic_proto.ps1` — all pass); `tools/check_shared_headers.py` clean (unaffected,
since `cbor_mesh_log.h` isn't mirrored into `flipper/` yet, same as `cbor_meshtastic.h`).
**Not done this pass**: no flashing, no hardware verification of any kind (no board available);
no manual on-demand query exists for this capability by design (it is push-only, mirroring the
frozen design's own framing — see `docs/PROTOCOL.md`); the Flipper-side accumulator
(`mesh/mesh_nodes_current.txt`, per the frozen design) is a follow-up task, not started here.

**Phase 8 (OLIMEX MOD-ESP32-C5 board support) started 2026-09-25.** Third ESP32-family target,
`esp32c5/`, same gate-override pattern as Phase 4/6/7. **Step 1 (board bring-up) is done and
hardware-verified 2026-09-25:** chip confirmed as ESP32-C5 rev v1.0 (dual-band Wi-Fi 6 + BLE 5 +
802.15.4, 8 MB flash, MAC `d0:cf:13:ff:fe:e0:88:40`, native USB) via read-only `esptool` on
COM11; `esp32c5/` scaffolded mirroring `heltec/`'s layout, wired into the shared
`components/feb_protocol/` component; unmodified baseline built, flashed, and confirmed booting
cleanly over serial. **Explicit scope limits for this phase:** 2.4 GHz Wi-Fi only (this board's
5 GHz capability is out of scope), and no factory-reset support (board has no onboard
pushbutton — `docs/BACKLOG.md` BL15). GPS: ATGM336H wired to GPIO4(RX)/GPIO5(TX). A new
`esp32c5-developer` subagent was added.

**Step 2 (transport/pairing/session-crypto + `wifi_scan`/`ble_scan`/`gps` capability porting)
is build-verified, flashed, and pairing-hardware-verified 2026-09-25.** `esp32c5/main/main.c`
replaces the placeholder with the full port from `esp32/main/main.c` (RISC-V/NimBLE-central
reference, not the Heltec's classic-Xtensa port): base transport/pairing/session-auth layer,
plus `wifi_scan` (2.4 GHz-only via `esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY)`,
confirmed against the installed ESP-IDF v5.5.2 headers), `ble_scan`, and `gps`
(`location.c`/`nmea_parser.c` ported unchanged, pins retargeted to GPIO4 RX/GPIO5 TX).
`feb_features[] = {"wifi_scan", "ble_scan", "gps"}` — no `wardriving`, matching this phase's
scope cut. New two-LED `status_led.c`: green (GPIO27, USER_LED1) mirrors the C6/Heltec's
connection-state indicator, red (GPIO26, USER_LED2) is wired for backlog-flushing but currently
unreachable (no wardriving this phase). No `factory_reset.c` (no onboard button — BL15).
`board_id` prefix `esp32c5-`. Partition table switched to
`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` (1500K factory) since the stock 1MB default was too
small for this build. `idf.py build` in `esp32c5/` is clean (15% free); `components/
feb_protocol/`, `esp32/`, and `heltec/` were not touched by this step. **Flashed to the physical
board (COM11) 2026-09-25; boot log confirmed healthy** — Wi-Fi and BLE both initialized, "wifi
band mode restricted to 2.4GHz only" logged, NimBLE started its v2 service-filtered scan, no
crashes. First boot found a stray NVS `pairing_secret`-shaped blob left over from whatever the
board ran before this project touched it (`idf.py flash` doesn't erase the NVS partition) and
attempted runtime auth with it — harmless, as expected: an unrelated blob can't produce a
matching session key, so it failed silently and fell through to a normal pairing window, exactly
like the C6/Heltec do when their stored secret doesn't match. **User confirmed a successful
pairing ceremony against the physical Flipper the same day** — this is this board's first-ever
pairing under `board_id=esp32c5-d0cf13feffe0`, so no stale capability-cache concern applies (the
gotcha that bit Heltec's steps 7/8/9). **Still unconfirmed:** a live `wifi_scan`/`ble_scan`/`gps`
round-trip against the paired Flipper, and a coexistence sweep for this board's own Wi-Fi 6 +
BLE 5 + 802.15.4 radio (new backlog item BL16, same accepted-gap class as Heltec's skipped step
5). Full detail:
`docs/PLAN.md`'s "Phase 8" section, `docs/BASELINES.md`'s MOD-ESP32-C5 entry,
[docs/hardware/olimex-mod-esp32-c5/README.md](hardware/olimex-mod-esp32-c5/README.md).

**Scope reversal + Step 3 (dual-band Wi-Fi + `wardriving` port), build-verified 2026-09-25,
hardware-verification pending.** Same day, the user lifted both of Step 2's scope cuts:
`start_wifi_subsystem()` now calls `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)` (2.4G+5G, no
wire-format change needed — a `wifi_scan` result's `channel` field is band-agnostic and 2.4/5GHz
channel numbers never overlap), and `wardriving` is now ported from the C6 reference (structurally
matching the Heltec's own port — the five `wardriving_*.c/.h` files copied unchanged, `main.c`'s
state machine/scan-arbitration/status-LED sync ported line-for-line, minus the boot-button
toggle mechanism this board has no button to trigger). `feb_features[]` is now
`{"wifi_scan", "ble_scan", "gps", "wardriving"}`. New custom `esp32c5/partitions.csv` (2 MB
factory app + 700-sector `wardrive` partition, identical sizing to `heltec/partitions.csv`) —
required deleting the generated `sdkconfig`/`build/` and rebuilding from scratch, since
`sdkconfig.defaults`' new `CONFIG_PARTITION_TABLE_CUSTOM=y` didn't take on an incremental
rebuild over Step 2's cached config (`docs/LESSONS.md`'s sdkconfig-defaults-not-retroactive
class). Red LED (`FEB_STATUS_LED_FLUSHING`) is now reachable via wardriving's backlog-flush
path; `feb_status_led_set_wardriving_active()` was added but is a deliberate no-op (BL17 — this
board's two-LED design never specified a third "wardriving active" visual state). `idf.py build`
clean for both changes, tested separately, no warnings; `components/feb_protocol/`, `esp32/`,
`heltec/` untouched. **Not done this pass**: no flashing, no live `wifi_scan`(5GHz)/`wardriving`
hardware verification (explicitly build-only per instruction). **Open, unconfirmed concern
(BL18)**: an earlier hardware session saw an apparent reboot-loop after a `wifi_scan` trigger,
recovered only by unplug/replug — not reproduced, not root-caused, and not chased in this
build-only pass.

**Step 4 (`wifi_band` — configurable dual-band scan speed/coverage), build-verified
2026-09-26.** Step 3's dual-band default (every 5GHz channel including DFS) made a manual
`wifi_scan` slow; user's explicit choice was "make it configurable". `wardriving`'s `start`
action gained a `wifi_band` field (`"2.4ghz"`/`"5ghz_fast"`/`"5ghz_full"`) in the shared
`cbor_wardriving.c`/`.h`, decoded like `country` (C6/Heltec decode-and-ignore it, no `main.c`
change needed there). `esp32c5/main/main.c` applies it once at wardriving start
(`wardriving_set_wifi_band_mode()` for the radio-level `esp_wifi_set_band_mode()` call,
`wardriving_apply_wifi_band()` for a per-scan `wifi_scan_config_t.channel_bitmap` restricting
`"5ghz_fast"` to 2.4GHz channels 1-14 + non-DFS 5GHz 36/40/44/48/149/153/157/161/165 — a
single `esp_wifi_scan_start()` call, confirmed against the installed ESP-IDF v5.5.2 headers
that a channel-bitmap scan needs no multi-scan restructuring). Boot default
`WARDRIVING_BAND_5GHZ_FULL` (matches Step 3's already-verified behavior); wardriving autostart
hardcodes the more conservative `WARDRIVING_BAND_2_4GHZ`, same reasoning as its existing
swelling/country hardcoding. `idf.py build` clean in `esp32c5/`, `esp32/`, `heltec/`;
`tests/esp32/build.ps1` host-native suite passes; `check_shared_headers.py` clean (Flipper
mirror landed concurrently in another session). **Not done this pass**: hardware verification
of any `wifi_band` value. Full detail: `docs/PLAN.md`'s Phase 8 Step 4,
`docs/hardware/olimex-mod-esp32-c5/README.md`'s scope-decision section.

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

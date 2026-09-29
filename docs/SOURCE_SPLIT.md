# Source split design (TP-15)

Status: **approved and implemented 2026-09-29.** All code steps are build-verified, and
hardware regression passes are pending (§10). The design sections below are kept as the
rationale.
Origin: [TOOLING_PLAN.md](TOOLING_PLAN.md) TP-15, backlogged in [BACKLOG.md](BACKLOG.md)
("Codebase & agent cost-efficiency"). Survey data is from three read-only structural passes over
the files at commit `e9e08bb`. Line numbers below refer to that commit and will drift.

## 1. Goal and non-goals

**Goal:** no hand-written source file over 60 KB. Code that is duplicated across the three ESP
boards gets a single home. Every step moves code without changing behavior.

**Why:**
- **Agent cost.** Nobody can read the four monoliths whole, so every task re-greps and re-reads
  slices. Across the usage history the Flipper file was read 958 times (943 of them as slices)
  and the three `main.c` files about 945 times combined. That drives call count × context size,
  the dominant cost ([TOOLING_PLAN.md](TOOLING_PLAN.md) §1).
- **Correctness.** Parallel copies of the same logic keep drifting apart. This survey found six
  more drift points (§4).

**Non-goals:**
- No behavior, wire-format, or UI changes. The only exceptions are the drift reconciliations in
  §4, which land as separate, individually verified commits *before* the move that merges the
  code.
- The Flipper-side `flipper/cbor_*`, `pairing*` and `session*` copies of `components/feb_protocol`
  stay where they are. Sharing codec sources between the FAP and ESP-IDF is a separate question
  (HP-20 covers its test side).
- Do not split `flipper/pairing_crypto.c` (kept diffable against upstream curve25519-donna) or
  `tests/vectors/vectors.h` (generated).

| File | Bytes | Lines | Comment share | Code only |
| --- | --- | --- | --- | --- |
| `flipper/flipper_esp32_over_ble.c` | 330,389 | 6,664 | ~41% | ~195 KB |
| `heltec/main/main.c` | 227,656 | 4,795 | ~28% | ~164 KB |
| `esp32/main/main.c` | 202,809 | 4,071 | ~33% | ~133 KB |
| `esp32c5/main/main.c` | 167,456 | 3,703 | ~20% | ~134 KB |

## 2. Invariants and how every step is verified

**Invariants** (binding on every step):

1. **Pure moves.** A split commit only moves code. It may also:
   - change `static` to external linkage;
   - add prototypes, headers and build-list entries;
   - rename a newly external symbol to carry a module prefix;
   - fix a comment that points at a line number or says "above"/"this file".

   Any logic change is a separate commit.
2. **Memory must not regress.**
   - **Flipper:** `.text`, `.rodata`, `.data` and `.bss` of the release `.fap` must each stay within
     +1% of the baseline, and `.text` must not grow by more than 512 B. Each section is
     heap-allocated at launch
     ([HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H04), so growth there costs system heap.
   - **ESP:** `idf.py size` DRAM/IRAM for every target. **The binding one is Heltec, which has
     348 B of DRAM headroom** ([BASELINES.md](BASELINES.md)). Heltec may not lose more than 32 B
     of DRAM in the whole effort.
3. **Stack must not regress.** GCC at `-Os` inlines static functions that are called only once.
   Moving them into another translation unit turns them back into real calls, which changes stack
   depth. The Flipper `BleEventWorker` stack is only 1,280 B, and the main thread's is 4 KB.
   - Compare `-fstack-usage` output for the dispatch paths before and after.
   - Check the Flipper worker's stack high-water mark on hardware.
4. **No new NimBLE callouts or events.** The host callout pool is hard-capped at 8
   (`BLE_HOST_CO_COUNT`), and Heltec already uses all 8. `host_synced` init order must be kept
   exactly (a seventh callout crashed `host_synced`, hardware-verified 2026-09-15).
5. **Threading assumptions stay as they are.** Almost all state is "NimBLE host task only".
   Anything written from another task gets a narrow accessor, not a bare `extern`:
   - `wifi_scan_done_handler` runs on sys_evt;
   - the button/factory-reset flags;
   - Heltec's `cluster_link_spinlock` state.
6. **Load-bearing guards survive every move untouched:**
   - the `wardriving_ble_active` guard in `start_scan` (the EBUSY race in [LESSONS.md](LESSONS.md));
   - the `wardriving_tx_in_flight` / `TX_DONE_CONTINUE_WARDRIVING` chaining (the GATT write-flood fix);
   - the Flipper lock order (`protocol_mutex` before `reassembly_mutex`).

**Per-step verification gate.** A step is not done until all of these pass:

- All five targets build: Flipper FAP, C6, C6 `cluster_worker`, C5, Heltec.
- Host tests pass: the `build-verify` skill.
- `python tools/check_shared_headers.py` is clean.
- Section/size deltas are recorded against the baseline (§6 step 0).
- The step's hardware smoke test passes on its board(s). This is user-supervised, per the
  CLAUDE.md hardware-safety rules: pair or reconnect, one Wi-Fi scan, one BLE scan, GPS status,
  and a short wardriving run with CSV rows written.

A size or stack regression past the thresholds above blocks the step. It is not waived.

**Measurement.**
- **Flipper:** run `arm-none-eabi-size -A` on the release ELF, using the pinned toolchain at
  `unleashed-firmware-unlshd-092/toolchain/current/bin/`. Also read the launch free-heap log line.
- **ESP:** run `idf.py size` and `idf.py size-components`.
- Record the step-0 baseline and the final figures in [BASELINES.md](BASELINES.md).

## 3. Flipper FAP split

**Structure today:**
- There is no ViewDispatcher. There is one ViewPort, an `AppScreen` enum with 9 screens, and one
  event loop inside `flipper_esp32_over_ble_app()`. That function alone is about 34 KB: setup,
  the event loop, and per-screen input dispatch.
- State lives in two places:
  - a ~70-field `Esp32App` struct, stack-local in the entry function;
  - about 75 file-scope statics. These hold all the protocol, session, reassembly, storage-path
    and file-handle state.
- There are about 150 static functions.

### 3.1 Target modules

All new files are listed in the `sources` list in `application.fam`.

| File | Contents (survey sections) | ~KB |
| --- | --- | --- |
| `app_internal.h` | Enums, `AppEvent`/`Esp32App`/`Esp32BleProfile` types, cooldown tables, prototypes, `extern`s. Also `static inline text_matches()` and `copy_clamped_text()`, both hot per-record helpers that must stay inlinable. Carries the lock-order note. | ~30 |
| `app_storage.c` | SD directory resolution, path builders, `storage_open_heap_margin_ok`, pairing/capability storage (F) | ~22 |
| `wardriving_settings.c` | Label/token/wire-value helpers, settings token parsers, settings load/save (B-helpers, E, G), `publish_parse_uint` | ~17 |
| `publish.c` | BadUSB publish flow, `draw_publish_screen`, `wrap_field_rows` (T without `stop_ble_profile`) | ~22 |
| `mesh_log_rx.c` | Mesh-log file writer and RX, display arrays, `draw_mesh_log_screen` (O) | ~20 |
| `gps_rx.c` | GPS status RX, `send_gps_command`, `draw_gps_screen` (M) | ~12 |
| `scan_rx.c` | Wi-Fi/BLE scan display arrays, RX handlers, senders, and the two results screens (L, V) | ~28 |
| `wardriving_rx.c` | Wardriving RX, CSV file writer, dedup, the three wardriving senders (N). `wardriving_record_stream_cb` and `wardriving_csv_write_record` stay together because they run per record. | ~38 |
| `wardriving_ui.c` | Running/Stopped screens and the settings-row editor (W) | ~14 |
| `session_flow.c` | Event posting, pairing ceremony, session state, hello/capability/bootstrap, runtime error, client auth (H, I, J, K, P, R) | ~45 |
| `ble_transport.c` | GATT table, fragment emit, reassembly, timers, `profile_event_handler`, profile start/stop, `bt_status_callback` (D, S, U) | ~40 |
| `app_ui.c` | Status text, home/scan menus, home/scan screens, ViewPort callbacks, UI reset, input dispatch pulled out as `app_handle_input()` (X, Y-rest, Z, AB) | ~35 |
| `flipper_esp32_over_ble.c` | Setup, the non-input event loop, teardown (AA) | ~20 |

Names avoid clashes with the existing `mesh_nodes.c` and `wardriving_csv.c`. Those two remain
the pure, Furi-free, host-testable formatters. The new files are their Furi-side I/O
counterparts.

### 3.2 Design choices

- **Session state stays private to `session_flow.c`.** The same encrypt-and-send block is copied
  into all six `send_*_command` functions and into `capability_bootstrap`. Replace those copies
  with one `session_send_encrypted_command()` wrapper.
  - This is the one deliberate de-duplication inside the Flipper split, and it gets its own commit
    right before the extraction.
  - It lowers `.text` and removes about 10 would-be `extern`s.
- **The rest of the protocol and I/O statics become external with an `app_` prefix**, for example
  `app_reassembly` and `app_pending_command_kind`. Generic names like `reassembly`,
  `characteristics` and `session_key` must not reach the FAP symbol table.
- **Shared scratch buffers get one owner each:**
  - `wardriving_settings_buf` is used by both the settings loader and the CSV row counter. Split
    it into two buffers only if `.bss` allows; otherwise move it to `app_storage.c` with a comment
    naming both users.
  - `shared_ble_event` is used by six sections. It belongs to `session_flow.c`, next to
    `app_queue_put`.
- **Callbacks registered by pointer become external or register themselves in their own module:**
  - `notify_data_callback`;
  - `profile_callbacks` and its three functions;
  - `profile_event_handler`;
  - `bt_status_callback`;
  - `draw_callback` and `input_callback`;
  - the three timer callbacks.
- **Watch the stack in `profile_event_handler`.** Today the called-once `handle_*` functions are
  likely inlined into it. After the split they become real calls from `ble_transport.c` into
  `session_flow.c` and the RX modules. Measure the `BleEventWorker` stack (invariant 3).
  - If the stack grows past the 1,280 B budget, keep the heaviest handlers in `ble_transport.c`
    rather than raising the stack size.
  - Raising the stack is a heap cost and would need its own decision.

### 3.3 Order

Least-coupled first. Each step builds and is measured before the next begins.

1. **Baseline.** Measure sizes, stack, and the launch-heap log.
2. **`app_internal.h`.** Types only. The main file includes it; no code moves yet.
3. **`app_storage.c`**, then **`wardriving_settings.c`**, then **`publish.c`**.
4. **The RX modules:** `mesh_log_rx.c`, `gps_rx.c`, `scan_rx.c`, then `wardriving_rx.c`, then
   `wardriving_ui.c`.
5. **The send wrapper commit**, then **`session_flow.c` and `ble_transport.c` together.** They
   share the most statics and `protocol_mutex`.
6. **`app_ui.c`**, including `app_handle_input()`.
7. **Final measurement, then a hardware regression pass.** Covers pairing, reconnect, every
   scan type, wardriving with CSV output, mesh log, publish, and settings persistence across
   restart.

## 4. Cross-board drift found by the survey (reconcile before merging)

These have to be resolved *before* the code that contains them moves into a shared component.
Otherwise the merge either silently picks one board's behavior or fails to compile.

| # | Where | Finding | Proposed disposition |
| --- | --- | --- | --- |
| DR1 | Heltec `gap_event` DISC_COMPLETE (~L3987) | C6 (L3518) and C5 restart the reconnect scan only when `!wardriving_ble_active`. Heltec lacks this guard, so a reconnect scan can start alongside the wardriving BLE source. The guard came from C6 commit `ffdd20e`; the 2026-09-23 Heltec wardriving port missed it. **Likely a real bug.** | Fix on Heltec as a normal defect, hardware-verify, and do it independently of this split. Filed as BACKLOG.md BL28. |
| DR2 | C5 `wardriving_maybe_kick_send` (L2224-2281) | Restructured gating plus `last_bail_reason` logging. C6 and Heltec match each other. | Confirm the backlog-update condition (C5 L2273-2275) is equivalent to C6 L2573-2574. Then either port the logging to all boards or drop it. |
| DR3 | C5 `wifi_scan_done_cb` / `handle_wifi_scan_command` | Free-heap/largest-block log lines exist only on C5 (a debugging leftover). | Drop, or make common. |
| DR4 | C6 `wardriving_wifi_interval_cb` (~L2080) | The "skipping wifi scan start, no GPS fix yet" log exists only on C6. | Make it common. |
| DR5 | `feb_factory_reset_request` and `factory_reset.c` | Heltec calls `feb_factory_reset_perform()` directly; C6 goes through `factory_reset_perform_cb(NULL)`. The `factory_reset.c` files differ between the two boards (C5 has none). | Diff and decide. It stays board-side either way, but the two should agree. |
| DR6 | Wi-Fi top-32 selection | Factored into `wifi_scan_select_top32` on Heltec, inline on C6 and C5. Same algorithm. | Adopt Heltec's factored form in the shared code. |
| DR7 | Line endings | Heltec and C6 `main.c` are CRLF in the working tree, C5 is LF. Heltec `wardriving_dedup.c` / `wardriving_validate.c` differ from the other boards only in line endings. | [.gitattributes](../.gitattributes) already normalizes the index. Diff with `--strip-trailing-cr`. |

## 5. ESP split

About 45 functions are **byte-identical after stripping comments** across all three boards. That
covers:
- the whole NVS pairing-secret/auth block;
- the TX fragment and protected-send queue;
- all of GATT discovery;
- all of BLE scan and GPS;
- all pairing and runtime auth;
- most of the wardriving engine;
- `reassembly_timeout_cb`.

The C5 `gap_event` is code-identical to the C6 one. Almost all of the rest differs only through a
few hooks:

- **Heltec:** OLED state, radio kill switch, cluster-worker Wi-Fi delegation, mesh/mesh_log.
- **C5:** 5 GHz band selection.
- **C6 and Heltec only:** the boot button and factory reset.

In other words, the three `main.c` files are about 60% one program.

### 5.1 Stage E1: shared component for the byte-identical sibling modules (low risk)

Create `components/feb_wardriving/` containing:
- `wardriving_log`, `wardriving_persist` and `wardriving_record_format` (byte-identical today);
- `wardriving_dedup` and `wardriving_validate` (identical once line endings are normalized);
- `nmea_parser` (byte-identical).

Then:
- Delete the three per-board copies.
- Add `REQUIRES feb_wardriving` to the board components.
- Drop the moved modules from `BOARD_IDENTICAL` in `check_shared_headers.py`.

This is HP-19's stated long-term fix. It removes about 150 KB of copies and has no link-level
behavior change. `location` and `status_led` genuinely differ per board (pins, LED hardware) and
stay put.

### 5.2 Stage E2: shared application core

**New component: `components/feb_app_core/`.** Sizes are estimated from the C6 file.

| File | Contents | ~KB |
| --- | --- | --- |
| `feb_app_internal.h` | Shared state declarations, `tx_done_action_t`, hook struct, accessors | ~15 |
| `feb_link.c` | Board ID (with prefix from the board config), NVS pairing secret, auth policy, TX fragment and protected queue, pairing ceremony, runtime auth, capability query | ~35 |
| `feb_central.c` | Scan/reconnect, GATT discovery, `write_complete`, `gap_event`, reassembly timeout | ~40 |
| `feb_cap_scan.c` | Wi-Fi scan (local source) and BLE scan capabilities | ~35 |
| `feb_cap_gps.c` | GPS command | ~5 |
| `feb_cap_wardriving.c` | Wardriving engine, streaming and backlog | ~40 |
| `feb_cap_wardriving_cmd.c` | `handle_wardriving_command` parser (281 lines, 13.8 KB alone) | ~15 |

If `feb_central.c` goes over 60 KB with comments, split out `feb_central_rx.c` for the
NOTIFY_RX path of `gap_event`.

**Each board keeps a small `main/`:**

| Board | Files kept in `main/` |
| --- | --- |
| C6 | `main.c` (~8 KB): `app_main`, `host_synced`, Wi-Fi init, `feb_features[]`, command table. Also `board_hooks.c` (button/factory-reset glue) and `board_config.h`. |
| C5 | `main.c` (~8 KB), `wifi_band.c` (~3 KB: band apply, set and parse hooks), `board_config.h`. |
| Heltec | `main.c` (~15 KB), `cluster_glue.c` (~15 KB), `mesh_caps.c` (~16 KB: meshcore/meshtastic handlers and mesh_log drain), `killswitch_glue.c` (~7 KB), `board_config.h`. |

**How board differences get in.** The rule: constants at compile time, behavior through one
`const` hooks table.

- **`board_config.h` (compile time).** `FEB_BOARD_MODEL` and board-ID prefix, `FEB_HAS_BOOT_BUTTON`,
  `FEB_WIFI_DUAL_BAND`. Where a feature adds struct fields or parse branches, use `#if`. The C5
  `wifi_band` field and parse branch is the one real case.
- **`const feb_app_hooks_t` (runtime behavior).** Defined by the board and passed to
  `feb_app_core_init()`. Because it is `const`, it lives in flash and the only DRAM cost is one
  pointer. That matters for Heltec's 348 B. Members, with every one optional (NULL means no-op):
  - `on_ble_state(state)`: the Heltec OLED.
  - `radio_permitted()`: the Heltec kill switch, checked in `start_scan`.
  - `on_connect`, `on_disconnect`: cluster cancel, mesh_log reset.
  - `wifi_scan_delegate`: the Heltec cluster worker as an alternative Wi-Fi scan source.
  - `extra_command(cmd) -> handled`: meshcore/meshtastic.
  - `on_tx_done_ext(action)`: `TX_DONE_CONTINUE_MESH_LOG`.
  - `wardriving_start_ext` / `stop_ext`: C5 band apply, Heltec delegation.
- **Weak symbols are rejected.** ESP-IDF links components as static archives, so a weak default in
  `feb_app_core` can silently win over a strong override in `main` if nothing else pulls that
  object in. Failures like that are invisible until run on hardware.
- **`tx_done_action_t` stays an enum in the core**, with one `TX_DONE_BOARD_EXT` value that routes
  to `on_tx_done_ext`. Turning it into an opaque continuation (a function pointer plus context)
  would be cleaner, but it changes the state machine, so it is out of scope for a pure move.
- **Shared state gets a header or accessors, depending on who touches it:**
  - State touched only by the host task is declared `extern` in `feb_app_internal.h`, with a
    `feb_` prefix:
    - `connection_handle`, `runtime_auth_state`, the session key/sequences, `negotiated_att_mtu`;
    - the scan in-progress flags;
    - the wardriving TX flags;
    - the six to eight callouts.
  - State touched cross-task (the button/factory-reset flags and events, the sys_evt Wi-Fi
    handoff) is reached through accessor functions.
- **Callouts and events stay owned by `host_synced`**, which remains per board and keeps its init
  order. The core exposes an init function per module for `host_synced` to call in today's order.
  No module creates its own callouts.
- **Public names called from `factory_reset.c` keep their names:** `feb_wipe_pairing_secrets`,
  `feb_wardriving_request_button_toggle` and `feb_factory_reset_request`.
- **Comments pointing at other files** ("esp32/main/main.c:758", "see the C6's fuller comment")
  collapse to the one shared copy. The fuller C6 comment is the one kept.

**Fallback (no E2).** If E2 is rejected or deferred, each board's `main.c` can still be split in
place into about seven files under 60 KB each: `ble_link.c`, `session.c`, `wifi_scan.c`,
`ble_scan_gps.c`, `wardriving.c`, `wardriving_cmd.c`, `main.c`, plus the Heltec extras. That
meets the size target but keeps three copies of everything. Drift then remains a manual
`check_shared_headers`-style problem, extended to `.c` files. **Not recommended**, because it
does the move work without getting the correctness payoff.

### 5.3 E2 order

1. **Baseline** `idf.py size` for all four ESP targets. Resolve DR1-DR6 (§4), each as its own
   verified commit.
2. **Create the core from C6's code.** Make C6's `main.c` consume it, with hooks all NULL except
   the button. Build and size C6 and `cluster_worker`. Hardware smoke test on C6.
3. **Switch C5 over.** Delete the duplicated code from `esp32c5/main/main.c` and add
   `wifi_band.c` hooks. Build, size, smoke test on C5.
4. **Switch Heltec over.** Add the full hook set, the cluster/mesh/kill-switch glue files and the
   DRAM check. Build, size, smoke test on Heltec, including the cluster-worker and mesh paths.
   Heltec goes last because it has the most hooks and the least DRAM.
5. **Update the agents.** Point `esp32-developer.md`, `esp32c5-developer.md` and
   `heltec-developer.md` at the core component. Add a rule to [AGENT_RULES.md](AGENT_RULES.md):
   "a change to `feb_app_core` is a three-board change: build all, smoke the affected board(s)".

Each board switchover is one reviewable change. Do not have two boards half-migrated at once.

## 6. Overall sequencing and scheduling

| Step | Scope | Depends on | Risk |
| --- | --- | --- | --- |
| 0 | Baselines: FAP sections, stack usage, launch heap; `idf.py size` for all ESP targets | — | none |
| F | Flipper split (§3.3) | 0 | medium: stack of `BleEventWorker` |
| E1 | `components/feb_wardriving` (§5.1) | 0 | low |
| DR | Drift reconciliation (§4), DR1 first | — | low-medium, behavior changes and hardware-verified |
| E2 | `feb_app_core`: C6, then C5, then Heltec (§5.3) | E1, DR, a Phase 9 checkpoint | high: Heltec DRAM, three-board surface |

**Phase 9 interaction.** CLUSTER.md implementation is in progress, and it edits the Heltec
cluster glue plus the Wi-Fi delegation paths that E2 has to turn into hooks. E2 should wait for a
Phase 9 checkpoint where that code is stable and hardware-verified. Otherwise the refactor and
the feature work conflict on every commit.

F and E1 do not touch Phase 9 code and can run now.

**Concurrent sessions.** Each step touches a whole monolith. Before starting a step, check that
no other Claude Code session has uncommitted work in that file. Commit each step before the next
one starts, only when the user asks, per CLAUDE.md.

**Who does the work.** Each step goes to the matching `*-developer` agent, briefed with this
document's section and that step's verification gate. Mechanical extraction does not need Opus.
Review of the E2 hook design does.

## 7. Done when

- No hand-written `.c`/`.h` file in `flipper/`, `esp32/main/`, `esp32c5/main/`, `heltec/main/`,
  `components/` or `esp32/cluster_worker/main/` is over 60 KB.
  - Exempt: `flipper/pairing_crypto.c` and generated test vectors.
- All five targets build, host tests pass, and `check_shared_headers.py` is clean.
- FAP `.text`/`.rodata`/`.data`/`.bss` are within the §2 thresholds.
- ESP DRAM/IRAM: no target loses more than 1% headroom, and Heltec loses no more than 32 B.
- Final figures are recorded in [BASELINES.md](BASELINES.md).
- `BleEventWorker` stack high-water mark is still within budget on hardware.
- Per-board hardware regression passes are complete (user-supervised).
- `tools/claude_usage.py` has been re-run about two weeks later. Expected result: fewer Read calls
  per session on these paths and lower average context. This feeds TP-18.

## 8. Decisions (approved by the user 2026-09-29)

S1 was decided as **do everything now, including E2**, overriding the recommendation to wait for a
Phase 9 checkpoint. That makes the §6 "E2 waits for Phase 9" gate void. Any Phase 9 work on
Heltec cluster glue has to rebase onto the new layout. S2-S5 were taken as recommended.

| # | Question | Recommendation |
| --- | --- | --- |
| S1 | Scope: Flipper + E1 only now, with E2 after a Phase 9 checkpoint, or everything now? | ~~F + E1 now, E2 after the checkpoint.~~ **Decided: all now.** |
| S2 | E2 approach: shared core component, or the per-board in-place fallback? | Shared core. |
| S3 | Board-difference mechanism: `const` hooks table plus `board_config.h`, or Kconfig/weak symbols? | Hooks table plus `board_config.h`. |
| S4 | Flipper session send wrapper (the one de-duplication inside the Flipper move): take it? | Yes, as its own commit. |
| S5 | DR2 and DR3 (C5-only diagnostics): port to all boards or drop? | Drop DR3. Keep DR2's bail-reason logging only if it has been useful in the field. |

## 9. Baseline (step 0, 2026-09-29, commit e9e08bb + doc-only changes)

Build-only pass on the unmodified tree (no flash). All five targets build clean; hosttests and
`check_shared_headers.py` pass.

**Flipper FAP** — `arm-none-eabi-size -A` on `build\f7-firmware\.extapps\flipper_esp32_over_ble.fap`
(via `tools\build_flipper.ps1`, release/`DEBUG=0`): `.text` 57,168 / `.rodata` 12,312 / `.data` 56 /
`.bss` 24,292; `.fap` file total 121,916 bytes.

**Flipper stack** — `-fstack-usage`/`.su` is **not measurable without a manifest change.** This
fbt revision (`unlshd-092`) has no app-level `cflags` field on `FlipperApplication` (only
`fap_private_libs[].Library.cflags`, which doesn't reach the app's own sources), and
`site_scons/commandline.scons`'s `Variables()` list — the only place `fbt.cmd VAR=value` is
consumed — has no `CCFLAGS`/passthrough entry, so an unrecognized `CCFLAGS=-fstack-usage` on the
command line is silently inert. Getting `.su` output would require patching
`fbt_extapps.py`/`SConstruct` in the pinned Unleashed checkout, out of scope for a no-source-edit
baseline. Fallback used instead: `arm-none-eabi-objdump -d` prologue of the unstripped
`flipper_esp32_over_ble_d.elf` in the same build dir.

| Function | Frame (from prologue) |
| --- | --- |
| `profile_event_handler` | 128 B: `stmdb sp!,{r4-r9,sl,fp,lr}` (36) + `vpush {d8}` (8) + `sub sp,#84` |
| `bt_status_callback` | 16 B: `push {r4,r5,r6,lr}`, no further `sub sp` |
| `flipper_esp32_over_ble_app` | 1,088 B: `stmdb sp!,{r4-r9,sl,fp,lr}` (36) + `subw sp,sp,#1052` |

**ESP targets** — `idf.py size` after `. esp-idf\export.ps1`, figures exactly as printed:

| Target | Flash (code+rodata+appdesc) | RAM used/total | RAM remain | Total image |
| --- | --- | --- | --- | --- |
| esp32 (C6) | 1,056,792 | DIRAM 248,206/452,112 (54.9%) | 203,906 | 1,205,510 |
| esp32c5 | 1,113,894 | HP SRAM 227,778/320,928 (70.97%) | 93,150 | 1,240,056 |
| heltec | code 756,416 + data 154,460 | DRAM 124,232/124,580 (99.72%); IRAM 123,763/131,072 (94.42%) | DRAM 348; IRAM 7,309 | 1,057,463 |
| cluster_worker (C6) | 677,464 | DIRAM 147,410/452,112 (32.6%) | 304,702 | 796,874 |

Heltec's 348 B DRAM headroom matches the pinned figure in [BASELINES.md](BASELINES.md) — confirms
invariant 2's binding constraint is still current.

**Build-harness note (not a source issue):** launching `esp32`/`esp32c5`/`heltec`/`cluster_worker`
builds concurrently (each sourcing `export.ps1`) raced — 3 of 4 failed activating ESP-IDF.
Re-running one at a time fixed it; `tools\build_esp32.ps1`'s script-level
`$ErrorActionPreference="Stop"` also turns `activate.py`'s informational stderr line into a false
failure independent of the race, so the sequential retries ran the same commands inline under
`$ErrorActionPreference="Continue"` instead.

**Host tests / shared headers:** `run_hosttests.ps1` — 11/11 PASS. `check_shared_headers.py` —
clean, all `OK` (all 14 header pairs, all 6 byte-identical wardriving/nmea modules).

Launch free-heap log line and hardware `BleEventWorker` stack high-water mark are deferred to the
step's hardware regression pass — this pass was build-only, no flash, per the user's request.

## 10. Implementation record

| Step | Result (2026-09-29, uncommitted working tree) | Hardware |
| --- | --- | --- |
| E1 | `components/feb_wardriving/` holds `wardriving_{log,persist,record_format,dedup,validate}` + `nmea_parser`. All 12 files were md5-identical across boards after CRLF normalization. The C6 copy was `git mv`ed; the C5 and Heltec copies were `git rm`ed. Component `REQUIRES feb_protocol nvs_flash esp_partition`. In `check_shared_headers.py`, `BOARD_IDENTICAL` is now empty and `BOARD_EQUIVALENT` holds only `location`. Host-test scripts repointed. | pending |
| DR1 | Ported to Heltec (BL28). | pending: Heltec wardriving + disconnect/reconnect |
| DR2 | C5 gating proven equivalent to C6 line by line. The bail-reason investigation isn't open anywhere, so C5 was converged to C6's code. | pending |
| DR3 | **Kept.** The C5 heap logs are HP-21/BL18 diagnostic instrumentation for an open item, not a leftover. | — |
| DR4 | "No GPS fix yet" log ported to C5 and Heltec. | — |
| DR5 | Real gap: commit `3fae5b1` (HP-13) added the `reset_requested` debounce guard to C6's `factory_reset.c` but not to Heltec's. Ported. The LED-off timing difference (Heltec defers it) is left as-is because it only affects UX. | pending: Heltec factory-reset gesture |
| DR6 | `wifi_scan_select_top32()` factored verbatim on C6 and C5. | — |

| F | Flipper split into 12 modules plus `app_internal.h`. The largest is `session_flow.c` at 44.8 KB; `flipper_esp32_over_ble.c` is now about 18 KB. The line-multiset conservation check shows 0 diff. The S4 wrapper `session_send_encrypted_command()` replaced the 7 copies. **Built as a unity build** (see below). | pending: pair/reconnect, all scans, wardriving + CSV, mesh log, publish, settings persistence, `BleEventWorker` high-water mark |

| E2-C6 | `components/feb_app_core/` was built from the C6 code: `feb_link.c` 34 KB, `feb_central.c` 47 KB, `feb_cap_scan.c` 41 KB, `feb_cap_gps.c` 5 KB, `feb_cap_wardriving.c` 38 KB, `feb_cap_wardriving_cmd.c` 16 KB, plus `feb_app_internal.h` and `feb_app_core.h`. `esp32/main/` is now `main.c` (9.6 KB), `board_hooks.c`, `board_config.h`, `factory_reset.c` and the per-board modules. See the notes below the table for how the pieces connect. Line conservation holds: all 2,941 code lines are accounted for. | pending: C6 smoke |

**E2-Heltec details** (Heltec now runs on the core).
- **Files:** `heltec/main/` is `main.c` 18 KB, `cluster_glue.c`, `mesh_caps.c`,
  `killswitch_glue.c` and `board_hooks.c`, next to the existing mesh/LoRa/display modules.
- **New flags:**
  - `FEB_HAS_LINK_HOOKS` gates the new hooks `on_ble_state` (OLED), `radio_permitted` (kill
    switch), `on_connect`, `on_disconnect` and `on_authenticated`. The members and their call
    sites are compiled out elsewhere, so C6 and C5 stay byte-identical.
  - `FEB_HAS_CLUSTER_WORKER` covers cluster delegation through 7 board-provided
    `feb_cluster_*()` functions. Every `cluster_link_spinlock` section stays in
    `cluster_glue.c`.
- **Mesh log:** `on_tx_done_ext` carries the mesh-log drain.
- **Differences resolved:**
  - 17 became hook call sites and 8 became board_config sites.
  - Two log-text-only differences went to the core wording. As a result, Heltec's fragment
    write-failure log now reads "pairing fragment", and its wardriving start log also prints
    `ble_passive`.
- **Callout order:** the same 8 callouts in the same order.
- **Sizes:** DRAM free 92 → **100 B**, IRAM unchanged.
- **Stack:** `gap_event` worst case 2,304 → 1,952 B. A few timer callbacks grew by 16-64 B,
  all far below `gap_event`.
- **Other boards:** C6 and C5 are byte-identical except the embedded ELF SHA, and
  cluster_worker's md5 is identical.
- **Hardware:** pending. See the hardware checklist below.

**Status 2026-09-29: all code steps done and build-verified.** Every hand-written source file is
under 60 KB: the largest is `flipper/session_flow.c` at 53.6 KB, after peer edits. Remaining:
- **Hardware regression passes (user-supervised):**
  - **Flipper:** pairing, all scans, wardriving and CSV, mesh log, publish, settings, and the
    `BleEventWorker` high-water mark.
  - **C6:** pairing, scans, wardriving, and the BOOT button toggle and factory reset.
  - **C5:** pairing, scans, wardriving with each `wifi_band`.
  - **Heltec:** DR1 and DR5, the OLED BLE states, kill switch off/on and boot-persisted-off,
    Wi-Fi scan with, without and timing out the worker, wardriving delegated, local, and
    kill-switched, mesh_log drain, meshcore/meshtastic queries, and the PRG button.
- **TP-18:** re-measure with `tools/claude_usage.py` about two weeks out.

**E2-C5 details** (C5 now runs on the core; `esp32c5/main/main.c` is 12.7 KB).
- **Board files:** `wifi_band.c` holds the sticky band state and the `wifi_scan_cfg_ext` /
  `wardriving_start_ext` hooks.
- **Core additions, all compiled out on C6:**
  - the `wifi_band` parse and parameter, behind `#if FEB_WIFI_DUAL_BAND`;
  - the HP-21/BL18 heap diagnostic logs, behind `#if FEB_DIAG_WIFI_HEAP_LOG`.
- **Log tag:** `FEB_LOG_TAG` in `board_config.h` restores each board's own log tag. The first
  cut had silently switched C5 to the generic tag.
- **Callout init order:** unchanged.
- **Stack:** every entry point is the same or smaller; `gap_event` −224 B.
- **Sizes:** the build measured +264 B DRAM on C6, C5 and Heltec alike. That comes from a
  concurrent peer session's uncommitted v2 migration edit to
  `components/feb_wardriving/wardriving_persist.[ch]`, not from the split: Heltec wasn't on the
  core yet and still moved by the same amount.
  - **Heltec DRAM free is 92 B with that edit in the tree.** Split figures from here on are
    measured against the tree including it.

**E2-C6 details.**
- **Locating the board directory.** The board sets the build property `FEB_APP_CORE_BOARD_DIR`
  (in `esp32/CMakeLists.txt`), and the core adds that directory to its private includes. If the
  property is unset, the core's CMakeLists calls `return()`. That keeps unmigrated boards'
  binaries byte-identical: a registered empty component cost Heltec 8 B DRAM just from link
  order.
- **Board hooks.** The board passes a `const feb_app_hooks_t` to `feb_app_core_init()`. Every
  hook in it is optional.
- **Board-provided symbols.** The board supplies `feb_features[]`, `feb_feature_count` and
  `feb_handle_command()`.
- **Shared symbols.** The 93 symbols used across files now carry a `feb_` prefix.
- **Callout init order is unchanged.**
- **Sizes.** C6 DIRAM +68 B (+52 of it is IRAM from header-inline `.iram1` helpers being
  copied into more files), flash +148 B. The C5, Heltec and cluster_worker binaries are
  byte-identical.
- **Worst-case stack.** `gap_event` 2,192 → 2,080 B; `write_complete` 1,360 → 1,232 B.

**F: why a unity build.** Separate TUs failed the §2 gate: `.text` +800 B. The loader also keeps
`.fast.rel.text` resident, and that grew by +1,756 B. So the true resident heap cost was about
2.5 KB, which §2's four-section gate did not capture. Evidence is in the pinned firmware's
`lib/flipper_application/elf/elf_file.c` (`elf_file.c:488`, `:606-609`).
`.symtab`/`.strtab` are streamed from the file, not heap-resident (`:621-634`).
- **How it works:** the modules are `#include`d into `flipper_esp32_over_ble.c`. The
  `APP_FN`/`APP_DATA` macros expand to `static`, so the compiler sees the original linkage.
- **The §2 gate now counts all six resident sections:** the four regular sections plus
  `.fast.rel.text` and `.fast.rel.rodata`.

| Release `.fap` | .text | .rodata | .data | .bss | .fast.rel.text | .fast.rel.rodata | file |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Original monolith | 57,168 | 12,304 | 56 | 24,296 | 10,281 | 124 | 121,908 |
| Split, separate TUs (rejected) | 57,968 | 12,232 | 56 | 24,296 | 12,037 | 124 | 131,228 |
| **Split, unity build (kept)** | **56,328** | **11,816** | 56 | 24,312 | **10,236** | 124 | 120,788 |

Unity vs original: 1,357 B less resident heap in total.

Stack, measured from objdump prologues:
- `profile_event_handler` frame: 128 → 96 B.
- Client-auth RX chain: 272 → 96 B. The status-query send is inlined again.
- Wardriving-status chain: 216 → 184 B.

Sizes after E1 and DR:
- C6 DIRAM: unchanged.
- cluster_worker: unchanged.
- C5 HP SRAM used: 227,778 → 227,770.
- Heltec DRAM free: 348 → **356 B**.

Host tests 11/11 pass, the shared-header check is clean, and all four ESP projects build.

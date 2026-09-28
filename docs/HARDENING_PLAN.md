# Hardening plan (code review 2026-09-28)

Output of a full read-only review of all four firmwares (Flipper FAP, ESP32-C6, OLIMEX
ESP32-C5, Heltec V2), the shared `components/feb_protocol/` + `components/feb_cluster_link/`
layer, the host publish script, and tooling/tests — reviewed against
[USER_GUIDE.md](USER_GUIDE.md), [PROTOCOL.md](PROTOCOL.md), [CLUSTER.md](CLUSTER.md),
[WARDRIVING_PUBLISH.md](WARDRIVING_PUBLISH.md) and the memory constraints in
[HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H04 / [BACKLOG.md](BACKLOG.md) BL23-BL27.

**Scope of what was reviewed:** the working tree as of 2026-09-28, *including other sessions'
uncommitted edits*. Where a finding exists only in uncommitted code it is marked **[WT]** — it
is a pre-commit blocker for whoever owns that edit, not a shipped bug.

**Relationship to other docs:** items already tracked elsewhere are not re-listed as new; §5
records only new evidence against them. When an item here is fixed, move it to
[PROJECT_HISTORY.md](PROJECT_HISTORY.md) per the usual convention and leave a one-line pointer.
Items that turn out to need a design pass move to HARDENING_BACKLOG.md.

**Verification legend:** *Confirmed* = code path traced end to end (the P0/P1 items were
re-traced by the reviewing session, not just reported by a sub-reviewer). *Plausible* = mechanism
traced, but triggering depends on timing or on hardware behaviour not measured.

## 0. Implementation status (2026-09-28)

**Implemented the same day; build- and host-test-verified, none of it hardware-verified.** All
five firmware targets build with zero warnings, all 11 host suites pass, and
`check_shared_headers.py` passes. Narrative is in PROJECT_HISTORY.md's 2026-09-28 entry; sizes
are in BASELINES.md.

| Status | Items |
| --- | --- |
| Done | HP-01..HP-20, HP-22..HP-33, HP-35, HP-37..HP-43 |
| Partly done | **HP-10:** residual risk on the rename fallback if Back is pressed mid-run (detected and reported, see WARDRIVING_PUBLISH.md §4). **HP-15:** high-water-mark logging added; the measurement itself needs hardware. **HP-21:** heap logging hooks added on the C5; needs a BL18 hardware session. |
| Documented only | **HP-34:** USER_GUIDE now says a US keyboard layout is required. **HP-44:** BACKLOG row. |
| Hardware check needed | **HP-36:** Method 2 HMAC key encoding. **HP-16:** confirm on a default-policy Windows account. |

**Hardware test checklist, in priority order:**
1. C5 autostart plus a forced disconnect/reconnect (HP-01).
2. No-fix wardriving with 0 s cooldown on the C6 and Heltec (HP-02/03).
3. Heltec kill-switch OFF→ON→`wifi_scan`, and a touch during cluster-delegated wardriving
   (HP-04/05).
4. Flipper GPS-screen polling during a backlog drain, plus fast reconnects (HP-06/07/08).
5. Wardrive → Publish without restarting the app, on a default-policy Windows account
   (HP-09/10/16/17).
6. Read the Heltec LoRa RX stack high-water mark (HP-15).

Follow-ups found during implementation:
- The Flipper's other fixed-order decoders still report `OUT_OF_ORDER` for an unknown key where
  the ESP reports `UNEXPECTED_TYPE`. No shared vector hits this yet; adopt
  `feb_cbor_i_decode_table_key()`.
- The Flipper's `feb_cbor_encode_wardriving_record()` requires `source`; the ESP encoder doesn't.
- In the C6/C5 build, `CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER` drops out under `-Os` (see
  BASELINES.md).
- The Heltec cluster-delegated re-arm in `wifi_scan_done_cb()` still uses the raw interval. It's
  paced by the worker's frames, so it can't busy-loop, but clamp it if a delegated 0 ms
  starvation is ever suspected.
- The C5 agent definition still says "2.4 GHz only". The user lifted that 2026-09-25, and the
  definition is left for the user to update.

The sections below are the original review text as written before implementation.

---

## 1. Priority summary

| ID | Board | Title | Sev | Conf |
| --- | --- | --- | --- | --- |
| HP-01 | C5 | `start_scan()` lost the `wardriving_ble_active` guard (H01 class, every autostart boot) | critical | confirmed |
| HP-02 | C6, Heltec **[WT]** | No-fix Wi-Fi re-arm at raw interval → 0 ms refire loop → task-WDT | high | confirmed |
| HP-03 | C6, Heltec **[WT]** | No-fix skip added to BLE interval cb → reconnect stalls while no GPS fix | high | confirmed |
| HP-04 | Heltec | Kill-switch frees `wardriving_cluster_flush_records` while host task reads it (UAF) | high | confirmed |
| HP-05 | Heltec | Kill-switch re-enable never restarts Wi-Fi (`esp_event_loop_create_default` → INVALID_STATE) | high | confirmed |
| HP-06 | Flipper | Shared static GCM nonce/AAD scratch raced by main-thread encrypt vs BLE-thread decrypt → nonce reuse | high | plausible |
| HP-07 | Flipper | Main thread's delayed `BtStatusConnected/Advertising` handling wipes an already-advanced session/pairing | high | plausible |
| HP-08 | Flipper | `wifi_scan`/`ble_scan` post one event per item into an 8-deep queue, unchecked → drops incl. critical events | high | confirmed |
| HP-09 | Flipper + script | Publish leaves CSV/mesh files open → CLI `storage` blocks forever | high | confirmed |
| HP-10 | script | Delayed `storage rename` can archive never-uploaded rows | high | plausible |
| HP-11 | script | Rename-as-copy of a multi-MB CSV exceeds 10 s timeout → success reported as fail, CLI desync | high | plausible |
| HP-12 | all ESP | Stack canary disabled (`CONFIG_COMPILER_STACK_CHECK_MODE_NONE`) despite 4+ stack-overflow incidents | high | confirmed |
| HP-13 | C6, Heltec | Factory-reset `nvs_flash_erase()` on its own task, uncoordinated with host-task NVS writes | high | plausible |
| HP-14 | all ESP | ESP images still built `-Og` (`CONFIG_COMPILER_OPTIMIZATION_DEBUG`) | medium | confirmed |
| HP-15 | Heltec | LoRa RX task (4096 B) stack depth never measured since Meshtastic AES+protobuf path was added | medium | plausible |
| HP-16 | Flipper bootstrap | Win+R `powershell` doesn't bypass execution policy → fails on stock Windows | medium | confirmed |
| HP-17 | script | Result file is an unordered hashtable holding raw HTTP bodies; FAP reads 512 B | medium | confirmed |
| HP-18 | script | Port discovery opens every COM port with DTR → resets attached ESP32/Heltec | medium | plausible |
| HP-19 | tooling | `check_shared_headers.py` ignores per-board module copies and all `.c` logic | medium | confirmed |
| HP-20 | tests | Flipper `cbor_meshcore.c`/`cbor_mesh_log.c`/`mesh_nodes.c` have zero host coverage | medium | confirmed |
| HP-21 | C5 | Dual-band scan heap pressure — BL18 hypothesis to measure | medium | plausible |
| HP-22…HP-40 | various | Low-severity items (§4) | low | — |

---

## 2. P0 — fix before the next hardware session

### HP-01 — C5 `start_scan()` missing the centralized `wardriving_ble_active` guard
- **Where:** [esp32c5/main/main.c:609-633](../esp32c5/main/main.c#L609-L633); compare
  [esp32/main/main.c:758](../esp32/main/main.c#L758), [heltec/main/main.c:697](../heltec/main/main.c#L697).
- **What:** the C6/Heltec `start_scan()` early-returns while wardriving's BLE source owns
  discovery, the exact fix for the 2026-09-10 root cause (LESSONS.md) behind H01/G36. The C5 copy
  doesn't have it. `git log -p` shows commit `9519a60` (the Phase 8 port) deleted it. Every
  `BLE_GAP_EVENT_DISCONNECT` reconnect path and `host_synced()` after wardriving autostart call it
  unconditionally, and the autostart code's own comment (~main.c:3575) assumes the guard exists.
- **Failure:** any disconnect during C5 BLE wardriving, and **every boot with BLE-source
  autostart**, races `wardriving_ble_interval_cb()`'s `ble_gap_disc()` → `BLE_HS_EBUSY`/failed
  connects → the reconnect stall users have already hit on the C6.
- **Fix:** re-add `if (wardriving_ble_active) return;` before `connecting_permitted()`. One line.
- **Verify:** C5 build; hardware test of autostart + forced disconnect/reconnect.

### HP-02 — [WT] Zero-delay no-fix Wi-Fi refire on C6 and Heltec
- **Where:** `wardriving_wifi_interval_cb()` in [esp32/main/main.c:2031](../esp32/main/main.c#L2031)
  and [heltec/main/main.c:2416](../heltec/main/main.c#L2416) (uncommitted edits in both; HEAD has no
  no-fix skip). Reference fix: [esp32c5/main/main.c:1810-1826](../esp32c5/main/main.c#L1810-L1826)
  + `FEB_WARDRIVING_NO_FIX_RETRY_FLOOR_MS` in `esp32c5/main/wardriving_validate.h:97`.
- **What:** on no fix, the new branch re-arms the callout with the raw
  `wardriving_wifi_interval_ms`. `FEB_WARDRIVING_WIFI_INTERVAL_MIN_MS` is `0`, and "WiFi Cooldown
  0s" is a normal user-selectable Flipper setting. The C5 hardware-reproduced this on 2026-09-27:
  a 0 ms refire loop starves IDLE on CPU0 and trips the task watchdog.
- **Fix:** port the floor constant into both boards' `wardriving_validate.h` and clamp the retry,
  as the C5 does.

### HP-03 — [WT] No-fix skip in BLE interval callback stalls reconnect (C6, Heltec)
- **Where:** `wardriving_ble_interval_cb()` [esp32/main/main.c:2069](../esp32/main/main.c#L2069),
  [heltec/main/main.c:2462](../heltec/main/main.c#L2462) (uncommitted). The C5 tried this and
  reverted it; the rationale is at [esp32c5/main/main.c:1857-1864](../esp32c5/main/main.c#L1857-L1864).
- **What:** that discovery window is also the reconnect scan (`start_scan()` defers to it, see
  HP-01). Skipping it while there's no fix means the Flipper is never found. The C5 reproduced this
  live on 2026-09-27: autostart, no fix, never reconnected.
- **Fix:** keep the BLE discovery unconditional. Discard no-fix results at window close, as the C5
  does. Only the Wi-Fi side may skip.
- **Process note:** the rationale for HP-02 and HP-03 exists only in a C5 code comment, and two
  parallel sessions have missed it. Promote both rules into [LESSONS.md](LESSONS.md) and into the
  three ESP developer agent definitions (per the "fold lessons into agents" convention).

### HP-04 — Heltec kill-switch UAF on `wardriving_cluster_flush_records`
- **Where:** `feb_radio_kill_switch_toggle()` [heltec/main/main.c:4419-4428](../heltec/main/main.c#L4419-L4428)
  runs on the `radio_ks` task ([radio_killswitch.c:141,150](../heltec/main/radio_killswitch.c#L141))
  and calls `wardriving_stop_internal()` ([main.c:2511-2525](../heltec/main/main.c#L2511-L2525)),
  whose own comment says "Runs on the NimBLE host task only". That function `free()`s and NULLs
  `wardriving_cluster_flush_records` without a lock. Meanwhile `wifi_scan_done_cb()`
  ([main.c:1323-1336](../heltec/main/main.c#L1323-L1336)) on the host task memcpy's into it under
  `cluster_link_spinlock`, then reads `raw = wardriving_cluster_flush_records` and iterates
  `raw[k]` **outside** the lock. `ble_npl_callout_stop()` does not stop an in-flight callback.
- **Failure:** touching the pad during cluster-delegated wardriving gives a NULL/freed deref, then
  a `LoadProhibited` panic or corrupted wardriving records. This is a direct counter-example to
  BL27's claim that "only plain bool/uint32 flags are raced".
- **Fix (0 B DRAM):** take the free+NULL and the pointer read under `cluster_link_spinlock`, with
  the `free()` itself done after exiting the critical section on a local copy. Or defer the
  free to the host task via a flag checked at the next `wifi_scan_done_cb()`/stop. Also audit
  every other `wardriving_stop_internal()` side effect for the same off-task assumption (callout
  stop, `cluster_link_send_scan_config()` UART writes racing the host task's own sends).

### HP-05 — Heltec kill-switch OFF→ON leaves Wi-Fi permanently dead until power cycle
- **Where:** the enable branch at [heltec/main/main.c:4487](../heltec/main/main.c#L4487) calls
  `start_wifi_subsystem()` ([main.c:4298-4337](../heltec/main/main.c#L4298-L4337)). The disable
  branch only calls `esp_wifi_stop()`.
- **What:** the second `esp_event_loop_create_default()` returns `ESP_ERR_INVALID_STATE` (checked
  against the pinned IDF's `default_event_loop.c`). The function logs it and returns before
  `esp_wifi_init/set_mode/start`, and because it is `void` the caller persists `enabled=true` and
  logs "restarting". BLE comes back; Wi-Fi scanning and wardriving Wi-Fi never do. Nothing on the
  OLED or the wire shows it.
- **Fix:** split `start_wifi_subsystem()` into a once-at-boot init (netif, event loop, default
  STA netif, `esp_wifi_init`, handler registration) and a restartable `esp_wifi_start()`. The
  re-enable path calls only the latter. Return `esp_err_t` so the toggle can report failure. This
  also removes the duplicate-netif and duplicate-handler-registration leaks that would otherwise
  appear once the early return is gone (HP-29).

### HP-09 — Publish leaves the CSV / mesh files open → CLI hangs
- **Where:** `publish_start()` [flipper/flipper_esp32_over_ble.c:4163-4196](../flipper/flipper_esp32_over_ble.c#L4163-L4196)
  calls `stop_ble_profile()` but never `wardriving_csv_close()`/`mesh_log_close()` (only reached
  via [:5567](../flipper/flipper_esp32_over_ble.c#L5567)). Afterwards `app.profile == NULL`, so
  the main loop's `if(app.profile)` BtStatus branch never runs the reset that would close them.
- **Failure:** wardrive (or let a Heltec drain mesh records), go Home, press Publish. The script's
  `storage read_chunks` hits `FSE_ALREADY_OPEN` and the storage service waits on the file-close
  flag with `FuriWaitForever`. The script times out, the fallback result write hits the same
  wedged CLI, and the Flipper shows Timeout after 180 s. This is the normal user flow.
- **Fix:** in `publish_start()`, under `wardriving_state_mutex`, close both files right after
  `stop_ble_profile()`.

---

## 3. P1 — next hardening batch

### HP-06 — Flipper AES-GCM nonce/AAD scratch shared across threads (security)
- **Where:** static `session_aad_buf`/`session_nonce_buf`/`session_tag_buf`
  [flipper/session.c:478-486](../flipper/session.c#L478-L486). Encrypt runs on the main thread
  (e.g. `send_gps_command()` from `AppEventGpsPollTick`,
  [flipper_esp32_over_ble.c:5913](../flipper/flipper_esp32_over_ble.c#L5913); wardriving
  start/stop; scans). Decrypt runs on BleEventWorker
  ([:4308](../flipper/flipper_esp32_over_ble.c#L4308)), for records the ESP32 pushes unsolicited
  (backlog drain, `mesh_log`).
- **What:** the comment's premise ("never simultaneously encrypted and decrypted") is false.
  Preemption between `build_nonce` and `furi_hal_crypto_gcm` lets the encrypt pick up the peer's
  `{sid, dir=0x01, seq_in}` nonce, which the ESP32 already used under the same key. Two
  ciphertexts under one (key, nonce) leak P1⊕P2 and enough to forge GHASH. The record is then
  rejected, but it has already been transmitted.
- **Fix:** per-direction scratch (+~60 B `.bss`, acceptable) or a session mutex around
  encrypt/decrypt. The mutex is the better choice because it also covers G10. Add a guard in
  `feb_session_encrypt_record` that refuses `direction != flipper→esp` so a nonce can never be
  built for the wrong direction.

### HP-07 — Flipper main-thread resets race BleEventWorker's session/pairing ceremony
- **Where:** `pairing_reset_state()`/`session_reset_state()` called from the main loop's
  `BtStatusConnected`/`BtStatusAdvertising` handler
  ([flipper_esp32_over_ble.c:5717-5733](../flipper/flipper_esp32_over_ble.c#L5717-L5733)) and from
  `stop_service()`, while the same statics (`pair_secret`, `pair_transcript`, `session_key`,
  `session_stage` …) are advanced by BleEventWorker. See the single-thread claim at
  [:686-689](../flipper/flipper_esp32_over_ble.c#L686-L689).
- **Failure:** the main thread is blocked in SD I/O (settings save, CSV close, mesh reload) while
  a fast reconnect completes `hello`→`hello_ack`→`client_auth` on BleEventWorker. The
  earlier-queued `Connected` event is then processed and wipes the live session, which looks like a
  spurious auth or sequence failure followed by a disconnect. This is a plausible contributor to
  BL06/G36-style "doesn't reconnect" reports.
- **Fix:** do the protocol-state reset on BleEventWorker (in the GATT connect/disconnect
  callback) and let the main thread reset only UI state. Or tag each state with a connection
  generation counter and have the main thread reset only if the generation still matches the one
  in the event. This is the G10 fix, generalized.

### HP-08 — Per-item event posting overflows the 8-deep app queue
- **Where:** `handle_wifi_scan_status()`'s loop → `post_wifi_scan_ap()` (and the BLE equivalent)
  [flipper_esp32_over_ble.c:2316, 2355-2370](../flipper/flipper_esp32_over_ble.c#L2316). The queue
  is `furi_message_queue_alloc(8, …)` ([:5631](../flipper/flipper_esp32_over_ble.c#L5631)) and up to
  32 items are posted per record, with timeout 0. None of the ~18 `furi_message_queue_put` calls in
  the file check the return value.
- **Failure:** scan lists are silently truncated. Worse, `AppEventSessionFatal`/`AppEventBtStatus`
  posted in the same burst are dropped, leaving the UI "connected" to a torn-down session. The
  code's own comment at [:362-373](../flipper/flipper_esp32_over_ble.c#L362-L373) acknowledges the
  risk, but it was never put in the backlog.
- **Fix:** batch one event per `status` record, the same pattern already used for wardriving
  (copy into the existing `wifi_scan_aps`/`ble_scan_devices` arrays under a lock, post one
  "results updated" event). Add a helper that logs and counts failed puts, and use a non-zero
  timeout or a reserved slot for fatal/status events.

### HP-10 / HP-11 — Publish archive step is unsafe (data loss / false failure)
- **Where:** [scripts/publish_wardriving.ps1](../scripts/publish_wardriving.ps1) rename at ~:313-322,
  :595, :647 (10 s `Read-FlipperUntil` default at :123). Flipper restarts BLE on timeout/cancel in
  `publish_finish_waiting()` ([flipper_esp32_over_ble.c:4104-4130](../flipper/flipper_esp32_over_ble.c#L4104)).
- **HP-10:** `storage rename` copies and then deletes the file *as it is at rename time*. If the
  Flipper has already given up (Back, or the 180 s timeout) and restarted BLE, newly drained rows
  land in `wardriving_current.csv`. The blocked rename resumes once the file closes and archives
  them, even though they were never uploaded, and the ESP32 has already marked them drained.
- **HP-11:** the rename copies in 512 B chunks, so a multi-MB CSV (H04 records 3.7 MB) plausibly
  exceeds 10 s. A successful upload is then reported as failed, and the leftover prompt desyncs
  every later CLI read.
- **Fix:** stop renaming. Write the archive from the local uploaded snapshot (`write_chunk`),
  `stat` the live file, and `remove` it only if its size still equals the uploaded size.
  `remove` fails fast with ALREADY_OPEN instead of blocking. Scale CLI timeouts with file size. On
  the Flipper side, don't restart BLE while the host script may still be mid-operation: extend the
  timeout, or require a result file before resuming.

### HP-12 — Enable stack overflow detection on every ESP target
- **Where:** generated `sdkconfig` for esp32/esp32c5/heltec: `CONFIG_COMPILER_STACK_CHECK_MODE_NONE=y`,
  not overridden in any `sdkconfig.defaults`. `CONFIG_FREERTOS_CHECK_STACKOVERFLOW` should also be
  checked.
- **Why:** this project's most repeated bug class is task stack overflow (SESSION_MEMORY
  "steps 3, 5, 7, wifi_scan"; G25; HP-15). Without a canary, the next one shows up as silent
  corruption rather than a clean abort.
- **Fix:** `CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y` and `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y`
  in all three `sdkconfig.defaults`. Remember that sdkconfig defaults aren't retroactive
  (LESSONS.md): delete `sdkconfig` and rebuild. **Heltec:** measure IRAM/DRAM impact first (45 B
  / 8 B headroom). If it doesn't fit, do HP-14 first to free space. Consider
  `CONFIG_ESP_TASK_WDT_PANIC=y` in the same pass (HP-27).

### HP-13 — Factory reset erases NVS underneath host-task NVS writers
- **Where:** `perform_factory_reset()` [esp32/main/factory_reset.c:33-53](../esp32/main/factory_reset.c#L33-L53)
  (same shape in `heltec/main/factory_reset.c`) runs on its own polling task and calls
  `nvs_flash_erase()` (which de-inits the partition) + `nvs_flash_init()`. Nothing coordinates it
  with `persist_pairing_secret()`/`wardriving_persist_save()` or the Heltec kill-switch persist,
  which run on the NimBLE host task and, on the Heltec, the `radio_ks` task.
- **Failure:** a 5 s hold that completes during a wardriving start/stop persist or at pairing
  completion leaves a stale handle or a torn write. `esp_restart()` follows immediately, so the
  window is small. Worst case is a panic instead of a clean reset, or a half-written blob that
  survives because the erase raced.
- **Fix:** post the reset request to the host task as a `ble_npl_event` (the pattern
  `wardriving_button_toggle_ev` already uses) so it's serialized with every NVS writer.

### HP-14 — ESP images still `-Og`
- **Where:** `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` in all three generated sdkconfigs. This is
  prior review finding CODE_REVIEW_FINDINGS #14, still open.
- **Why now:** the Flipper just recovered 11.8 KB by moving to `-Os`. On the Heltec, IRAM
  (45 B) and flash-resident code would both shrink, and that headroom is exactly what HP-12, HP-04's
  and HP-05's fixes, and the next capability need. The C5 app partition is also 85% full.
- **Fix:** `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` in each `sdkconfig.defaults`; record the
  before/after `idf.py size` in BASELINES.md. Keep assertions enabled.

### HP-15 — Measure the Heltec LoRa RX task stack
- **Where:** `lora_shared_radio_task` (4096 B, [lora_shared_radio.cpp:71](../heltec/main/lora_shared_radio.cpp#L71))
  → `lora_handle_meshtastic_frame()` → `meshtastic_proto_parse()` →
  `decrypt_default_channel_payload()` ([meshtastic_proto.c:326-396](../heltec/main/meshtastic_proto.c#L326-L396):
  240 B plaintext + mbedTLS AES context + nonce/stream blocks) → protobuf decode, then
  `mesh_log_record_sighting()`'s on-stack CBOR scratch, plus float-formatting `ESP_LOG` calls on
  the same chain (BL21 diagnostics).
- **Fix:** log `uxTaskGetStackHighWaterMark()` once per N frames in a test build (0 B `.bss`), or
  do a `-fstack-usage` build. Remove the BL21 `diag:` logs, which is free. If headroom is under
  ~512 B, make the plaintext buffer the one shared static used across the task.

### HP-16 — BadUSB bootstrap fails under default execution policy
- **Where:** typed command [flipper_esp32_over_ble.c:3871-3875, 3936](../flipper/flipper_esp32_over_ble.c#L3871).
- **What:** the Windows client default policy is `Restricted`, so `& "$env:TEMP\publish_wardriving.ps1"`
  is refused and the user sees a Timeout after 180 s. USER_GUIDE.md promises the flow works on
  "a different PC". It only worked on the dev machine because that machine has a relaxed policy.
- **Fix:** type `powershell -NoProfile -ExecutionPolicy Bypass` into Win+R. In the same edit, fix
  HP-31 (`-ErrorAction Stop` + `if ($?)`). Changing the typed string means bumping the pinned
  script commit, so do it in one pass.

### HP-17 — Result file can't be parsed reliably by the FAP
- **Where:** `$resultFields = @{}` [publish_wardriving.ps1:523](../scripts/publish_wardriving.ps1#L523);
  raw server bodies in `message`/`mesh_message` (:506, :604, :670); the FAP reads 512 B
  ([flipper_esp32_over_ble.c:4041-4078](../flipper/flipper_esp32_over_ble.c#L4041)).
- **Failure:** `status=` falls after byte 512, or a multi-KB proxy HTML body pushes it out, so the
  Flipper shows "Unrecognized result file"/Fail for a successful and already-archived upload.
- **Fix:** `[ordered]@{}` with `status` first; strip CR/LF and cap messages at ~90 chars (the FAP
  buffer is 96).

### HP-18 — Port discovery resets dev boards
- **Where:** [publish_wardriving.ps1:51-64](../scripts/publish_wardriving.ps1#L51-L64) opens every
  COM port with `DtrEnable=$true`.
- **Why it matters here:** the C6 (USB-Serial-JTAG) and Heltec (CP210x) reset on open, and
  resetting the Heltec has already wiped live state (this is in the project's own feedback notes).
  Bluetooth virtual COM ports can also block `Open()` for seconds each.
- **Fix:** filter candidates by the Flipper's USB VID `0483` via `Win32_PnPEntity` before opening.

### HP-19 — Extend `check_shared_headers.py` to per-board copies
- **Where:** [tools/check_shared_headers.py:28-43](../tools/check_shared_headers.py#L28-L43) only
  pairs `components/feb_protocol` ↔ `flipper/`.
- **What it misses:** `wardriving_{validate,dedup,log,record_format,persist}`, `nmea_parser`, and
  `location` across esp32/esp32c5/heltec; any `.c` logic; multi-line `#define`s and struct bodies.
  HP-02 (the retry-floor constant exists only in C5's `wardriving_validate.h`) is a live miss.
- **Fix:** add per-board triples, with hash equality for the byte-identical `.c` modules and an
  allowlist for intended per-board constants (pins). Longer term, move the byte-identical modules
  into a shared component (`components/feb_wardriving/`) so drift is impossible. The port-drift
  review found them currently identical, so this is a mechanical move.

### HP-20 — Host coverage for Flipper mesh decoders
- **Where:** [tests/flipper/build.ps1:34-44](../tests/flipper/build.ps1#L34-L44) doesn't compile
  `flipper/cbor_meshcore.c`, `flipper/cbor_mesh_log.c`, or `flipper/mesh_nodes.c`. The existing
  `FEB_VEC_MESHCORE_*`/`FEB_VEC_MESH_LOG_*` vectors are only used by the ESP32 suite.
- **Why:** these are the production decoders for ESP→Flipper traffic. The Flipper meshcore
  decoder is an independent rewrite that infers optional fields from map count
  (`flipper/cbor_meshcore.c:114-116`). The project already tracks this parallel-codec drift
  pattern.
- **Fix:** add all three to the build, decode the shared vectors, and add a `mesh_nodes` line
  round-trip test in the exact format the publish script parses.

### HP-21 — C5 dual-band scan heap pressure (BL18 hypothesis)
- The app's AP array is correctly bounded (`FEB_WIFI_SCAN_RAW_MAX=64`), but the IDF driver's
  internal AP list scales with the number of APs seen, and `WIFI_BAND_MODE_AUTO` sees more. When
  BL18 is next chased, log `esp_get_free_heap_size()` and `heap_caps_get_largest_free_block()`
  before and during a dense-area scan. Also consider `CONFIG_ESP_WIFI_SCAN_AP_NUM` / a
  `wifi_scan_config_t` that limits results.

---

## 4. P2 — low severity / defense in depth

| ID | Where | Issue | Fix |
| --- | --- | --- | --- |
| HP-22 | [esp32/main/main.c:2415-2496](../esp32/main/main.c#L2415) (and ports) | `wardriving_send_backlog_count_update()` ignores the send result, but `wardriving_last_reported_backlog` advances anyway, so the Flipper's backlog count stays stale after a full-FIFO drop | Return `bool`; advance only on success |
| HP-23 | [esp32/main/nmea_parser.c:258-316](../esp32/main/nmea_parser.c#L258) (identical in all 3) | No range check on RMC date/time fields, so a checksum-valid but insane value becomes a bogus `utc_timestamp_s` baked into records | Reject hh≥24, mm/ss≥60, day∉1..31, month∉1..12 |
| HP-24 | Flipper [framing reassembly](../flipper/flipper_esp32_over_ble.c#L4420) | `feb_reassembly_reset()` runs only in `profile_start()`, not per connection; the ESP's `tx_message_id` is a never-reset `uint8_t`, so after an ESP reboot a stale partial can collide 1/256 (bounded by the reassembly timeout timer) | Reset under `reassembly_mutex` on every connect |
| HP-25 | `components/feb_protocol/cbor_primitives.c:330,357`, `cbor_records.c:190,325` + Flipper mirrors | 64-bit CBOR heads truncated to 32-bit `size_t`/`version`; non-canonical acceptance on device (not on 64-bit host tests); a MITM can re-encode `version` and the AAD still matches | Reject values > `SIZE_MAX`/`UINT32_MAX` with `TOO_LARGE`; add vector |
| HP-26 | `flipper/cbor_records.c:696,800` vs `components/.../cbor_records.c:940,1098` | Flipper accepts non-map `arguments`/`result`, the ESP requires a map (lockstep violation, not exploitable today) | Mirror the major-type-5 check; add vector |
| HP-27 | all ESP sdkconfig | `CONFIG_ESP_TASK_WDT_PANIC` unset, so a genuine starvation (e.g. HP-02) only logs | Enable with HP-12 |
| HP-28 | decrypted-payload decoders, both sides | Trailing bytes after decrypted plaintext are not rejected; PROTOCOL.md's "exact span" rationale doesn't hold for protected records | `skip_value` must consume exactly `plaintext_len`; update PROTOCOL.md |
| HP-29 | [heltec/main/main.c:4311](../heltec/main/main.c#L4311) | `esp_netif_create_default_wifi_sta()` handle discarded, handler registration repeated; leaks per kill-switch cycle once HP-05's early return is removed | Covered by HP-05's init/start split |
| HP-30 | [components/feb_cluster_link/cluster_link.c:184-204](../components/feb_cluster_link/cluster_link.c#L184-L204) | A corrupted length swallows up to 514 B before CRC fail, and the swallowed bytes are never re-scanned for SOF, so one UART overrun can drop several frames; the header comment claims otherwise; `cluster_link.h:22` CRC-order note is stale | Re-feed bytes after the rejected SOF, or document the cost in CLUSTER.md; fix comments |
| HP-31 | [flipper_esp32_over_ble.c:3874-3875](../flipper/flipper_esp32_over_ble.c#L3874) | `iwr …; & X` runs a stale local script if the download fails | `-ErrorAction Stop; if ($?)`, delete X first (with HP-16) |
| HP-32 | publish_wardriving.ps1:553-557, :542, :697 | API key echoed by `Read-Host`, saved before validation, never cleared on 401, plaintext temp copy survives a closed window | `-AsSecureString`, 64-hex check, persist only after 200/202, clear on 401 |
| HP-33 | publish_wardriving.ps1:368-371, :470-473 | `HttpWebRequest` follows redirects carrying `X-API-Key` | `AllowAutoRedirect=$false`; treat 3xx as fail |
| HP-34 | flipper_esp32_over_ble.c:3885-3891 | BadUSB typing assumes a US layout (host locale here is BG) | Document "US layout required", or ALT-code typing |
| HP-35 | publish_wardriving.ps1:623, :686 | A mesh-read serial failure after CSV success leaves `status=ok` with no `mesh_status` (BL26-adjacent) | Outer catch sets `mesh_status=fail` |
| HP-36 | publish_wardriving.ps1:458-461 | Method 2 HMAC keyed on UTF-8 of the hex key; hex-decoded bytes may be expected | Confirm on first live Method 2 run |
| HP-37 | `flipper/session.c:326-331`, `flipper/pairing.c:674-679` | Proof/confirm helpers clamp over-length transcripts while the ESP zeroes output (CODE_REVIEW #4 scope, unreachable today) | Zero like the ESP (decision D4) |
| HP-38 | `components/feb_protocol/cbor_ble_scan.c:15`, `cbor_wardriving.c:487-496` | The ESP doesn't bound BLE `name` ≤31 on encode but the Flipper rejects it on decode, so a whole batch would drop (clamped upstream today) | Mirror the Flipper's check |
| HP-39 | session nonce build, both sides | 24-bit sequence cap enforced only in app code; no test that seq and seq+2^24 alias | Enforce in `feb_session_encrypt_record`; add test |
| HP-40 | [esp32/main/main.c](../esp32/main/main.c) `wardriving_send_next_batch()` | Duplicate, wrong `"failed to build…"` log after a queue failure (the ports already fixed it) | Delete stray `ESP_LOGE` |
| HP-41 | tools/monitor_flipper_log.py:132-156 | No `finally: ser.close()` / encode guard, so it loops on "Access denied" after one exception | Copy `monitor_esp32_raw.py`'s handling |
| HP-42 | tools/build_*.ps1, flash_esp32.ps1 | `$ErrorActionPreference="Stop"` + `2>&1` on native tools makes a stderr warning a false build failure under PS 5.1 | Scope `Continue` around native calls |
| HP-43 | Cluster worker `handle_scan_done()` ([esp32/cluster_worker/main/main.c:226-236, 428](../esp32/cluster_worker/main/main.c#L226)) | 0-byte UART TX ring means per-AP blocking writes (~hundreds of ms per batch) that delay `scan_config_set` handling | Give the UART a TX ring buffer (Phase 9 work) |
| HP-44 | `components/feb_protocol/pairing_crypto.c:24-27` | ESP X25519 via `mbedtls_mpi` `exp_mod` is not constant-time (self-flagged in source, no backlog row). Low risk, since the ceremony is a one-time, reset-gated event | Add a BACKLOG row; consider mbedTLS's ECP X25519 (`mbedtls_ecp_mul` on Curve25519) |

---

## 5. New evidence against already-tracked items

| Known ID | Update |
| --- | --- |
| **H01** | Still open on the C6 at [esp32/main/main.c:2069-2115](../esp32/main/main.c#L2069): only the `BLE_HS_EBUSY` backoff exists, and the "connect in flight" flag is not implemented. HP-01 means the C5 is additionally missing the *prior* centralized guard. |
| **G10** | Sharpened by HP-06 (GCM scratch → nonce reuse) and HP-07 (main-thread resets of pairing/session statics). A single session mutex (or moving all protocol-state mutation onto BleEventWorker) closes all three. |
| **G18** | Appears **fixed in the working tree**: `flipper/pairing_crypto.c` now heap-allocates the donna pool and zeroizes it before free (~:853), but BACKLOG.md still lists it as Open. Close it when that edit is committed. |
| **G25** | Still present: `uint8_t buffer[256]` in `BLE_GAP_EVENT_NOTIFY_RX` ([esp32/main/main.c:3600](../esp32/main/main.c#L3600)). HP-12 at least makes a regression fail loudly. |
| **BL27** | HP-04 is a concrete counter-example to its "only bool/uint32 flags are raced" justification, and HP-05 shows the re-enable path was never exercised. Its hardware test should explicitly include an OFF→ON→`wifi_scan` cycle and a touch during cluster-delegated wardriving. |
| **BL18** | HP-21 (dual-band heap) is a measurable hypothesis. HP-02's zero-delay refire (which the C5 had before its own fix) is a confirmed watchdog mechanism on the same board, and could explain the earlier sighting if a no-fix session with 0 s cooldown was involved. |
| **BL21** | Removing the `diag:` logs is now also a stack-margin fix (HP-15). |
| **CODE_REVIEW #14** | Still open → HP-14. |
| **CODE_REVIEW #5, #24** | Verified fixed (framing flags/capacity converged; ESP `full[]` zeroized). |
| **H05 note** | "Not yet done for esp32/heltec" about `FEB_WRITE_CHAR_MAX_LEN` is stale: all three boards now use the shared `framing.h` constant. |
| **Backlog hygiene** | The Flipper's own code comment ([:362-373](../flipper/flipper_esp32_over_ble.c#L362)) acknowledged HP-08 without a backlog row. Promote such "latent risk" comments to rows when they're written. |

---

## 6. Reviewed and found sound (don't re-review without cause)

- **Attacker-controlled RF parsing** (MeshCore ADVERT, Meshtastic header/AES-CTR/protobuf): every
  length/offset read is bounds-checked; lat/lon widened before scaling. No OOB found.
- **Flipper storage:** `board_id` restricted to `[A-Za-z0-9_-]` before any path build, so no
  traversal. Pairing/capability/settings writes are temp→sync→rename with cleanup. WiGLE CSV
  sanitizer + RFC-4180 quoting is correct and worst-case sized (no CSV injection).
- **Crypto hygiene:** constant-time compares on every tag/proof/confirm; `volatile` zeroization;
  RFC 7748 clamping; plaintext zeroized on decrypt failure; HKDF salts/info/labels and nonce
  layout match PROTOCOL.md on both sides.
- **Framing/CBOR bounds:** fragment index/count/length validated before any write; array/map
  counts checked against `MAX` before fill loops; nesting depth capped.
- **Shared ESP modules** (`wardriving_*`, `nmea_parser`): byte-identical across all three boards
  today.
- **Wardriving flash log and dedup, mesh_log mutex discipline and malloc-failure degrade, OLED I2C
  error handling, SX1276 IRAM ISR, partition tables:** no defects found.
- **Publish supply chain:** bootstrap pins a full 40-char commit hash, the pinned script matches
  HEAD's `scripts/`, and no file or CSV content is interpolated into commands. A truncated serial
  read throws rather than uploading partial data.

---

## 7. Suggested execution order

Each batch is one firmware's worth of work, delegated to that board's developer agent, and
follows the normal rules: build both sides for any wire change, run host suites, and update
USER_GUIDE.md via Haiku where behaviour it describes changes (HP-05, HP-09, HP-16).

1. **Immediately, before anyone commits the in-flight C6/Heltec edits:** HP-02 and HP-03 (fix
   the uncommitted edits); HP-01 (C5 one-liner). Write the LESSONS.md entry and update the agent
   definitions in the same pass.
2. **Heltec batch:** HP-14 first (frees IRAM/DRAM), then HP-05 + HP-29, HP-04, HP-12, HP-15
   measurement, remove the BL21 logs. Hardware: kill-switch OFF→ON→scan, touch during
   cluster-delegated wardriving, check stack high-water marks.
3. **Flipper batch:** HP-09, HP-08, then one session mutex covering HP-06/HP-07/G10, then HP-24.
   Re-measure `arm-none-eabi-size` (H04 discipline). Hardware: GPS screen polling during a backlog
   drain; wardrive → Publish without restarting the app.
4. **Publish script + bootstrap:** HP-16/HP-31 (one pinned-commit bump), HP-10/HP-11, HP-17,
   HP-18, HP-32, HP-33, HP-35. Test on a Windows account with default execution policy.
5. **All-ESP config pass:** HP-12, HP-14, HP-27 on C6 and C5 (Heltec done in batch 2), plus HP-13,
   HP-22, HP-23, HP-40.
6. **Shared-layer lockstep pass:** HP-25, HP-26, HP-28, HP-37, HP-38, HP-39 with vectors (these
   are wire-strictness changes, so both sides build together), plus HP-20 and HP-19 tooling.
7. **Deferred / Phase-bound:** HP-21 (with the BL18 hardware session), HP-30 and HP-43 (with Phase
   9 work), HP-34, HP-36, HP-44.

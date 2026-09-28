# Implementation plan — CSV row count, flush-gate redesign, Heltec mesh-dedup shrink

Written 2026-09-28 for an implementing agent. It has three independent parts (A, B, C). Each one can be done, built and committed separately. Delete or archive this file once all three have landed (see CLAUDE.md: SESSION_MEMORY/PLAN must not collect narrative, and finished work goes to PROJECT_HISTORY.md).

**Ground rules for the implementer**
- Line numbers below are approximate. The working tree has uncommitted edits, possibly from a concurrent session. **Always find code by the quoted grep anchor, not by line number.**
- Do not flash hardware. Build only.
- Firmware style: comment-sparse. Add at most one short comment per new block, and don't copy the long historical comments that are already there.
- Parts B and C touch ESP32 boards. Part B's shared files must stay in lockstep across `esp32/`, `esp32c5/` and `heltec/`. Run `python tools/check_shared_headers.py` and it must report no MISMATCH.
- USER_GUIDE.md sync: delegate it to a Haiku-model agent (CLAUDE.md rule). Don't edit it inline.

---

## Part A — Flipper: seed "Recs" from the CSV's real row count, cheaply

### Goal
The Running Wardriving screen's `Recs: N` should show the number of data rows in `wardriving_current.csv`, which is everything captured since the last publish. It should count up as rows are written.

To get that cheaply:
- **Cost at app start (normal case):** one `storage_common_stat()`. The file itself is not read.
- **Full chunked recount:** only when the saved size doesn't match the real file (crash, hand edit, publish archived it, or an upgrade from a build without the saved values).
- **Memory:** net 256 bytes less `.bss` than today. Two 256-byte statics are merged into one, and three `uint32_t` are added.

### Semantics change (intended)
Today `Recs` = records received from the ESP32 this session (`batch_count`, counted before the Flipper-side dedup). It resets on every fresh "started" ack and on every disconnect. After this change, `Recs` = rows actually in the CSV file. It only goes up when a row is really written, never resets on start/disconnect, and drops to 0 only when the file is archived or deleted.

### Background facts (verified)
- File: `wardriving/wardriving_current.csv` under the app data dir. The name comes from the `FEB_WARDRIVING_CSV_FILENAME` define in `flipper/flipper_esp32_over_ble.c`.
- **The header is two lines** (WigleWifi metadata line plus column-name line, see `flipper/wardriving_csv.c:12-15`). So rows = newline count − 2, floored at 0.
- The file is opened lazily for append (`wardriving_csv_ensure_open`) and kept open for the whole BLE session. It's closed only in `wardriving_csv_close()`, which is called from `publish_start()` and `reset_scan_ui_state_impl()`, both on the main thread.
- **Never open or stat the CSV while `wardriving_csv_file != NULL`.** If `storage_file_open()` hits `FSE_ALREADY_OPEN`, it waits forever (Unleashed `storage_external_api.c:106`).
- `wardriving_state_mutex` guards all CSV state. It is allocated in `main` *after* `wardriving_settings_load(&app)` (anchor `wardriving_state_mutex = furi_mutex_alloc`).
- Settings live in `wardriving_settings.txt`: flat `key=value\n`, parsed in `wardriving_settings_load()` and written atomically by `wardriving_settings_save()` (temp file then rename).
- `storage_common_stat(Storage*, const char*, FileInfo*)` is in the pinned ABI (`api_symbols.csv:3562`). `FileInfo.size` is a `uint64_t`.
- The host publish script archives or removes the live CSV only after a confirmed upload (`scripts/publish_wardriving.ps1`, `Complete-FlipperArchive`). The FAP closes the CSV in `publish_start()` and resumes BLE in `publish_finish_waiting()` via `start_profile(app)`.
- The main thread stack is 4 KB (`flipper/application.fam`). Don't add large stack buffers.

### Steps (all in `flipper/flipper_esp32_over_ble.c`)

**A1. Merge the two settings buffers into one shared file-scope buffer.**
- `wardriving_settings_load()` has `static char buf[FEB_WARDRIVING_SETTINGS_MAX_LEN];` and `wardriving_settings_save()` has another one with the same name and size.
- Replace both with a single file-scope `static char wardriving_settings_buf[FEB_WARDRIVING_SETTINGS_MAX_LEN];` declared just above `wardriving_settings_set_defaults`. Use it in both functions.
- Safe because both run only on the main thread and never nest. This saves 256 B, and the recount in A6 reuses this buffer.

**A2. Add three statics** next to `static File* wardriving_csv_file;` (anchor):
```c
static uint32_t wardriving_csv_row_count;   /* live data rows in the CSV; wardriving_state_mutex */
static uint32_t wardriving_csv_saved_rows;  /* pair persisted in wardriving_settings.txt, */
static uint32_t wardriving_csv_saved_size;  /* snapshotted only at close */
```
Settings load runs before these statics' declaration point in the file, so either declare them above `wardriving_settings_set_defaults`, or add forward declarations there. Keep them together.

**A3. Settings load/save: two new keys.**
- `wardriving_settings_set_defaults()`: set `wardriving_csv_saved_rows = 0; wardriving_csv_saved_size = UINT32_MAX;`. The UINT32_MAX sentinel means "unknown" and forces one recount for users upgrading from a file without these keys.
- `wardriving_settings_load()` parser chain: add
  `else if(text_matches(line, key_len, "csv_rows")) wardriving_csv_saved_rows = publish_parse_uint(value, value_len);`
  and the same for `"csv_size"` into `wardriving_csv_saved_size`.
  `publish_parse_uint` is defined later in the file (anchor `static uint32_t publish_parse_uint`), so add a forward declaration above `wardriving_settings_load`.
- `wardriving_settings_save()`: append `csv_rows=%lu\ncsv_size=%lu\n` to the format string, passing `(unsigned long)wardriving_csv_saved_rows, (unsigned long)wardriving_csv_saved_size`. The buffer is 256 B and the current content is about 120 chars, so it fits. The existing overflow check still guards it.
- Save must write the **saved** pair, never the live `wardriving_csv_row_count`. A settings change in the middle of a session must not persist a live count next to a stale size.

**A4. Keep the live count up to date (BLE thread, already under the mutex).**
- In `wardriving_csv_ensure_open()`, inside the `if(storage_file_size(file) == 0)` branch, after the header write succeeds, set `wardriving_csv_row_count = 0;`. A new file has no rows.
- In `wardriving_csv_write_record()`, right after the successful `storage_file_write(...) != row_len` check (before the `wardriving_csv_records_since_sync++` line), add `wardriving_csv_row_count++;`.

**A5. Snapshot at close and persist.** Change `static void wardriving_csv_close(void)` to `static void wardriving_csv_close(Esp32App* app)` and update both callers: `publish_start()` and `reset_scan_ui_state_impl()`; grep `wardriving_csv_close(` for both. Body:
```c
bool persist = false;
furi_mutex_acquire(wardriving_state_mutex, FuriWaitForever);
if(wardriving_csv_file) {
    storage_file_sync(wardriving_csv_file);
    wardriving_csv_saved_size = (uint32_t)storage_file_size(wardriving_csv_file);
    wardriving_csv_saved_rows = wardriving_csv_row_count;
    persist = true;
    storage_file_close(wardriving_csv_file);
    storage_file_free(wardriving_csv_file);
    wardriving_csv_file = NULL;
}
wardriving_csv_reset_state();
furi_mutex_release(wardriving_state_mutex);
if(persist) wardriving_settings_save(app);
```
- `storage_file_size` on an already-open handle costs nothing extra; it doesn't open the file again.
- Check that `app->storage` is still valid on the app-exit path. `stop_service()` reaches `reset_scan_ui_state_impl()`, and `furi_record_close(RECORD_STORAGE)` must come after it (it's at the end of `main`, anchor `furi_record_close(RECORD_STORAGE)`).
- `wardriving_csv_reset_state()` must **not** touch `wardriving_csv_row_count`.

**A6. Recount helper plus refresh (main thread only, CSV closed).** Add both functions after `wardriving_csv_ensure_open`:
```c
static uint32_t wardriving_csv_count_rows(Storage* storage, const char* path) {
    static bool low_heap_logged;
    uint32_t newlines = 0;
    if(!storage_open_heap_margin_ok("wardriving CSV count", &low_heap_logged)) return 0;
    uint32_t start = furi_get_tick();
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t n;
        while((n = storage_file_read(file, wardriving_settings_buf, sizeof(wardriving_settings_buf))) > 0) {
            for(size_t i = 0; i < n; i++) newlines += (wardriving_settings_buf[i] == '\n');
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    FURI_LOG_I(TAG, "wardriving CSV: recounted %lu rows in %lu ms",
               (unsigned long)(newlines > 2 ? newlines - 2 : 0), (unsigned long)(furi_get_tick() - start));
    return newlines > 2 ? newlines - 2 : 0;
}

static void wardriving_csv_count_refresh(Esp32App* app) {
    if(!build_wardriving_path(wardriving_csv_path, sizeof(wardriving_csv_path), FEB_WARDRIVING_CSV_FILENAME)) return;
    FileInfo info;
    FS_Error err = storage_common_stat(app->storage, wardriving_csv_path, &info);
    uint32_t rows;
    bool changed = false;
    if(err == FSE_NOT_EXIST) {
        rows = 0;
        changed = wardriving_csv_saved_rows != 0 || wardriving_csv_saved_size != 0;
        wardriving_csv_saved_rows = 0;
        wardriving_csv_saved_size = 0;
    } else if(err != FSE_OK) {
        FURI_LOG_W(TAG, "wardriving CSV: stat failed: %d", err);
        rows = wardriving_csv_saved_rows;
    } else if((uint32_t)info.size == wardriving_csv_saved_size) {
        rows = wardriving_csv_saved_rows;
    } else {
        rows = wardriving_csv_count_rows(app->storage, wardriving_csv_path);
        wardriving_csv_saved_rows = rows;
        wardriving_csv_saved_size = (uint32_t)info.size;
        changed = true;
    }
    furi_mutex_acquire(wardriving_state_mutex, FuriWaitForever);
    wardriving_csv_row_count = rows;
    furi_mutex_release(wardriving_state_mutex);
    app->wardriving_csv_rows = rows;
    if(changed) wardriving_settings_save(app);
}
```
- The `wardriving_settings_buf` reuse is safe: `wardriving_settings_save()` runs only after the read loop has finished.
- If the heap-margin check fails, the helper returns 0 and the count simply shows 0 until the next refresh. That's acceptable.
- `wardriving_csv_path` is already a file-scope static, so reuse it.

**A7. Call `wardriving_csv_count_refresh()` in exactly two places.**
1. In `main`, right after `wardriving_state_mutex = furi_mutex_alloc(...); furi_check(wardriving_state_mutex);`. That's before any `start_profile`, so the BLE thread can't have the file open yet.
2. In `publish_finish_waiting()`, **before** `start_profile(app)`. The CSV is still closed there (HP-09), so this is when an archived file is noticed and the count becomes 0.

**A8. Screen/event plumbing.**
- Rename the app field `wardriving_records_this_session` to `wardriving_csv_rows`. Grep the old name; there are about six sites.
- In the `AppEventWardrivingBatch` union struct, rename `batch_count` to `csv_rows`. In `post_wardriving_batch()`, rename that parameter.
- In `handle_wardriving_status()`, just before the `post_wardriving_batch(` call, take a snapshot:
  ```c
  furi_mutex_acquire(wardriving_state_mutex, FuriWaitForever);
  uint32_t csv_rows = wardriving_csv_row_count;
  furi_mutex_release(wardriving_state_mutex);
  ```
  Pass `csv_rows` instead of `(uint32_t)result->record_count`.
- Main loop `AppEventWardrivingBatch` handler: change the `+=` to an assignment, `app.wardriving_csv_rows = event.u.wardriving_batch.csv_rows;`.
- **Delete** `app.wardriving_records_this_session = 0;` in the `is_fresh_start` block (`AppEventWardrivingRunState` handler) and `app->wardriving_records_this_session = 0;` in `reset_scan_ui_state_impl()`. The file-backed count must survive a restart or disconnect.
- `draw_wardriving_running_screen()`: keep the `"Recs: %lu ..."` format strings as they are, but pass `app->wardriving_csv_rows`.

### A — Verification
- `tools/build_flipper.ps1` builds with no warnings (`-Werror`).
- `tests/flipper/build.ps1` host tests still pass. The codec isn't touched, so this is a sanity check.
- Hardware (user runs it):
  1. With an existing CSV, launch the app and open Wardriving Running. `Recs` should equal `(lines − 2)` of a copy pulled with `storage.py`. The log shows `recounted N rows in X ms` the first time. **Record X in docs/PROJECT_HISTORY.md**; that's the real measurement the user wanted.
  2. Exit and relaunch. There should be no `recounted` log line (saved size matched), and the same `Recs`.
  3. Capture a few records and confirm `Recs` rises by the number of rows actually written.
  4. Publish. After it finishes, `Recs` = 0.
- Docs: in `docs/WARDRIVING_REDESIGN.md` (around line 195, the `"Recs: N Backlog: M"` description), update the meaning of Recs. Update `docs/USER_GUIDE.md` via Haiku.

---

## Part B — ESP32 boards: flush gate driven by the last 3 Wi-Fi passes

### Goal (confirmed with the user)
Replace the "stopped ≥ 10 s below 5 km/h" rule with a window over the last 3 scans:
- A scan is **one Wi-Fi wardriving pass**. Its count is the number of **records actually appended to the flash log** (after ESP32 dedup) since the previous pass, Wi-Fi plus BLE.
- Sum of the last 3 counts **< 100** → gate opens (unpause). **> 100** → gate closes (pause). Exactly **100** → state unchanged.
- These existing rules still open the gate: **no GPS fix**, and **backlog > `FEB_WARDRIVING_FLUSH_BACKLOG_THRESHOLD` (2000)**. Note that the threshold is 2000, not 1000.
- The speed/stopped rule is **removed**.
- The gate still only decides whether a drain **starts**. A drain already running continues to empty, same as today.

### Defaults the implementer must apply (the user didn't specify these; they're chosen conservatively)
- **Initial state = closed** when a run starts, until the first full window. Until 3 passes exist, the state is simply left unchanged (still closed unless the other rules open it).
- **Wardriving not running** (neither `wardriving_wifi_active` nor `wardriving_ble_active`) → the window rule doesn't apply and the gate is **open**. There's no radio contention, so the unsolicited backlog drain at session start must still work while idle.
- **BLE-only run** (`wardriving_ble_active && !wardriving_wifi_active`) → no Wi-Fi passes happen, so push one sample every `FEB_WARDRIVING_FLUSH_BLE_WINDOWS_PER_SAMPLE` (10) BLE window closes, about 5 s at the default 500 ms interval.

### Cost
- About 9 bytes of `.bss` added per board, and the `int64_t wardriving_flush_stopped_since_us` (8 B) is removed, so the net is roughly +1 B. That matters on the Heltec, which has only about 40 B of DRAM headroom.
- O(1) CPU once per pass. No flash reads.

### Shared, host-testable helper — `wardriving_validate.h` / `.c` (all three boards)
`wardriving_validate.c` is **BOARD_IDENTICAL** (byte-identical ×3) and `.h` is **BOARD_EQUIVALENT** (see `tools/check_shared_headers.py`). Make the change once in `esp32/main/`, then copy both files to `esp32c5/main/` and `heltec/main/` unchanged.

In `wardriving_validate.h`, **delete** `FEB_WARDRIVING_FLUSH_STOPPED_SPEED_E1_KMH_MAX` and `FEB_WARDRIVING_FLUSH_STOPPED_SECONDS`, and rewrite the comment above them in 2-3 lines. Keep `FEB_WARDRIVING_FLUSH_BACKLOG_THRESHOLD`. Add:
```c
#define FEB_WARDRIVING_FLUSH_WINDOW_SCANS 3u
#define FEB_WARDRIVING_FLUSH_BUSY_RECORDS 100u
#define FEB_WARDRIVING_FLUSH_BLE_WINDOWS_PER_SAMPLE 10u

typedef struct {
    uint8_t counts[FEB_WARDRIVING_FLUSH_WINDOW_SCANS]; /* per-pass new records, saturated at 255 */
    uint8_t next;
    uint8_t filled;
    bool open;
} wardriving_flush_window_t;

void wardriving_flush_window_reset(wardriving_flush_window_t *w);
bool wardriving_flush_window_push(wardriving_flush_window_t *w, uint32_t new_records);
```
- Saturating at 255 loses nothing: a single pass of ≥ 100 already decides "busy".
- Match the prefix of the functions already in that header (`wardriving_resolve_start_intervals`, no `feb_`), so the checker's `feb_`-prototype regex behaves the same as today.

In `wardriving_validate.c`:
```c
void wardriving_flush_window_reset(wardriving_flush_window_t *w)
{
    memset(w, 0, sizeof(*w));
}

bool wardriving_flush_window_push(wardriving_flush_window_t *w, uint32_t new_records)
{
    uint32_t sum = 0;
    uint8_t i;

    w->counts[w->next] = (uint8_t)(new_records > 255u ? 255u : new_records);
    w->next = (uint8_t)((w->next + 1u) % FEB_WARDRIVING_FLUSH_WINDOW_SCANS);
    if (w->filled < FEB_WARDRIVING_FLUSH_WINDOW_SCANS) {
        w->filled++;
    }
    if (w->filled == FEB_WARDRIVING_FLUSH_WINDOW_SCANS) {
        for (i = 0; i < FEB_WARDRIVING_FLUSH_WINDOW_SCANS; i++) {
            sum += w->counts[i];
        }
        if (sum < FEB_WARDRIVING_FLUSH_BUSY_RECORDS) {
            w->open = true;
        } else if (sum > FEB_WARDRIVING_FLUSH_BUSY_RECORDS) {
            w->open = false;
        }
    }
    return w->open;
}
```
Add `#include <string.h>` if it isn't already there.

### Counting appended records — `wardriving_dedup.c/.h` (BOARD_IDENTICAL / BOARD_EQUIVALENT, all three boards)
`wardriving_dedup_and_maybe_append()` returns `true` both for "filtered" and for "appended", so callers can't tell the two apart. Don't change its signature. Instead add a monotonic counter:
- `.c`: `static uint16_t wd_appended_total;` and increment it right after the successful `wardriving_log_append(record)` inside `wardriving_dedup_and_maybe_append()`. Add `uint16_t wardriving_dedup_appended_total(void) { return wd_appended_total; }`.
- `.h`: declare `uint16_t wardriving_dedup_appended_total(void);`.
- Callers compute the delta as `(uint16_t)(now - last)`. That's wrap-safe; per-pass counts are far below 65535.

### Per-board `main.c` changes (esp32/main, esp32c5/main, heltec/main — same edit on each)
Use these anchors; line numbers differ per board.
1. **Statics.** Next to `static int64_t wardriving_flush_stopped_since_us = -1;`, **delete that variable** and add:
   ```c
   static wardriving_flush_window_t wardriving_flush_window;
   static uint16_t wardriving_flush_last_appended;
   static uint8_t wardriving_flush_ble_windows;
   ```
2. **Reset on run start.** At every `wardriving_wifi_active = true;` and at the BLE-start equivalent (grep `wardriving_ble_active = true`; the C6 has one Wi-Fi site, the Heltec has two), call a small static helper:
   ```c
   static void wardriving_flush_window_start(void)
   {
       wardriving_flush_window_reset(&wardriving_flush_window);
       wardriving_flush_last_appended = wardriving_dedup_appended_total();
       wardriving_flush_ble_windows = 0;
   }
   ```
   Call it once per start command. If both Wi-Fi and BLE start in the same command, calling it twice is harmless.
3. **Sample at the end of each Wi-Fi pass.** In `wifi_scan_done_cb()`'s wardriving branch, immediately before its `wardriving_maybe_kick_send(connection_handle);` (the one followed by `ble_npl_callout_reset(&wardriving_wifi_interval_co, ...)`), add:
   ```c
   {
       uint16_t now_total = wardriving_dedup_appended_total();
       wardriving_flush_window_push(&wardriving_flush_window, (uint16_t)(now_total - wardriving_flush_last_appended));
       wardriving_flush_last_appended = now_total;
   }
   ```
   This also runs when the pass had no fix and discarded its results; that pushes 0, which is fine because the no-fix rule opens the gate anyway.
4. **Sample in BLE-only runs.** In `ble_scan_window_close_cb()`'s wardriving branch, immediately before its `wardriving_maybe_kick_send(connection_handle);`, add:
   ```c
   if (!wardriving_wifi_active && ++wardriving_flush_ble_windows >= FEB_WARDRIVING_FLUSH_BLE_WINDOWS_PER_SAMPLE) {
       uint16_t now_total = wardriving_dedup_appended_total();
       wardriving_flush_window_push(&wardriving_flush_window, (uint16_t)(now_total - wardriving_flush_last_appended));
       wardriving_flush_last_appended = now_total;
       wardriving_flush_ble_windows = 0;
   }
   ```
5. **Gate.** In `wardriving_maybe_kick_send()`, delete the `if (loc_state == FEB_LOCATION_FIX) { ... wardriving_flush_stopped_since_us ... }` block and the `now_us` variable, then replace the `gate_open = ...` expression with:
   ```c
   gate_open = (loc_state != FEB_LOCATION_FIX) ||
               (pending > FEB_WARDRIVING_FLUSH_BACKLOG_THRESHOLD) ||
               (!wardriving_wifi_active && !wardriving_ble_active) ||
               wardriving_flush_window.open;
   ```
   Replace the long comment above the function's "Gate:" paragraph with 3-4 lines describing the new rule. Leave everything below the gate (backlog-count update, in-flight check, send) **unchanged**.
6. Grep each board for any remaining `FLUSH_STOPPED` or `wardriving_flush_stopped_since_us` references; there must be none. Also check `esp32/cluster_worker/` (it currently has none).

### B — Tests
Add `test_flush_window()` to `tests/esp32/test_wardriving_log.c` (it already links `wardriving_validate.c` via `tests/esp32/build_wardriving.ps1`) and call it from `main`. Cases:
- New window: pushes 1 and 2 don't change `open` (starts false).
- 3 × 10 → open.
- Then 60, 60 → window {10,60,60} = 130 → closed.
- Then 0, 0 → {60,0,0} = 60 → open.
- Exactly 100, e.g. {40,30,30} after an open state → stays open; after a closed state → stays closed.
- Saturation: push 1000 → stored as 255 → closed.

Run `tests/esp32/build_wardriving.ps1`; all checks must pass.

### B — Verification
- `python tools/check_shared_headers.py` shows all OK.
- `tools/build_esp32.ps1` for each of the three boards (or `idf.py build` in `esp32/`, `esp32c5/`, `heltec/`). **On the Heltec, compare the `.dram0.bss` size before and after**; it must not grow by more than a few bytes. If the link fails for DRAM, stop and report back; don't restructure anything.
- Docs: rewrite `docs/PROTOCOL.md` "**Backlog-flush start gate**" (around line 398) with the new rule, keeping its last sentence about in-flight drains continuing. Add a dated line to `docs/PROJECT_HISTORY.md`.
- Hardware (user runs it): with a debug log line of the window sum, confirm the gate closes in a dense area while driving and opens within about 15 s of parking. The ESP32 dedup suppresses repeats while stationary, so the per-pass new-record count falls toward 0.

---

## Part C — Heltec: shrink the mesh-log dedup table (keep dedup on the Heltec)

### Decision (confirmed with the user)
Dedup stays on the Heltec. Removing it would flood the 16 KB `meshlog` flash partition and send one BLE batch per sighting, and the Flipper has no RAM to spare for its own table. The table is shrunk instead.

### Current state (verified, `heltec/main/mesh_log.c`)
- `ML_DEDUP_CAPACITY 128u`, entry `ml_dedup_entry_t { uint8_t len; char id[16]; }` = 17 B, `malloc()`'d once in `mesh_log_init()`, so 2176 B of heap. Heap is not the tight `.dram0.bss`; only the pointer, count and flag live in `.bss`.
- It's used by `ml_dedup_contains()` / `ml_dedup_insert()`: in `mesh_log_record_sighting()` (checked under `ml_mutex` before appending), and in `mesh_log_init()`'s resume walk (seeds the table from every record still in flash).
- Node ids are hex strings: Meshtastic has 8 chars (`MESHTASTIC_NODE_ID_HEX_LEN`) and MeshCore has 16 (`MESHCORE_NODE_ID_HEX_LEN`).
- `meshcore_table.c` / `meshtastic_table.c` are **not** dedup. They're the live node tables for the OLED and the scan capabilities. Don't touch them.

### Change: store a 32-bit hash instead of the full id
- Result: 128 × 4 B = **512 B** of heap, down from 2176 B (−1664 B), with the same 128-node capacity.
- Collision risk: two different nodes hashing equal means the second is silently never logged. Probability is about n²/2³³, which is around 2×10⁻⁶ at n = 128. That's acceptable because wdgwars.pl treats a mesh node as seen or not seen.

Steps in `heltec/main/mesh_log.c`:
1. Replace the `ml_dedup_entry_t` typedef with nothing, and change the storage to `static uint32_t *ml_dedup_hashes;`. Keep `ml_dedup_count` and `ml_dedup_table_full_warned` unchanged.
2. Add:
   ```c
   static uint32_t ml_node_hash(const char *id, size_t len)
   {
       uint32_t h = 2166136261u;
       size_t i;

       for (i = 0; i < len; i++) {
           h = (h ^ (uint8_t)id[i]) * 16777619u;
       }
       return h;
   }
   ```
3. `ml_dedup_contains()`: compute `h = ml_node_hash(node_id, node_id_len)` and compare against `ml_dedup_hashes[i]` for `i < ml_dedup_count`.
4. `ml_dedup_insert()`: keep the NULL, capacity and warn-once logic, and store `ml_dedup_hashes[ml_dedup_count++] = ml_node_hash(...)`.
5. `mesh_log_init()`: `ml_dedup_hashes = (uint32_t *)malloc(ML_DEDUP_CAPACITY * sizeof(uint32_t));`. Update the failure log's byte count expression to match. The seed walk needs no change because it calls contains/insert.
6. Rewrite the top-of-file comment's dedup paragraph and the `mesh_log.h` "Dedup:" bullet. Keep both short: a 4-byte FNV-1a hash per node, 128 entries, about 512 B of heap, and the collision tradeoff in one sentence. Don't keep the long historical narrative; move anything worth keeping to `docs/PROJECT_HISTORY.md`.
7. No wire or flash format change: records on flash still store the full `node_id`, and only the in-RAM table changes. So `docs/PROTOCOL.md` is untouched and the Flipper is untouched.

### C — Verification
- `idf.py build` in `heltec/` succeeds. `.dram0.bss` is unchanged (still a pointer).
- There are no host tests for `mesh_log.c` (it depends on `esp_partition`), so none are needed. If you want one, the hash function could be tested separately, but that's optional.
- Hardware (user runs it): the boot log still prints `mesh log dedup table seeded with N entries`. The same node heard twice logs `recorded new ... node` only once.
- Update the Heltec RAM notes in `docs/hardware/heltec-wifi-lora-32-v2/README.md` or `docs/BACKLOG.md` BL24, whichever records the mesh dedup heap figure. Grep for `2.2 KB` / `ML_DEDUP`.

---

## Commit/tag
Only when the user asks (CLAUDE.md). Suggested split: one commit per part. Stage with `git add -p` because other sessions have edited the same files concurrently (see the user's memory note on partial staging).

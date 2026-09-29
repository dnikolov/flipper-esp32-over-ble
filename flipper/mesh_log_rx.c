#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif

/* mesh_log display state (docs/WARDRIVING_PUBLISH.md "Mesh node publishing") -- unlike
   wifi_scan_aps/ble_scan_devices/the old meshcore_nodes above, this list is written from TWO
   threads: the Home menu's "enter AppScreenMeshLog" handler (main thread) reloads it wholesale
   from mesh/mesh_nodes_current.txt (mesh_log_display_reload(), see that function), and
   handle_mesh_log_status() (BLE thread) appends each newly-decoded push as it's drained, so a
   node arriving while the screen happens to already be open shows up without leaving and
   re-entering. Both writers, and draw_mesh_log_screen()'s own read, take app_wardriving_state_mutex
   for the duration of their access -- unlike the single-writer arrays above, this one has a
   real cross-thread race to guard, not just an accepted convention. Bounded at
   MESH_LOG_DISPLAY_MAX_NODES (64) -- mesh nodes are "sparse, dozens not hundreds" per this
   capability's own design doc; a push arriving once the list is already full is silently
   dropped from the display only (the file on disk is unaffected -- mesh_log_write_record()
   already succeeded before this list is touched). */
#define MESH_LOG_DISPLAY_MAX_NODES 64u

/* feb_mesh_node_display_entry_t (mesh_nodes.h), not feb_mesh_node_entry_t -- int32_t e7
   lat/lon instead of two doubles, 36 bytes/entry instead of 48 at this same unchanged
   64-entry capacity (docs/HARDENING_BACKLOG.md H04). Both writers below convert at
   insert/reload time (feb_mesh_node_entry_to_display(), or direct wire-offset arithmetic in
   handle_mesh_log_status()); draw_mesh_log_screen() converts back to a double only for its
   own single `%.5f` snprintf call, never storing one. */
static feb_mesh_node_display_entry_t mesh_log_display_nodes[MESH_LOG_DISPLAY_MAX_NODES];
/* ---- mesh_log capability (docs/PROTOCOL.md "`mesh_log` command and status payloads",
   docs/WARDRIVING_PUBLISH.md "Mesh node publishing", docs/CAPABILITIES.md's mesh_log
   bullet) ----

   No UI, no AppEvent: this is pure backend plumbing appending to a flat accumulator file,
   same shape as the wardriving CSV export but without the on-screen counters/summaries
   wardriving surfaces. No dedup either -- the Heltec side already dedups "once ever" before
   a record ever lands in its own flash-backed log, so every record this handler sees is
   appended unconditionally. */

/* Reuses app_wardriving_state_mutex rather than a dedicated mutex: both handle_wardriving_status()
   and handle_mesh_log_status() run synchronously on the same BLE thread
   (profile_event_handler's single dispatch, never reentrant), and the only cross-thread
   accessor of either file's state is reset_scan_ui_state_impl() on the app's main thread,
   which already needs to close both files at the same session-teardown points -- a second
   mutex would add nothing but another lock to remember to take. */
static File* mesh_log_file;
static char mesh_log_path[FEB_MESH_LOG_PATH_MAX_LEN];
static bool mesh_log_write_failed;
/* One-shot latch for storage_open_heap_margin_ok()'s log line -- see that function. */
static bool mesh_log_low_heap_logged;

/* G08 (docs/archive/grok-4.6-findings-2026-09-11.md "### G08"): mesh_log_ensure_open()/
   mesh_log_write_record() used to run synchronously inside handle_mesh_log_status() below, on
   BleEventWorker -- same fix as wardriving's own CSV ring (wardriving_rx.c), scaled down: at
   most FEB_MESH_LOG_MAX_RECORDS_PER_BATCH (1) record per reply, so a small ring with a little
   slack for a burst of several chained single-record backlog-drain replies is enough; there is
   no full-batch sizing pressure the way wardriving's 32-per-batch ring has. */
#define MESH_LOG_RING_CAPACITY 8u

typedef struct {
    char node_id[FEB_MESH_LOG_NODE_ID_MAX_LEN];
    uint8_t node_id_len;
    char network[FEB_MESH_LOG_NETWORK_MAX_LEN];
    uint8_t network_len;
    uint64_t lat_e7_offset;
    uint64_t lon_e7_offset;
} MeshLogPendingRecord;

static MeshLogPendingRecord mesh_log_ring[MESH_LOG_RING_CAPACITY];
static size_t mesh_log_ring_head;
static size_t mesh_log_ring_count;
static uint32_t mesh_log_ring_dropped;

/* Caller must hold app_wardriving_state_mutex -- its only caller, mesh_log_close() below,
   already does. */
static void mesh_log_reset_state(void) {
    mesh_log_write_failed = false;
    mesh_log_low_heap_logged = false;
    mesh_log_ring_head = 0;
    mesh_log_ring_count = 0;
    mesh_log_ring_dropped = 0;
}

/* Drains any still-pending ring rows first (main-thread-only caller, same reasoning as
   wardriving_csv_close()'s own comment) so a session ending right after a push doesn't
   silently discard a record that was never even attempted. */
APP_FN void mesh_log_close(Esp32App* app) {
    mesh_log_drain_pending(app);
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    if(mesh_log_file) {
        storage_file_sync(mesh_log_file);
        storage_file_close(mesh_log_file);
        storage_file_free(mesh_log_file);
        mesh_log_file = NULL;
    }
    mesh_log_reset_state();
    furi_mutex_release(app_wardriving_state_mutex);
}

/* Same lazy-open-on-first-record, append-only, fixed-filename pattern as
   wardriving_csv_ensure_open() -- no header row (mesh_nodes.h's format has none) and no
   calendar-date rollover; only a future host script would rename this file away, and only
   after a confirmed successful publish (out of scope for this pass). */
static WardrivingCsvOpenResult mesh_log_ensure_open(Storage* storage) {
    if(mesh_log_file) {
        return WardrivingCsvOpenOk;
    }
    if(!build_mesh_path(mesh_log_path, sizeof(mesh_log_path), FEB_MESH_LOG_FILENAME)) {
        FURI_LOG_E(TAG, "mesh_log: path build failed");
        return WardrivingCsvOpenFailed;
    }
    /* Same deferral reasoning as wardriving_csv_ensure_open()'s own margin check, reusing its
       result enum rather than declaring a second identical one -- the Heltec keeps undrained
       sightings in its own flash log, so deferring here costs a later re-drain, not data. */
    if(!storage_open_heap_margin_ok("mesh_log", &mesh_log_low_heap_logged)) {
        return WardrivingCsvOpenDeferred;
    }

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, mesh_log_path, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(!ok) {
        FURI_LOG_E(TAG, "mesh_log: failed to create '%s'", mesh_log_path);
        storage_file_close(file);
        storage_file_free(file);
        return WardrivingCsvOpenFailed;
    }
    mesh_log_file = file;
    FURI_LOG_I(TAG, "mesh_log: writing to '%s'", mesh_log_path);
    return WardrivingCsvOpenOk;
}

/* Syncs after every write rather than batching every-Nth-record like
   wardriving_csv_write_record() -- FEB_MESH_LOG_MAX_RECORDS_PER_BATCH is permanently 1
   (cbor_mesh_log.h), so there is no per-batch amortization to gain, and mesh node sightings
   are sparse enough that per-write sync overhead is not a concern (mesh_nodes.h). */
static bool mesh_log_write_record(const feb_mesh_log_record_t* record) {
    /* Precondition: handle_mesh_log_status() has already resolved the log file for this batch
       -- same split, and the same reason, as wardriving_csv_write_record()'s own precondition
       comment. */
    if(!mesh_log_file) {
        return false;
    }
    /* Same explicit-double-literal style as wardriving_csv.c's own row formatter, to avoid
       this project's known -Werror=double-promotion/-fsingle-precision-constant trap on the
       real FBT build (docs/LESSONS.md). */
    double lat = ((double)(int64_t)record->lat_e7_offset - (double)900000000) / (double)10000000;
    double lon = ((double)(int64_t)record->lon_e7_offset - (double)1800000000) / (double)10000000;

    static char line_buf[FEB_MESH_LOG_LINE_MAX_LEN];
    size_t line_len = feb_mesh_log_format_line(
        line_buf,
        sizeof(line_buf),
        record->node_id,
        record->node_id_len,
        record->network,
        record->network_len,
        lat,
        lon);
    if(line_len == 0 || storage_file_write(mesh_log_file, line_buf, line_len) != line_len) {
        return false;
    }
    storage_file_sync(mesh_log_file);
    return true;
}

/* Reloads mesh_log_display_nodes/mesh_log_display_count from mesh/mesh_nodes_current.txt
   (docs/WARDRIVING_PUBLISH.md "Mesh node publishing") -- called only from the Home menu's
   "enter AppScreenMeshLog" handler (main thread), never from the BLE thread (that side only
   ever appends, see handle_mesh_log_status() below). Guarded by app_wardriving_state_mutex for the
   whole reload, same as every other access to this list, so the BLE thread's own append can
   never interleave with a reload in progress. A missing/unreadable file, or one with no valid
   lines, just leaves the list empty (screen shows "No mesh nodes yet") -- matches this file's
   "a stale/hand-edited file degrades, never blocks" tolerance (wardriving_settings_load()'s own
   comment).

   Streams the file through a small fixed chunk buffer rather than reading it in one shot
   (former implementation used a 8192-byte static `buf` sized to hold an entire file -- by far
   the single largest static allocation in this app, and the direct cause of this build no
   longer fitting in the Flipper's runtime memory budget for external FAPs: unlike this
   project's ESP32/Heltec firmwares, where .bss is a fixed flash-partition-relative budget
   unrelated to runtime headroom, a Flipper external .fap's entire .text+.data+.bss is loaded
   into the shared system heap at launch, so every `static` byte here competes directly with
   whatever RAM the rest of the OS/BLE stack needs at that moment -- confirmed via the built
   ELF's own linker map, docs/BACKLOG.md). line_carry accumulates a record's bytes across chunk
   boundaries (bounded at FEB_MESH_LOG_LINE_MAX_LEN, the same bound feb_mesh_log_format_line()
   itself enforces on the write side, so a well-formed line can never overflow it; an
   over-length or otherwise malformed line is simply skipped, same degrade-don't-block
   tolerance as a parse failure). No cap on how much of the file is scanned (unlike the former
   implementation's implicit 8192-byte-from-the-start truncation) -- reading stops only once
   MESH_LOG_DISPLAY_MAX_NODES valid entries are loaded or the file is exhausted. */
APP_FN void mesh_log_display_reload(Esp32App* app) {
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    mesh_log_display_count = 0;

    static char path[FEB_MESH_LOG_PATH_MAX_LEN];
    if(build_mesh_path(path, sizeof(path), FEB_MESH_LOG_FILENAME)) {
        File* file = storage_file_alloc(app->storage);
        bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
        if(ok) {
            static char chunk[128];
            static char line_carry[FEB_MESH_LOG_LINE_MAX_LEN];
            size_t line_carry_len = 0;
            bool line_overflowed = false;

            size_t read_len;
            while(mesh_log_display_count < MESH_LOG_DISPLAY_MAX_NODES &&
                  (read_len = storage_file_read(file, chunk, sizeof(chunk))) > 0) {
                for(size_t i = 0; i < read_len; i++) {
                    char c = chunk[i];
                    if(c == '\n' || c == '\r') {
                        if(line_carry_len > 0 && !line_overflowed) {
                            feb_mesh_node_entry_t entry;
                            if(feb_mesh_log_parse_line(line_carry, line_carry_len, &entry)) {
                                feb_mesh_node_entry_to_display(
                                    &entry, &mesh_log_display_nodes[mesh_log_display_count++]);
                            }
                        }
                        line_carry_len = 0;
                        line_overflowed = false;
                        if(mesh_log_display_count >= MESH_LOG_DISPLAY_MAX_NODES) {
                            break;
                        }
                        continue;
                    }
                    if(line_carry_len < sizeof(line_carry)) {
                        line_carry[line_carry_len++] = c;
                    } else {
                        line_overflowed = true;
                    }
                }
            }
            if(line_carry_len > 0 && !line_overflowed &&
               mesh_log_display_count < MESH_LOG_DISPLAY_MAX_NODES) {
                feb_mesh_node_entry_t entry;
                if(feb_mesh_log_parse_line(line_carry, line_carry_len, &entry)) {
                    feb_mesh_node_entry_to_display(
                        &entry, &mesh_log_display_nodes[mesh_log_display_count++]);
                }
            }
        }
        storage_file_close(file);
        storage_file_free(file);
    }

    furi_mutex_release(app_wardriving_state_mutex);
}

/* `status(state="mesh_data")` (docs/PROTOCOL.md): unsolicited, always `request_id == 0`,
   never inspected here since this handler treats every call identically regardless (same
   convention as handle_wardriving_status()'s own unsolicited-backlog-drain branch). Routed
   here by profile_event_handler's status dispatch matching directly on the "mesh_data" state
   text, which is globally unique across the whole protocol (cbor_mesh_log.h's own top
   comment). At most one record per reply (FEB_MESH_LOG_MAX_RECORDS_PER_BATCH == 1) -- the
   loop below still iterates record_count defensively rather than assuming exactly one.

   G08: no longer touches the file or the display list directly -- deep-copies each decoded
   record into mesh_log_ring (bounded, fixed-size, no aliasing of the transient decode buffer)
   and posts AppEventMeshLogPending once; mesh_log_drain_pending() (main thread) does the
   ensure-open/write/sync and the display-list append. */
APP_FN void
    handle_mesh_log_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    feb_cbor_status_t status = feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "mesh_log status payload decode failed: %d; dropping", status);
        return;
    }
    if(!status_payload.has_result) {
        FURI_LOG_W(TAG, "mesh_log status: state=mesh_data but no result; dropping");
        return;
    }

    feb_mesh_log_status_result_payload_t* result = &app_shared_status_result.mesh_log;
    feb_cbor_status_t result_status = feb_cbor_decode_mesh_log_status_result_payload(
        status_payload.result_span, status_payload.result_span_len, result);
    if(result_status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "mesh_log status.result decode failed: %d; dropping", result_status);
        return;
    }

    bool pushed_any = false;
    for(size_t i = 0; i < result->record_count; i++) {
        const feb_mesh_log_record_t* record = &result->records[i];
        furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
        if(mesh_log_ring_count >= MESH_LOG_RING_CAPACITY) {
            mesh_log_ring_dropped++;
            uint32_t dropped = mesh_log_ring_dropped;
            furi_mutex_release(app_wardriving_state_mutex);
            FURI_LOG_E(
                TAG,
                "mesh_log: pending ring full, dropping record (total dropped this session %lu)",
                (unsigned long)dropped);
            continue;
        }
        size_t idx = (mesh_log_ring_head + mesh_log_ring_count) % MESH_LOG_RING_CAPACITY;
        MeshLogPendingRecord* slot = &mesh_log_ring[idx];
        /* Raw byte copies, not copy_clamped_text() -- these fields are wire-max-sized exactly
           (FEB_MESH_LOG_NODE_ID_MAX_LEN/_NETWORK_MAX_LEN), with no spare byte for a NUL the
           way copy_clamped_text() itself requires (dst_cap must include room for it), and
           feb_mesh_log_record_t's own node_id/network fields are non-NUL-terminated spans
           already, matching this. */
        size_t node_id_n =
            record->node_id_len > sizeof(slot->node_id) ? sizeof(slot->node_id) : record->node_id_len;
        memcpy(slot->node_id, record->node_id, node_id_n);
        slot->node_id_len = (uint8_t)node_id_n;
        size_t network_n =
            record->network_len > sizeof(slot->network) ? sizeof(slot->network) : record->network_len;
        memcpy(slot->network, record->network, network_n);
        slot->network_len = (uint8_t)network_n;
        slot->lat_e7_offset = record->lat_e7_offset;
        slot->lon_e7_offset = record->lon_e7_offset;
        mesh_log_ring_count++;
        furi_mutex_release(app_wardriving_state_mutex);
        pushed_any = true;
    }

    if(pushed_any) {
        AppEvent* event = &app_shared_ble_event;
        memset(event, 0, sizeof(*event));
        event->type = AppEventMeshLogPending;
        app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
    }
}

/* Main-thread-only: drains mesh_log_ring, doing the ensure-open (including its own heap-margin
   check), storage_file_write()/storage_file_sync(), and the Mesh Log screen's display-list
   append -- everything handle_mesh_log_status() used to do inline on BleEventWorker, per G08.
   `app` may be NULL only when called with nothing pending (defensive; every real caller passes
   a live Esp32App*). */
APP_FN void mesh_log_drain_pending(Esp32App* app) {
    for(;;) {
        MeshLogPendingRecord entry;
        furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
        if(mesh_log_ring_count == 0) {
            furi_mutex_release(app_wardriving_state_mutex);
            break;
        }
        entry = mesh_log_ring[mesh_log_ring_head];
        mesh_log_ring_head = (mesh_log_ring_head + 1) % MESH_LOG_RING_CAPACITY;
        mesh_log_ring_count--;
        furi_mutex_release(app_wardriving_state_mutex);

        furi_check(app != NULL);

        bool mesh_usable = false;
        furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
        if(!mesh_log_write_failed) {
            switch(mesh_log_ensure_open(app->storage)) {
            case WardrivingCsvOpenOk:
                mesh_usable = true;
                break;
            case WardrivingCsvOpenFailed:
                mesh_log_write_failed = true;
                break;
            case WardrivingCsvOpenDeferred:
                break;
            }
        }
        furi_mutex_release(app_wardriving_state_mutex);

        feb_mesh_log_record_t record = {
            .node_id = entry.node_id,
            .node_id_len = entry.node_id_len,
            .network = entry.network,
            .network_len = entry.network_len,
            .lat_e7_offset = entry.lat_e7_offset,
            .lon_e7_offset = entry.lon_e7_offset,
        };

        furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
        bool write_failed_now = false;
        if(mesh_usable && !mesh_log_write_record(&record)) {
            mesh_log_write_failed = true;
            write_failed_now = true;
        }
        /* Mirror this record into the Mesh Log screen's in-memory display list regardless of
           the file write's own outcome above -- see this block's own former comment in
           handle_mesh_log_status(), unchanged reasoning, just relocated here. */
        if(mesh_log_display_count < MESH_LOG_DISPLAY_MAX_NODES) {
            feb_mesh_node_display_entry_t* display = &mesh_log_display_nodes[mesh_log_display_count];
            copy_clamped_text(display->node_id, sizeof(display->node_id), entry.node_id, entry.node_id_len);
            copy_clamped_text(display->network, sizeof(display->network), entry.network, entry.network_len);
            display->lat_e7 = (int32_t)((int64_t)entry.lat_e7_offset - (int64_t)900000000);
            display->lon_e7 = (int32_t)((int64_t)entry.lon_e7_offset - (int64_t)1800000000);
            mesh_log_display_count++;
        }
        furi_mutex_release(app_wardriving_state_mutex);
        if(write_failed_now) {
            FURI_LOG_E(TAG, "mesh_log: write failed, no further records written this session");
        }
    }
}

/* Mesh Log screen (docs/WARDRIVING_PUBLISH.md "Mesh node publishing") -- repurposed
   2026-09-27 from the old meshcore_scan live-poll table this screen used to show. mesh_log has
   no command/query shape at all (cbor_mesh_log.h's own top comment): the ESP32/Heltec only
   ever unsolicited-pushes its buffered backlog once around session establishment, never
   re-kicked while a session stays open -- there is nothing to poll. This screen therefore
   shows whatever has already been captured into mesh/mesh_nodes_current.txt as of the last
   time it was entered (mesh_log_display_reload(), called from the Home menu's own OK-case),
   plus anything that streams in via handle_mesh_log_status() while the screen happens to stay
   open (that handler appends straight into this same mesh_log_display_nodes[] list). This is
   the honest achievable scope here: "refreshed on open, plus anything that arrives while
   open," not continuous live polling -- an explicit, already-documented scope cut, not
   something this pass fixes. Scrollable (unlike the old fixed 4-row table, since the backing
   list can hold up to MESH_LOG_DISPLAY_MAX_NODES entries), mirroring
   draw_wifi_scan_results()'s own scroll-offset/footer pattern. Records carry no name/role/rssi/
   age (unlike the old meshcore_scan table) -- just node_id, network, and an already-decimal
   lat/lon pair (mesh_nodes.h's own flat-line format), so the row is a simple three-field line. */
#define MESH_LOG_RESULTS_ROW_HEIGHT 10
#define MESH_LOG_RESULTS_MAX_ROWS 4
#define MESH_LOG_RESULTS_FIRST_ROW_Y 22
#define MESH_LOG_RESULTS_FOOTER_Y 62

APP_FN void draw_mesh_log_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Mesh Log");
    canvas_set_font(canvas, FontSecondary);

    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    size_t count = mesh_log_display_count;

    if(count == 0) {
        furi_mutex_release(app_wardriving_state_mutex);
        canvas_draw_str(canvas, 2, MESH_LOG_RESULTS_FIRST_ROW_Y, "No mesh nodes yet");
        canvas_draw_str(canvas, 2, MESH_LOG_RESULTS_FOOTER_Y, "Back: exit view");
        return;
    }

    if(app->mesh_log_scroll_offset >= count) {
        app->mesh_log_scroll_offset = 0;
    }

    uint8_t y = MESH_LOG_RESULTS_FIRST_ROW_Y;
    for(size_t row = 0; row < MESH_LOG_RESULTS_MAX_ROWS; row++) {
        size_t index = app->mesh_log_scroll_offset + row;
        if(index >= count) {
            break;
        }
        const feb_mesh_node_display_entry_t* node = &mesh_log_display_nodes[index];
        /* Converted back to a double only here, for this one snprintf call -- never stored
           (docs/HARDENING_BACKLOG.md H04, mesh_log_display_nodes's own declaration comment). */
        double lat = (double)node->lat_e7 / (double)10000000;
        double lon = (double)node->lon_e7 / (double)10000000;
        char line[64];
        snprintf(
            line,
            sizeof(line),
            "%s %s %.5f,%.5f",
            node->node_id,
            node->network,
            lat,
            lon);
        canvas_draw_str(canvas, 2, (uint8_t)(y + row * MESH_LOG_RESULTS_ROW_HEIGHT), line);
    }
    furi_mutex_release(app_wardriving_state_mutex);

    char footer[32];
    snprintf(
        footer,
        sizeof(footer),
        "%u/%u  Back: exit",
        (unsigned)(app->mesh_log_scroll_offset + 1),
        (unsigned)count);
    canvas_draw_str(canvas, 2, MESH_LOG_RESULTS_FOOTER_Y, footer);
}


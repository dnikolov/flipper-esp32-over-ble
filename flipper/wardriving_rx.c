#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif
/* meshcore_scan (docs/PROTOCOL.md "`meshcore_scan` command and status payloads", Heltec WiFi
   LoRa 32 V2 only) is no longer polled/displayed here -- the Flipper's own screen for this
   data was repurposed to the mesh_log capability's backlog view (AppScreenMeshLog, see
   draw_mesh_log_screen()'s own comment). meshcore_scan itself is unchanged on the ESP32/Heltec
   side; this Flipper just never sends it a `command` anymore, so it never receives an "ok"
   reply to route here either. */

/* ---- wardriving capability (docs/PROTOCOL.md "`wardriving` command and status payloads",
   docs/CAPABILITIES.md's wardriving bullet) ---- */

APP_FN void post_wardriving_run_state(Esp32App* app, bool running, bool is_fresh_start) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventWardrivingRunState;
    event->u.wardriving_run_state.running = running;
    event->u.wardriving_run_state.is_fresh_start = is_fresh_start;
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

static void post_wardriving_batch(
    Esp32App* app,
    uint32_t csv_rows,
    uint64_t backlog_remaining,
    const char* last_wifi_summary,
    const char* last_ble_summary) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventWardrivingBatch;
    event->u.wardriving_batch.csv_rows = csv_rows;
    event->u.wardriving_batch.backlog_remaining = backlog_remaining;
    strncpy(
        event->u.wardriving_batch.last_wifi_summary,
        last_wifi_summary,
        sizeof(event->u.wardriving_batch.last_wifi_summary) - 1);
    strncpy(
        event->u.wardriving_batch.last_ble_summary,
        last_ble_summary,
        sizeof(event->u.wardriving_batch.last_ble_summary) - 1);
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

APP_FN void post_wardriving_error(Esp32App* app, const char* message) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventWardrivingError;
    strncpy(event->u.error_message, message, sizeof(event->u.error_message) - 1);
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* CSV export file state -- primarily touched from handle_wardriving_status(), further below,
   which runs synchronously inside profile_event_handler on BleEventWorker, but also from
   wardriving_csv_close() via reset_scan_ui_state_impl() on the app's main thread (see
   app_wardriving_state_mutex below, which serializes the two). One export file spans one
   authenticated BLE session: opened lazily on the first wardriving record this session sees
   (whether from an unsolicited backlog drain or a live capture after an explicit start), kept
   open and appended to for the rest of the session regardless of any stop/restart within it,
   and closed on disconnect/profile-teardown/app-exit (reset_scan_ui_state(), further below).
   Judgment call: this is simpler than slicing a file per start/stop, and a single connection's
   backlog-drain-then-maybe-live-capture reads naturally as one contiguous export rather than
   several fragments. The per-address dedup table and FirstSeen anchor (docs/CAPABILITIES.md)
   share this exact file-lifetime scope -- reset together with the file in
   wardriving_csv_close() only, never on a same-session "started" ack (former docs/BACKLOG.md
   G29: resetting dedup on every restart made every address still in range look brand-new
   again, defeating the whole policy). */
static File* wardriving_csv_file;
static char wardriving_csv_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
static uint32_t wardriving_csv_records_since_sync;
static bool wardriving_csv_write_failed;
/* One-shot latch for storage_open_heap_margin_ok()'s log line -- see that function. Cleared
   with the rest of the export file's state in wardriving_csv_reset_state(). */
static bool wardriving_csv_low_heap_logged;

/* WardrivingCsvOpenResult -- why a tri-state rather than bool: a low-heap deferral and a real
   open failure must NOT be handled the same way. A real failure (path build, storage refused
   the open) is permanent for this session and correctly latches wardriving_csv_write_failed. A
   deferral is a transient heap condition that may well clear before the next batch arrives, so
   latching on it would throw away the rest of a multi-minute capture over one bad moment.
   Declared in app_internal.h, not here: mesh_log_rx.c's mesh_log_ensure_open() shares this
   same tri-state shape. */
/* G08 update: wardriving_csv_file/wardriving_csv_records_since_sync/wardriving_csv_write_failed/
   wardriving_dedup_table are now touched ONLY from the main thread (wardriving_csv_close() and
   wardriving_csv_drain_pending(), both called from flipper_esp32_over_ble.c's event loop or
   from wardriving_csv_close() itself) -- handle_wardriving_status() on BleEventWorker no
   longer opens/writes/dedups at all, only pushes bounded copies onto wardriving_csv_ring (see
   that ring's own declaration comment). app_wardriving_state_mutex is kept around these fields
   anyway (harmless single-thread overhead) since it's the same mutex the ring itself, and
   handle_wifi_scan_status()/handle_ble_scan_status()'s own HP-08 batching further up, still
   genuinely need cross-thread -- declared near the top of the file (next to
   app_reassembly_mutex/app_protocol_mutex), not here, for that reason. */
/* app_wardriving_flush_led_active (the solid-green-while-flushing / solid-blue-when-idle LED
   indicator for an active wardriving backlog flush, docs/PROTOCOL.md's backlog_remaining
   semantics) is declared earlier in this file, next to session_reset_state() which must
   clear it -- see handle_wardriving_status()'s "data" branch, further below, for both
   transition points. */

/* Same file-lifetime scope and same app_wardriving_state_mutex protection as the fields above
   (see wardriving_csv_file's own declaration comment) -- see wardriving_csv.h's
   feb_wardriving_dedup_should_write() for the policy this table drives. */
static feb_wardriving_dedup_table_t wardriving_dedup_table;

/* G08 (docs/archive/grok-4.6-findings-2026-09-11.md "### G08"): the CSV write path used to
   run synchronously inside wardriving_record_stream_cb() on BleEventWorker (the thread that
   pumps BLE) -- the single most frequent SD I/O in this app, up to
   FEB_WARDRIVING_MAX_RECORDS_PER_BATCH (32) rows per status(data) record. That callback now
   does ONLY a bounded, fixed-size deep-copy of each decoded record's fields into this ring;
   dedup, CSV-row formatting, ensure-open, and every storage_file_* call move to
   wardriving_csv_drain_pending() below, called only from the main thread (the
   AppEventWardrivingBatch/AppEventWardrivingRunState branches in
   flipper_esp32_over_ble.c's event loop, and the shutdown drain at the end of that
   function).

   Ring capacity is one full batch: a whole batch's records are all pushed inside a single
   profile_event_handler call, before the main thread gets a chance to run at all, so a
   smaller ring would drop the tail of every ordinary near-full batch, not just a
   pathological one. There is no protocol-level flow control tied to this Flipper's own
   storage completion -- the ESP32 only paces the next batch on its own BLE TX-completion
   (feb_wardriving_send_next_batch()'s TX_DONE_CONTINUE_WARDRIVING chaining, components/
   feb_app_core/feb_cap_wardriving.c), never on anything this Flipper does with the data --
   so if the main thread is still draining a previous batch when a new one's push starts, the
   ring fills and the rest of that push is dropped with a rate-limited log; the dropped
   record is lost from the CSV for this session (same permanence as an actual write failure
   already latches). This is the bounded-ring/log-and-drop policy this project's own
   docs/LESSONS.md rule calls for, not a design gap.

   Slot fields are fixed-width copies of only what feb_wardriving_csv_format_row() and
   feb_wardriving_dedup_should_write() actually need -- not the ~250-byte formatted CSV row
   text -- to keep the worst-case ring (32 slots) affordable. ssid/auth are clamped to the
   same bounds this app's existing wardriving_csv.c formatter already clamps to (auth_field's
   own 32-byte cap; ssid is already wire-bounded to FEB_WARDRIVING_WIFI_SSID_MAX_LEN), so this
   adds no new truncation point beyond what already exists in feb_wardriving_csv_format_row().
   channel is saturated to 255 rather than truncated -- any wire value that large already maps
   to a blank Channel/Frequency field via channel_to_freq_mhz()'s own range check
   (wardriving_csv.c), so saturating preserves that outcome instead of silently wrapping. */
#define FEB_WARDRIVING_CSV_RING_CAPACITY FEB_WARDRIVING_MAX_RECORDS_PER_BATCH
#define FEB_WARDRIVING_CSV_RING_AUTH_LEN 32u

typedef struct {
    uint64_t utc_timestamp_s;
    uint64_t lat_e7_offset;
    uint64_t lon_e7_offset;
    feb_wardriving_payload_kind_t payload_kind;
    union {
        struct {
            uint8_t ssid[FEB_WARDRIVING_WIFI_SSID_MAX_LEN];
            uint8_t ssid_len;
            uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN];
            uint8_t rssi_offset;
            uint8_t channel;
            char auth[FEB_WARDRIVING_CSV_RING_AUTH_LEN];
            uint8_t auth_len;
        } wifi;
        struct {
            uint8_t address[FEB_WARDRIVING_BLE_ADDRESS_LEN];
            char name[FEB_WARDRIVING_BLE_NAME_MAX_LEN];
            uint8_t name_len;
            bool has_name;
            uint8_t rssi_offset;
        } ble;
    } payload;
} WardrivingCsvPendingRecord;

static WardrivingCsvPendingRecord wardriving_csv_ring[FEB_WARDRIVING_CSV_RING_CAPACITY];
static size_t wardriving_csv_ring_head;
static size_t wardriving_csv_ring_count;
static uint32_t wardriving_csv_ring_dropped;
/* Set (under app_wardriving_state_mutex) by handle_wardriving_status()'s "stopped" branch,
   which used to call storage_file_sync() directly on BleEventWorker -- consumed by
   wardriving_csv_drain_pending() on the main thread instead, same G08 reasoning as the ring
   above. */
static bool wardriving_csv_sync_requested;

/* storage_file_sync() every Nth record rather than every record (durability against a mid-
   session power loss vs. flash-write overhead) or only at close (would lose the whole
   session's writes since the last sync on a power loss) -- 8 chosen to match this app's
   existing message-queue depth, no other significance. */
#define FEB_WARDRIVING_CSV_SYNC_EVERY_N_RECORDS 8u

/* Caller must hold app_wardriving_state_mutex -- its only caller, wardriving_csv_close() below,
   already does. */
static void wardriving_csv_reset_state(void) {
    wardriving_csv_records_since_sync = 0;
    wardriving_csv_write_failed = false;
    wardriving_csv_low_heap_logged = false;
    feb_wardriving_dedup_reset(&wardriving_dedup_table);
    wardriving_csv_ring_head = 0;
    wardriving_csv_ring_count = 0;
    wardriving_csv_ring_dropped = 0;
    wardriving_csv_sync_requested = false;
}

/* Snapshots the live row count/file size into the saved settings pair before closing, so the
   next launch's wardriving_csv_count_refresh() can skip a full recount. Drains any still-
   pending ring rows first (main-thread-only caller, see wardriving_csv_drain_pending()'s own
   comment) so a session ending right after a batch arrives doesn't silently discard rows that
   were never even attempted -- no write-after-close risk, since the drain always runs before
   the file itself is touched below. */
APP_FN void wardriving_csv_close(Esp32App* app) {
    wardriving_csv_drain_pending(app);
    bool persist = false;
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    if(wardriving_csv_file) {
        storage_file_sync(wardriving_csv_file);
        app_wardriving_csv_saved_size = (uint32_t)storage_file_size(wardriving_csv_file);
        app_wardriving_csv_saved_rows = app_wardriving_csv_row_count;
        persist = true;
        storage_file_close(wardriving_csv_file);
        storage_file_free(wardriving_csv_file);
        wardriving_csv_file = NULL;
    }
    wardriving_csv_reset_state();
    furi_mutex_release(app_wardriving_state_mutex);
    if(persist) wardriving_settings_save(app);
}

/* Fixed filename (docs/WARDRIVING_PUBLISH.md "Capture-side change"): one "current" file that
   keeps accumulating across however many capture sessions happen between publishes, no
   calendar-date rollover. FSOM_OPEN_APPEND creates-if-absent and seeks to EOF, matching this
   file's append-only, not atomically-replaced, write pattern (contrast with
   pairing_storage_save()'s temp-file/rename dance, which does not fit an incrementally-
   appended, potentially hours-long export). Only the host script renames this file away,
   and only after a confirmed successful publish -- this FAP never renames it during capture. */
static WardrivingCsvOpenResult wardriving_csv_ensure_open(Storage* storage) {
    if(wardriving_csv_file) {
        return WardrivingCsvOpenOk;
    }
    if(!build_wardriving_path(
           wardriving_csv_path, sizeof(wardriving_csv_path), FEB_WARDRIVING_CSV_FILENAME)) {
        FURI_LOG_E(TAG, "wardriving CSV: path build failed");
        return WardrivingCsvOpenFailed;
    }
    /* Defers this batch rather than letting the Storage service's own malloc reboot the device
       -- see storage_open_heap_margin_ok(). Costs nothing durable: the ESP32 keeps undrained
       records in its own flash backlog, so a batch skipped here is re-offered later. */
    if(!storage_open_heap_margin_ok("wardriving CSV", &wardriving_csv_low_heap_logged)) {
        return WardrivingCsvOpenDeferred;
    }

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, wardriving_csv_path, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok) {
        if(storage_file_size(file) == 0) {
            static char header_buf[FEB_WARDRIVING_CSV_HEADER_MAX_LEN];
            size_t header_len = feb_wardriving_csv_format_header(header_buf, sizeof(header_buf));
            ok = header_len > 0 && storage_file_write(file, header_buf, header_len) == header_len;
            if(ok) {
                app_wardriving_csv_row_count = 0;
            }
        }
    }
    if(!ok) {
        FURI_LOG_E(TAG, "wardriving CSV: failed to create '%s'", wardriving_csv_path);
        storage_file_close(file);
        storage_file_free(file);
        return WardrivingCsvOpenFailed;
    }
    wardriving_csv_file = file;
    FURI_LOG_I(TAG, "wardriving CSV: writing to '%s'", wardriving_csv_path);
    return WardrivingCsvOpenOk;
}

/* Full chunked recount, only reached when the saved size doesn't match the real file -- the
   CSV is guaranteed closed here (main thread only, called before any BLE session can open it),
   so reusing app_wardriving_settings_buf as a scratch read buffer is safe. */
static uint32_t wardriving_csv_count_rows(Storage* storage, const char* path) {
    static bool low_heap_logged;
    uint32_t newlines = 0;
    if(!storage_open_heap_margin_ok("wardriving CSV count", &low_heap_logged)) return 0;
    uint32_t start = furi_get_tick();
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t n;
        while((n = storage_file_read(
                   file, app_wardriving_settings_buf, sizeof(app_wardriving_settings_buf))) > 0) {
            for(size_t i = 0; i < n; i++) newlines += (app_wardriving_settings_buf[i] == '\n');
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    FURI_LOG_I(
        TAG,
        "wardriving CSV: recounted %lu rows in %lu ms",
        (unsigned long)(newlines > 2 ? newlines - 2 : 0),
        (unsigned long)(furi_get_tick() - start));
    return newlines > 2 ? newlines - 2 : 0;
}

/* Seeds app_wardriving_csv_row_count cheaply from the saved settings pair, falling back to a full
   recount only when the file's real size doesn't match what was saved at last close. */
APP_FN void wardriving_csv_count_refresh(Esp32App* app) {
    if(!build_wardriving_path(
           wardriving_csv_path, sizeof(wardriving_csv_path), FEB_WARDRIVING_CSV_FILENAME)) {
        return;
    }
    FileInfo info;
    FS_Error err = storage_common_stat(app->storage, wardriving_csv_path, &info);
    uint32_t rows;
    bool changed = false;
    if(err == FSE_NOT_EXIST) {
        rows = 0;
        changed = app_wardriving_csv_saved_rows != 0 || app_wardriving_csv_saved_size != 0;
        app_wardriving_csv_saved_rows = 0;
        app_wardriving_csv_saved_size = 0;
    } else if(err != FSE_OK) {
        FURI_LOG_W(TAG, "wardriving CSV: stat failed: %d", err);
        rows = app_wardriving_csv_saved_rows;
    } else if((uint32_t)info.size == app_wardriving_csv_saved_size) {
        rows = app_wardriving_csv_saved_rows;
    } else {
        rows = wardriving_csv_count_rows(app->storage, wardriving_csv_path);
        app_wardriving_csv_saved_rows = rows;
        app_wardriving_csv_saved_size = (uint32_t)info.size;
        changed = true;
    }
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    app_wardriving_csv_row_count = rows;
    furi_mutex_release(app_wardriving_state_mutex);
    app->wardriving_csv_rows = rows;
    if(changed) wardriving_settings_save(app);
}

/* FirstSeen (docs/CAPABILITIES.md, docs/PROTOCOL.md's `utc_timestamp_s` wardriving-record
   field, docs/PLAN.md "Real GPS driver..." decision 7): every decoded record is now
   guaranteed to carry a valid `utc_timestamp_s` (feb_cbor_decode_wardriving_record() rejects
   any record missing it -- see cbor_wardriving.c), so this is a direct Unix-epoch-seconds ->
   calendar conversion, not the RTC-anchored backdating approximation this replaced (former
   feb_wardriving_backdate_first_seen(), which assumed the only available timestamp was
   boot-relative `timestamp_ms` with no real wall-clock reference at all). No fallback path
   for a missing/zero utc_timestamp_s is implemented here: the wire contract already rules
   that case out by construction, so one would be dead code (docs/PLAN.md's own framing). */
static bool wardriving_csv_write_record(const feb_wardriving_record_t* record) {
    /* Precondition: handle_wardriving_status() has already resolved the export file for this
       batch (see its own per-batch open, and WardrivingCsvOpenResult's declaration comment for
       why that resolution cannot happen per record). Opening is deliberately NOT retried here
       -- doing so would reintroduce the ambiguity the tri-state exists to remove, since this
       function's bool return has no way to say "deferred, don't latch". The guard below is
       defensive only; wardriving_csv_file is cleared solely by wardriving_csv_close(), which
       holds the same mutex this caller holds. */
    if(!wardriving_csv_file) {
        return false;
    }
    DateTime first_seen_dt;
    datetime_timestamp_to_datetime((uint32_t)record->utc_timestamp_s, &first_seen_dt);
    static char first_seen_str[FEB_WARDRIVING_CSV_FIRST_SEEN_LEN];
    snprintf(
        first_seen_str,
        sizeof(first_seen_str),
        "%04u-%02u-%02u %02u:%02u:%02u",
        (unsigned)first_seen_dt.year,
        (unsigned)first_seen_dt.month,
        (unsigned)first_seen_dt.day,
        (unsigned)first_seen_dt.hour,
        (unsigned)first_seen_dt.minute,
        (unsigned)first_seen_dt.second);

    static char row_buf[FEB_WARDRIVING_CSV_ROW_MAX_LEN];
    size_t row_len = feb_wardriving_csv_format_row(
        row_buf, sizeof(row_buf), record, first_seen_str, strlen(first_seen_str));
    if(row_len == 0 || storage_file_write(wardriving_csv_file, row_buf, row_len) != row_len) {
        return false;
    }
    app_wardriving_csv_row_count++;
    wardriving_csv_records_since_sync++;
    if(wardriving_csv_records_since_sync >= FEB_WARDRIVING_CSV_SYNC_EVERY_N_RECORDS) {
        storage_file_sync(wardriving_csv_file);
        wardriving_csv_records_since_sync = 0;
    }
    return true;
}

/* G08: pushes a bounded, self-contained copy of `record` onto wardriving_csv_ring -- no
   dedup lookup, no ensure-open, no storage_file_* call, all deferred to
   wardriving_csv_drain_pending() on the main thread (see the ring's own declaration comment
   above). `record`'s own ssid/auth/name spans alias the transient decode buffer (valid only
   for this call), so every field is copied by value here, never by pointer. Channel/rssi are
   saturated (not truncated) to fit the narrower ring field widths -- see the ring struct's own
   comment for why that's safe. */
static void wardriving_csv_ring_push(const feb_wardriving_record_t* record) {
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    if(wardriving_csv_ring_count >= FEB_WARDRIVING_CSV_RING_CAPACITY) {
        wardriving_csv_ring_dropped++;
        uint32_t dropped = wardriving_csv_ring_dropped;
        furi_mutex_release(app_wardriving_state_mutex);
        FURI_LOG_E(
            TAG,
            "wardriving CSV: pending ring full, dropping record (total dropped this session %lu)",
            (unsigned long)dropped);
        return;
    }
    size_t idx =
        (wardriving_csv_ring_head + wardriving_csv_ring_count) % FEB_WARDRIVING_CSV_RING_CAPACITY;
    WardrivingCsvPendingRecord* slot = &wardriving_csv_ring[idx];
    slot->utc_timestamp_s = record->utc_timestamp_s;
    slot->lat_e7_offset = record->lat_e7_offset;
    slot->lon_e7_offset = record->lon_e7_offset;
    slot->payload_kind = record->payload_kind;
    if(record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
        const feb_wardriving_wifi_payload_t* wifi = &record->payload.wifi;
        size_t ssid_n = wifi->ssid_len > sizeof(slot->payload.wifi.ssid) ? sizeof(slot->payload.wifi.ssid) :
                                                                            wifi->ssid_len;
        memcpy(slot->payload.wifi.ssid, wifi->ssid, ssid_n);
        slot->payload.wifi.ssid_len = (uint8_t)ssid_n;
        memcpy(slot->payload.wifi.bssid, wifi->bssid, FEB_WIFI_SCAN_BSSID_LEN);
        slot->payload.wifi.rssi_offset = (uint8_t)(wifi->rssi_offset > 255u ? 255u : wifi->rssi_offset);
        slot->payload.wifi.channel = (uint8_t)(wifi->channel > 255u ? 255u : wifi->channel);
        size_t auth_n = wifi->auth_len > sizeof(slot->payload.wifi.auth) ? sizeof(slot->payload.wifi.auth) :
                                                                            wifi->auth_len;
        memcpy(slot->payload.wifi.auth, wifi->auth, auth_n);
        slot->payload.wifi.auth_len = (uint8_t)auth_n;
    } else {
        const feb_wardriving_ble_payload_t* ble = &record->payload.ble;
        memcpy(slot->payload.ble.address, ble->address, FEB_WARDRIVING_BLE_ADDRESS_LEN);
        slot->payload.ble.has_name = ble->has_name ? true : false;
        if(ble->has_name) {
            size_t name_n = ble->name_len > sizeof(slot->payload.ble.name) ? sizeof(slot->payload.ble.name) :
                                                                              ble->name_len;
            memcpy(slot->payload.ble.name, ble->name, name_n);
            slot->payload.ble.name_len = (uint8_t)name_n;
        } else {
            slot->payload.ble.name_len = 0;
        }
        slot->payload.ble.rssi_offset = (uint8_t)(ble->rssi_offset > 255u ? 255u : ble->rssi_offset);
    }
    wardriving_csv_ring_count++;
    furi_mutex_release(app_wardriving_state_mutex);
}

/* Rebuilds a feb_wardriving_record_t whose ssid/auth/name spans point back into `pending`'s
   own fixed arrays (stable for as long as `pending` itself is, i.e. for this whole call) so
   wardriving_csv_drain_pending() can reuse feb_wardriving_dedup_should_write()/
   wardriving_csv_write_record() completely unchanged rather than re-deriving their logic
   against a second record shape. */
static void wardriving_csv_pending_to_record(
    const WardrivingCsvPendingRecord* pending, feb_wardriving_record_t* out) {
    memset(out, 0, sizeof(*out));
    out->utc_timestamp_s = pending->utc_timestamp_s;
    out->lat_e7_offset = pending->lat_e7_offset;
    out->lon_e7_offset = pending->lon_e7_offset;
    out->payload_kind = pending->payload_kind;
    if(pending->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
        out->payload.wifi.ssid = pending->payload.wifi.ssid;
        out->payload.wifi.ssid_len = pending->payload.wifi.ssid_len;
        memcpy(out->payload.wifi.bssid, pending->payload.wifi.bssid, FEB_WIFI_SCAN_BSSID_LEN);
        out->payload.wifi.rssi_offset = pending->payload.wifi.rssi_offset;
        out->payload.wifi.channel = pending->payload.wifi.channel;
        out->payload.wifi.auth = pending->payload.wifi.auth;
        out->payload.wifi.auth_len = pending->payload.wifi.auth_len;
    } else {
        memcpy(out->payload.ble.address, pending->payload.ble.address, FEB_WARDRIVING_BLE_ADDRESS_LEN);
        out->payload.ble.has_name = pending->payload.ble.has_name ? 1 : 0;
        out->payload.ble.name = pending->payload.ble.has_name ? pending->payload.ble.name : NULL;
        out->payload.ble.name_len = pending->payload.ble.name_len;
        out->payload.ble.rssi_offset = pending->payload.ble.rssi_offset;
    }
}

/* Main-thread-only: drains wardriving_csv_ring, doing the dedup check, ensure-open (including
   its own heap-margin check), CSV-row formatting, and the actual storage_file_write()/
   periodic storage_file_sync() -- everything wardriving_record_stream_cb() used to do inline
   on BleEventWorker, per G08. Called from flipper_esp32_over_ble.c's event loop
   (AppEventWardrivingBatch/AppEventWardrivingRunState branches) and from
   wardriving_csv_close() before the file itself is closed. A permanently-failed session
   (wardriving_csv_write_failed already latched) still drains-and-discards every pending entry
   rather than leaving them in the ring forever. Also services
   wardriving_csv_sync_requested (set by handle_wardriving_status()'s "stopped" branch,
   another storage_file_sync() call that used to run on BleEventWorker). */
APP_FN void wardriving_csv_drain_pending(Esp32App* app) {
    for(;;) {
        WardrivingCsvPendingRecord entry;
        furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
        if(wardriving_csv_ring_count == 0) {
            furi_mutex_release(app_wardriving_state_mutex);
            break;
        }
        entry = wardriving_csv_ring[wardriving_csv_ring_head];
        wardriving_csv_ring_head = (wardriving_csv_ring_head + 1) % FEB_WARDRIVING_CSV_RING_CAPACITY;
        wardriving_csv_ring_count--;
        furi_mutex_release(app_wardriving_state_mutex);

        if(wardriving_csv_write_failed) {
            continue;
        }
        if(!wardriving_csv_file) {
            WardrivingCsvOpenResult open_result = wardriving_csv_ensure_open(app->storage);
            if(open_result == WardrivingCsvOpenDeferred) {
                /* Heap still tight -- this ring entry is lost (already popped): the ring
                   bounds BleEventWorker-vs-main-thread timing, not indefinite storage against
                   a sustained low-heap condition. The ESP32 has already marked this record
                   drained from its own backlog by the time the Flipper ever saw it, so it will
                   not be resent either way -- same permanence as today's pre-refactor
                   deferred-batch case, just decided per-record now instead of per-batch. */
                FURI_LOG_W(TAG, "wardriving CSV: deferred (low heap), dropping this record");
                continue;
            }
            if(open_result == WardrivingCsvOpenFailed) {
                wardriving_csv_write_failed = true;
                FURI_LOG_E(TAG, "wardriving CSV: open failed, no records written this session");
                post_wardriving_error(app, "CSV export write failed");
                continue;
            }
        }

        feb_wardriving_record_t record;
        wardriving_csv_pending_to_record(&entry, &record);
        if(feb_wardriving_dedup_should_write(&wardriving_dedup_table, &record) &&
           !wardriving_csv_write_record(&record)) {
            wardriving_csv_write_failed = true;
            FURI_LOG_E(TAG, "wardriving CSV: write failed, no further records written this session");
            post_wardriving_error(app, "CSV export write failed");
        }
    }

    bool do_sync;
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    do_sync = wardriving_csv_sync_requested;
    wardriving_csv_sync_requested = false;
    uint32_t csv_rows = app_wardriving_csv_row_count;
    furi_mutex_release(app_wardriving_state_mutex);
    if(do_sync && wardriving_csv_file) {
        storage_file_sync(wardriving_csv_file);
        wardriving_csv_records_since_sync = 0;
    }
    app->wardriving_csv_rows = csv_rows;
}

/* Streaming apply-pass state for handle_wardriving_status()'s "data" branch, one instance per
   batch (docs/HARDENING_BACKLOG.md H04 -- replaces the former whole-array
   feb_wardriving_status_result_payload_t decode-scratch, up to 32 full records resident at
   once, with feb_cbor_decode_wardriving_status_result_payload_stream()'s one-record-at-a-time
   callback, cbor_wardriving.h). G08: no longer tracks csv_resolved/csv_usable -- the callback
   below now only deep-copies each record into wardriving_csv_ring (wardriving_csv_ring_push())
   and computes the two display summary strings, both off BleEventWorker's SD-I/O path
   entirely; ensure-open/dedup/write moved to wardriving_csv_drain_pending() on the main
   thread. Declared `static`, not a stack local of the callback/handler pair, matching this
   project's own ">=100 bytes reachable from BleEventWorker must be static" convention
   (docs/LESSONS.md), even though it shrank once csv_resolved/csv_usable were removed. */
typedef struct {
    char last_wifi_summary[40];
    char last_ble_summary[40];
} WardrivingRecordStreamCtx;

static void wardriving_record_stream_cb(const feb_wardriving_record_t* record, void* ctx_ptr) {
    WardrivingRecordStreamCtx* ctx = (WardrivingRecordStreamCtx*)ctx_ptr;

    wardriving_csv_ring_push(record);

    if(record->payload_kind == FEB_WARDRIVING_PAYLOAD_BLE) {
        const feb_wardriving_ble_payload_t* ble = &record->payload.ble;
        snprintf(
            ctx->last_ble_summary,
            sizeof(ctx->last_ble_summary),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            ble->address[0],
            ble->address[1],
            ble->address[2],
            ble->address[3],
            ble->address[4],
            ble->address[5]);
    } else {
        const feb_wardriving_wifi_payload_t* wifi = &record->payload.wifi;
        size_t n = wifi->ssid_len > sizeof(ctx->last_wifi_summary) - 1 ?
                       sizeof(ctx->last_wifi_summary) - 1 :
                       wifi->ssid_len;
        for(size_t j = 0; j < n; j++) {
            uint8_t b = wifi->ssid[j];
            ctx->last_wifi_summary[j] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
        }
        ctx->last_wifi_summary[n] = '\0';
        if(n == 0) {
            strncpy(ctx->last_wifi_summary, "(hidden)", sizeof(ctx->last_wifi_summary) - 1);
            ctx->last_wifi_summary[sizeof(ctx->last_wifi_summary) - 1] = '\0';
        }
    }
}

/* `status` (docs/PROTOCOL.md "`wardriving` command and status payloads") -- unlike
   wifi_scan/ble_scan's `partial`/`complete` pair, wardriving's own states ("started"/"data"/
   "stopped") are never ambiguous with those or each other by text alone, so no
   app_pending_command_kind check is needed to route here (see profile_event_handler's status
   dispatch, further below) or within this function. Every state is handled regardless of
   `request_id`, including the `request_id == 0` unsolicited-backlog-drain sentinel
   (docs/PROTOCOL.md "Unsolicited backlog drain") -- this function never inspects
   status_payload.request_id at all, so there is nothing to special-case for it. G08: CSV rows
   for a "data" batch are no longer written synchronously here -- wardriving_record_stream_cb()
   (invoked from inside feb_cbor_decode_wardriving_status_result_payload_stream(), still on the
   BLE thread) only deep-copies each decoded record onto wardriving_csv_ring; the dedup gate
   (feb_wardriving_dedup_should_write(), wardriving_csv.h) and the actual
   wardriving_csv_write_record() call both move to wardriving_csv_drain_pending(), run on the
   main thread once this function's own post_wardriving_batch() call, further below, wakes it
   -- so a 32-record batch still can never overrun the main-thread event queue's depth (see
   AppEventWardrivingBatch's own comment), and BleEventWorker itself never touches storage. */
APP_FN void
    handle_wardriving_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    feb_cbor_status_t status = feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "wardriving status payload decode failed: %d; dropping", status);
        return;
    }

    if(text_matches(status_payload.state, status_payload.state_len, "started")) {
        /* Deliberately NOT wardriving_csv_reset_state() here (docs/BACKLOG.md's former G29):
           a manual stop/restart mid-session must not wipe the FirstSeen anchor or the
           per-address dedup table, or every address still in range looks brand-new again
           and the RSSI-improvement/movement policy is defeated. Dedup scope is the CSV
           export file's lifetime (docs/CAPABILITIES.md), same as wardriving_csv_file itself
           -- both are reset together only in wardriving_csv_close(), on disconnect/teardown,
           never on a same-session restart. record->timestamp_ms is esp_timer_get_time()-based
           on the ESP32 (monotonic since its boot, not reset by a start/stop), so the anchor
           staying live across a restart cannot regress or go stale. */
        app_pending_command_kind = PendingCommandNone;
        post_wardriving_run_state(app, true, true);
        return;
    }
    if(text_matches(status_payload.state, status_payload.state_len, "stopped")) {
        app_pending_command_kind = PendingCommandNone;
        /* G08: storage_file_sync() used to run right here on BleEventWorker -- just request
           it; wardriving_csv_drain_pending() (main thread) performs the actual sync the next
           time it runs, which the AppEventWardrivingRunState this posts below already
           triggers (see that branch in flipper_esp32_over_ble.c). */
        furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
        wardriving_csv_sync_requested = true;
        furi_mutex_release(app_wardriving_state_mutex);
        post_wardriving_run_state(app, false, false);
        return;
    }
    if(!text_matches(status_payload.state, status_payload.state_len, "data")) {
        FURI_LOG_W(
            TAG,
            "wardriving status: unexpected state '%.*s'; dropping",
            (int)status_payload.state_len,
            status_payload.state);
        return;
    }
    if(!status_payload.has_result) {
        return;
    }

    /* A live wardriving session is already active as soon as the ESP32 emits a real
       `status(state="data")` batch, even before a fresh "started" ack is seen in this
       session. Without this update, the UI stays in the "unknown" fallback forever after a
       reconnect or a reopened screen, and the user sees Start instead of Stop. */
    post_wardriving_run_state(app, true, false);

    static WardrivingRecordStreamCtx stream_ctx;
    memset(&stream_ctx, 0, sizeof(stream_ctx));

    uint64_t backlog_remaining = 0;
    feb_cbor_status_t result_status = feb_cbor_decode_wardriving_status_result_payload_stream(
        status_payload.result_span,
        status_payload.result_span_len,
        wardriving_record_stream_cb,
        &stream_ctx,
        &backlog_remaining);
    if(result_status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "wardriving status.result decode failed: %d; dropping", result_status);
        return;
    }

    if(!app_wardriving_flush_led_active && app->notifications) {
        notification_message(app->notifications, &sequence_set_only_green_255);
        app_wardriving_flush_led_active = true;
    }

    /* Report the file-backed count, not this batch's record_count -- Recs must reflect rows
       actually written to the CSV, including the dedup skips wardriving_record_stream_cb()
       applied above. */
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    uint32_t csv_rows = app_wardriving_csv_row_count;
    furi_mutex_release(app_wardriving_state_mutex);
    post_wardriving_batch(
        app, csv_rows, backlog_remaining, stream_ctx.last_wifi_summary, stream_ctx.last_ble_summary);

    if(app_wardriving_flush_led_active && backlog_remaining == 0 && app->notifications) {
        notification_message(app->notifications, &sequence_set_only_blue_255);
        app_wardriving_flush_led_active = false;
    }
}

/* map(1) + "action"key(1+6)+"start"value(1+5) + "sources"key(1+7)+array header(1)+2 text
   values ("wifi"=1+4,"ble_passive"=1+11) + "wifi_interval_ms"key(1+16)+uint(5) +
   "ble_window_ms"key(1+13)+uint(5) + "ble_interval_ms"key(1+16)+uint(5) +
   "wifi_swelling"key(1+13)+"speed_based"value(1+11) + "country"key(1+7)+"RoW"value(1+3) +
   "wifi_band"key(1+9)+"5ghz_fast"value(1+9) == ~175 bytes worst case (wifi_swelling/country
   added 2026-09-21, docs/WARDRIVING_REDESIGN.md; wifi_band added 2026-09-26,
   docs/PROTOCOL.md); sized with real margin (see the shared app_cmd_payload_buf
   declaration's own comment above for why this project no longer shaves these to the byte).
   FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN itself lives on below only to size this function's
   local `arguments_buf`; the command payload/ciphertext/record scratch is the shared
   app_cmd_payload_buf/app_cmd_ciphertext_buf/app_cmd_record_buf declared with wifi_scan's command
   scratch above. */
#define FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN 224u
static uint64_t wardriving_next_request_id = 1;

/* Sends the wardriving `start` command (docs/PROTOCOL.md "`wardriving` command and status
    payloads") built from the Stopped screen's five independent settings fields (docs/
    WARDRIVING_REDESIGN.md, 2026-09-21, replacing the old single WardrivingSourceMode enum).
    `use_wifi`/`use_ble` intersect the user's Mode choice with what the board actually
    advertises, same as the old code did with its own want/capability split -- a board that
    only advertises one radio is never blocked by a stale Mode value, since that row is
    hidden (not editable) on such a board anyway. `ble_passive` requests an observer-only BLE
    scan; the ESP32 temporarily uses active discovery while disconnected so wardriving can
    still find the Flipper and reconnect. */
APP_FN bool send_wardriving_start_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    /* static, not stack-local: this struct grew to ~96 bytes once wifi_swelling/country
       were added (2026-09-21, further grown by wifi_band 2026-09-26), crossing this file's
       own "sizeable buffer on a BLE-thread-reachable path must be static" rule of thumb
       (docs/LESSONS.md) -- send_wardriving_
       status_query() below shares this same struct type and IS reached from BleEventWorker
       (handle_client_auth() -> send_wardriving_status_query()), so all three wardriving
       command-builder functions use the same static convention for consistency, even though
       this particular function (start) is only ever called from the main app thread today. */
    static feb_wardriving_command_payload_t command_args;
    bool want_wifi = app->wardriving_mode != WardrivingModeBle;
    bool want_ble = app->wardriving_mode != WardrivingModeWifi;
    bool use_wifi = app->capability_has_wifi_scan && want_wifi;
    bool use_ble = app->capability_has_ble_scan && want_ble;
    bool ble_passive = app->wardriving_ble_mode == WardrivingBleModePassive;

    memset(&command_args, 0, sizeof(command_args));
    command_args.action = "start";
    command_args.action_len = sizeof("start") - 1;
    command_args.has_sources = 1;
    size_t source_count = 0;
    if(use_wifi) {
        command_args.sources[source_count] = "wifi";
        command_args.source_lens[source_count] = sizeof("wifi") - 1;
        source_count++;
    }
    if(use_ble) {
        command_args.sources[source_count] = ble_passive ? "ble_passive" : "ble";
        command_args.source_lens[source_count] = ble_passive ? sizeof("ble_passive") - 1 : sizeof("ble") - 1;
        source_count++;
    }
    command_args.source_count = source_count;
    if(use_wifi) {
        command_args.has_wifi_interval_ms = 1;
        command_args.wifi_interval_ms = app->wardriving_cooldown_ms;
        command_args.has_wifi_swelling = 1;
        command_args.wifi_swelling = wardriving_swelling_wire_value(app->wardriving_swelling);
        command_args.wifi_swelling_len = strlen(command_args.wifi_swelling);
        command_args.has_country = 1;
        command_args.country = wardriving_country_wire_value(app->wardriving_country);
        command_args.country_len = strlen(command_args.country);
        command_args.has_wifi_band = 1;
        command_args.wifi_band = wardriving_wifi_band_wire_value(app->wardriving_wifi_band);
        command_args.wifi_band_len = strlen(command_args.wifi_band);
    }
    command_args.has_ble_params = use_ble;
    if(use_ble) {
        command_args.ble_window_ms = 100;
        command_args.ble_interval_ms = 500;
    }
    if(source_count == 0) {
        FURI_LOG_W(TAG, "wardriving start: no source selected/available");
        return false;
    }

    uint8_t arguments_buf[FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN];
    size_t arguments_len = feb_cbor_encode_wardriving_command_payload(
        arguments_buf, sizeof(arguments_buf), &command_args);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "wardriving start: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "wardriving",
        .capability_len = sizeof("wardriving") - 1,
        .request_id = wardriving_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        app_cmd_payload_buf, sizeof(app_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "wardriving start: payload encode failed");
        return false;
    }
    size_t record_len = session_send_encrypted_command(
        "wardriving start",
        "command",
        sizeof("command") - 1,
        app_cmd_payload_buf,
        payload_len,
        app_cmd_ciphertext_buf,
        sizeof(app_cmd_ciphertext_buf),
        app_cmd_record_buf,
        sizeof(app_cmd_record_buf));
    if(record_len == 0) {
        return false;
    }
    app_pending_command_kind = PendingCommandWardrivingStart;
    if(!send_pairing_record(profile, app_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "wardriving start: send failed");
        return false;
    }
    FURI_LOG_I(
        TAG,
        "wardriving start command sent (request_id=%llu)",
        (unsigned long long)command.request_id);
    return true;
}

/* Sends a wardriving status-query (`arguments = {action:"status"}`) to refresh the UI's
   current running/stopped state immediately after session auth or when the wardriving screen
   is reopened before any fresh `started`/`data` packet has arrived. This is a second guard
   against the reconnect/unknown-state bug: a live board will respond with its current state
   even if no new record has yet drained across the connection. */
APP_FN bool send_wardriving_status_query(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    /* static, not stack-local -- see send_wardriving_start_command()'s own comment on this
       same struct type; this call site is the one that's actually BleEventWorker-reachable
       (handle_client_auth() -> send_wardriving_status_query()). */
    static feb_wardriving_command_payload_t command_args;
    memset(&command_args, 0, sizeof(command_args));
    command_args.action = "status";
    command_args.action_len = sizeof("status") - 1;

    uint8_t arguments_buf[32];
    size_t arguments_len = feb_cbor_encode_wardriving_command_payload(
        arguments_buf, sizeof(arguments_buf), &command_args);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "wardriving status query: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "wardriving",
        .capability_len = sizeof("wardriving") - 1,
        .request_id = wardriving_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        app_cmd_payload_buf, sizeof(app_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "wardriving status query: payload encode failed");
        return false;
    }
    size_t record_len = session_send_encrypted_command(
        "wardriving status query",
        "command",
        sizeof("command") - 1,
        app_cmd_payload_buf,
        payload_len,
        app_cmd_ciphertext_buf,
        sizeof(app_cmd_ciphertext_buf),
        app_cmd_record_buf,
        sizeof(app_cmd_record_buf));
    if(record_len == 0) {
        return false;
    }
    app_pending_command_kind = PendingCommandWardrivingStatus;
    if(!send_pairing_record(profile, app_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "wardriving status query: send failed");
        return false;
    }
    FURI_LOG_I(
        TAG,
        "wardriving status query sent (request_id=%llu)",
        (unsigned long long)command.request_id);
    return true;
}

/* Sends the wardriving `stop` command (arguments = {action:"stop"} alone, per PROTOCOL.md);
   mirrors send_wardriving_start_command() above, same ordering-safety argument. */
APP_FN bool send_wardriving_stop_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    /* static, not stack-local -- see send_wardriving_start_command()'s own comment on this
       same struct type. */
    static feb_wardriving_command_payload_t command_args;
    memset(&command_args, 0, sizeof(command_args));
    command_args.action = "stop";
    command_args.action_len = sizeof("stop") - 1;

    uint8_t arguments_buf[32];
    size_t arguments_len = feb_cbor_encode_wardriving_command_payload(
        arguments_buf, sizeof(arguments_buf), &command_args);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "wardriving stop: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "wardriving",
        .capability_len = sizeof("wardriving") - 1,
        .request_id = wardriving_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        app_cmd_payload_buf, sizeof(app_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "wardriving stop: payload encode failed");
        return false;
    }
    size_t record_len = session_send_encrypted_command(
        "wardriving stop",
        "command",
        sizeof("command") - 1,
        app_cmd_payload_buf,
        payload_len,
        app_cmd_ciphertext_buf,
        sizeof(app_cmd_ciphertext_buf),
        app_cmd_record_buf,
        sizeof(app_cmd_record_buf));
    if(record_len == 0) {
        return false;
    }
    app_pending_command_kind = PendingCommandWardrivingStop;
    if(!send_pairing_record(profile, app_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "wardriving stop: send failed");
        return false;
    }
    FURI_LOG_I(
        TAG,
        "wardriving stop command sent (request_id=%llu)",
        (unsigned long long)command.request_id);
    return true;
}


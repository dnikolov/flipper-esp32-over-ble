#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif
/* ---- `agps` capability (docs/AGPS.md): manual "Send A-GPS" from the GPS screen. ----
   The chosen window (<= 4096 B) is never held in RAM: agps_step() reads one
   AGPS_WIRE_CHUNK_MAX-byte chunk from agps.bin (open/seek/read/close, so no file handle can
   outlive a disconnect) and sends it as one `agps` command; the next chunk goes out only when
   the C5's `partial` reply for it arrives (AppEventAgpsPartial -> agps_on_partial()), as
   docs/PROTOCOL.md requires. All CASIC knowledge stays on the
   C5; the Flipper only validates the file container and picks the window by RTC. Everything
   here runs on the main thread except handle_agps_status()/post_agps_error() (BleEventWorker,
   statics only). */

static bool agps_low_heap_logged;

typedef struct {
    File* file;
} AgpsReadCtx;

static bool agps_read_cb(void* ctx, uint32_t offset, uint8_t* buf, size_t len) {
    File* file = ((AgpsReadCtx*)ctx)->file;
    return storage_file_seek(file, offset, true) && storage_file_read(file, buf, len) == len;
}

static void agps_fail(Esp32App* app, AgpsError error) {
    app->agps_state = AgpsStateFailed;
    app->agps_error = error;
    /* A timed-out transfer keeps the pending kind so a late `complete` is still routed here
       (and then ignored) instead of being mistaken for a wifi_scan/ble_scan status. */
    if(error != AgpsErrorTimeout && app_pending_command_kind == PendingCommandAgps) {
        app_pending_command_kind = PendingCommandNone;
    }
    FURI_LOG_W(TAG, "A-GPS failed: error=%u", (unsigned)error);
}

/* OK on the GPS screen. Checks file and window first and refuses (never sends) on anything but
   a currently-valid window. */
__attribute__((noinline)) APP_FN void agps_start(Esp32App* app) {
    app->agps_error = AgpsErrorNone;
    app->agps_sent = 0;
    app->agps_acked = 0;
    app->agps_nacked = 0;
    app->agps_timeout = 0;
    app->agps_off = 0;
    app->agps_total = 0;
    app->agps_date_unix = 0;
    if(!app->capability_has_agps) {
        return;
    }
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        agps_fail(app, AgpsErrorNotConnected);
        return;
    }
    uint32_t now = furi_hal_rtc_get_timestamp();
    if(now < AGPS_WIRE_UTC_MIN || now >= AGPS_WIRE_UTC_MAX) {
        agps_fail(app, AgpsErrorBadClock);
        return;
    }
    if(!build_app_data_path(app_path_a, sizeof(app_path_a), FEB_AGPS_FILENAME) ||
       !storage_file_exists(app->storage, app_path_a)) {
        agps_fail(app, AgpsErrorNoFile);
        return;
    }
    if(!storage_open_heap_margin_ok("agps", &agps_low_heap_logged)) {
        agps_fail(app, AgpsErrorNoMemory);
        return;
    }
    File* file = storage_file_alloc(app->storage);
    AgpsWindow window;
    AgpsSelResult selection = AgpsSelBadFormat;
    if(storage_file_open(file, app_path_a, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(file);
        if(size <= 0x7FFFFFFFu) {
            AgpsReadCtx ctx = {.file = file};
            selection = agps_select_window(agps_read_cb, &ctx, (uint32_t)size, now, &window);
        }
    }
    storage_file_close(file);
    storage_file_free(file);

    if(selection == AgpsSelExpired) {
        app->agps_date_unix = window.last_valid_to;
        agps_fail(app, AgpsErrorExpired);
    } else if(selection == AgpsSelNoWindowNow) {
        agps_fail(app, AgpsErrorNoWindowNow);
    } else if(selection != AgpsSelOk) {
        agps_fail(app, AgpsErrorBadFile);
    } else {
        app->agps_utc = now;
        app->agps_window_offset = window.offset;
        app->agps_total = window.length;
        app->agps_date_unix = window.valid_to;
        app->agps_state = AgpsStateSending;
        app_pending_command_kind = PendingCommandAgps;
        agps_step(app);
    }
}

static uint32_t agps_next_request_id = 1;

/* Sends the next chunk; agps_on_partial() calls it again once the C5 accepted this one. */
__attribute__((noinline)) APP_FN void agps_step(Esp32App* app) {
    if(app->agps_state != AgpsStateSending) {
        return;
    }
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        agps_fail(app, AgpsErrorNotConnected);
        return;
    }
    uint16_t len = app->agps_total - app->agps_off;
    if(len > AGPS_WIRE_CHUNK_MAX) {
        len = AGPS_WIRE_CHUNK_MAX;
    }
    uint8_t chunk[AGPS_WIRE_CHUNK_MAX];
    if(!build_app_data_path(app_path_a, sizeof(app_path_a), FEB_AGPS_FILENAME)) {
        agps_fail(app, AgpsErrorReadFailed);
        return;
    }
    if(!storage_open_heap_margin_ok("agps", &agps_low_heap_logged)) {
        agps_fail(app, AgpsErrorNoMemory);
        return;
    }
    File* file = storage_file_alloc(app->storage);
    AgpsReadCtx ctx = {.file = file};
    bool read_ok = storage_file_open(file, app_path_a, FSAM_READ, FSOM_OPEN_EXISTING) &&
                   agps_read_cb(&ctx, app->agps_window_offset + app->agps_off, chunk, len);
    storage_file_close(file);
    storage_file_free(file);
    if(!read_ok) {
        agps_fail(app, AgpsErrorReadFailed);
        return;
    }

    uint8_t arguments[AGPS_WIRE_CHUNK_MAX + 48];
    size_t arguments_len = agps_wire_encode_chunk_arguments(
        arguments, sizeof(arguments), app->agps_utc, app->agps_total, app->agps_off, chunk, len);
    if(arguments_len == 0) {
        agps_fail(app, AgpsErrorSendFailed);
        return;
    }
    app->agps_sent_id = agps_next_request_id++;
    feb_command_payload_t command = {
        .capability = AGPS_WIRE_CAPABILITY,
        .capability_len = sizeof(AGPS_WIRE_CAPABILITY) - 1,
        .request_id = app->agps_sent_id,
        .arguments_span = arguments,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        app_cmd_payload_buf, sizeof(app_cmd_payload_buf), &command);
    size_t record_len = 0;
    if(payload_len != 0) {
        record_len = session_send_encrypted_command(
            "agps command",
            "command",
            sizeof("command") - 1,
            app_cmd_payload_buf,
            payload_len,
            app_cmd_ciphertext_buf,
            sizeof(app_cmd_ciphertext_buf),
            app_cmd_record_buf,
            sizeof(app_cmd_record_buf));
    }
    if(record_len == 0 ||
       !send_pairing_record((Esp32BleProfile*)app->profile, app_cmd_record_buf, record_len)) {
        agps_fail(app, AgpsErrorSendFailed);
        return;
    }
    app->agps_wait_elapsed_ms = 0;
    app->agps_off = (uint16_t)(app->agps_off + len);
    if(app->agps_off >= app->agps_total) {
        /* The final chunk's own `partial` is ignored from here on; `complete` ends the wait. */
        app->agps_state = AgpsStateWaitingReply;
    }
}

/* The C5 accepted chunk `request_id`: send the next one (stale/duplicate replies are dropped). */
APP_FN void agps_on_partial(Esp32App* app, uint32_t request_id) {
    if(app->agps_state == AgpsStateSending && request_id == app->agps_sent_id) {
        agps_step(app);
    }
}

/* Called from the GPS poll tick (every GPS_POLL_PERIOD_MS) while a transfer is in flight. */
APP_FN void agps_wait_tick(Esp32App* app) {
    app->agps_wait_elapsed_ms += GPS_POLL_PERIOD_MS;
    uint32_t limit = app->agps_state == AgpsStateSending ? FEB_AGPS_PARTIAL_TIMEOUT_MS :
                                                           FEB_AGPS_REPLY_TIMEOUT_MS;
    if(app->agps_wait_elapsed_ms >= limit) {
        agps_fail(app, AgpsErrorTimeout);
    }
}

/* Back while sending/waiting: discards the in-progress transfer (the C5 drops an incomplete
   blob on its own timeout). While waiting for `complete` the pending kind is kept so that late
   status is still routed to agps (and ignored here) instead of to the scan handlers. */
APP_FN void agps_cancel(Esp32App* app) {
    if(app->agps_state != AgpsStateWaitingReply && app_pending_command_kind == PendingCommandAgps) {
        app_pending_command_kind = PendingCommandNone;
    }
    app->agps_state = AgpsStateIdle;
    app->agps_error = AgpsErrorNone;
}

APP_FN void agps_apply_result(Esp32App* app, const AppEvent* event) {
    if(app_pending_command_kind == PendingCommandAgps && event->type == AppEventAgpsResult) {
        app_pending_command_kind = PendingCommandNone;
    }
    if(app->agps_state != AgpsStateWaitingReply && app->agps_state != AgpsStateSending) {
        return;
    }
    if(event->type == AppEventAgpsError) {
        agps_fail(app, (AgpsError)event->u.agps.error);
        return;
    }
    if(app->agps_state != AgpsStateWaitingReply || event->u.agps.request_id != app->agps_sent_id) {
        return;
    }
    app->agps_sent = event->u.agps.sent;
    app->agps_acked = event->u.agps.acked;
    app->agps_nacked = event->u.agps.nacked;
    app->agps_timeout = event->u.agps.timeout;
    app->agps_state = AgpsStateDone;
}

APP_FN void post_agps_error(Esp32App* app, AgpsError error) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventAgpsError;
    event->u.agps.error = (uint8_t)error;
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* `status` partial/complete for the pending `agps` command, routed by profile_event_handler's
   state peek (the two state names collide with the scans', so routing is by pending kind). */
__attribute__((noinline)) APP_FN void
    handle_agps_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    static AgpsWireResult wire_result;
    feb_cbor_status_t status =
        feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        return;
    }
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->u.agps.request_id = (uint32_t)status_payload.request_id;
    if(text_matches(status_payload.state, status_payload.state_len, AGPS_WIRE_STATE_PARTIAL)) {
        event->type = AppEventAgpsPartial;
    } else if(
        status_payload.has_result &&
        agps_wire_decode_result(
            status_payload.result_span, status_payload.result_span_len, &wire_result)) {
        event->type = AppEventAgpsResult;
        event->u.agps.sent = wire_result.sent;
        event->u.agps.acked = wire_result.acked;
        event->u.agps.nacked = wire_result.nacked;
        event->u.agps.timeout = wire_result.timeout;
    } else {
        post_agps_error(app, AgpsErrorRejected);
        return;
    }
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* Indexed by AgpsError. Fixed-width rows (not a const char* table): no pointer relocations to
   keep resident, FAP heap H04. */
static const char agps_error_text[AgpsErrorBadClock + 1][22] = {
    "A-GPS error", /* AgpsErrorNone */
    "No A-GPS file on SD",
    "A-GPS data expired",
    "No A-GPS window",
    "A-GPS file invalid",
    "SD read failed",
    "ESP32 not connected",
    "Send failed",
    "ESP32 busy",
    "ESP32 rejected data",
    "No reply from ESP32",
    "Low memory, retry",
    "Flipper clock not set",
};

/* Replaces the GPS screen body while agps_state != Idle. */
static void draw_agps_panel(Canvas* canvas, const Esp32App* app) {
    char line[48];
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "A-GPS");
    canvas_set_font(canvas, FontSecondary);
    if(app->agps_state == AgpsStateSending) {
        canvas_draw_str(canvas, 2, 22, "Sending to ESP32...");
        snprintf(line, sizeof(line), "%u/%u bytes", (unsigned)app->agps_off, (unsigned)app->agps_total);
        canvas_draw_str(canvas, 2, 32, line);
        canvas_draw_str(canvas, 2, 52, "Back: cancel");
    } else if(app->agps_state == AgpsStateWaitingReply) {
        canvas_draw_str(canvas, 2, 22, "Loading into GPS...");
        canvas_draw_str(canvas, 2, 52, "Back: cancel");
    } else if(app->agps_state == AgpsStateDone) {
        snprintf(
            line,
            sizeof(line),
            "A-GPS: %u/%u accepted",
            (unsigned)app->agps_acked,
            (unsigned)app->agps_sent);
        canvas_draw_str(canvas, 2, 22, line);
        if(app->agps_nacked != 0 || app->agps_timeout != 0) {
            snprintf(
                line,
                sizeof(line),
                "nack=%u timeout=%u",
                (unsigned)app->agps_nacked,
                (unsigned)app->agps_timeout);
            canvas_draw_str(canvas, 2, 32, line);
        }
        canvas_draw_str(canvas, 2, 52, "OK/Back: close");
    } else {
        canvas_draw_str(canvas, 2, 22, agps_error_text[app->agps_error < AgpsErrorBadClock + 1 ? app->agps_error : AgpsErrorNone]);
        if(app->agps_error == AgpsErrorExpired) {
            DateTime dt;
            datetime_timestamp_to_datetime(app->agps_date_unix, &dt);
            snprintf(
                line, sizeof(line), "(ended %04u-%02u-%02u)", dt.year, dt.month, dt.day);
            canvas_draw_str(canvas, 2, 32, line);
        }
        canvas_draw_str(canvas, 2, 52, "OK/Back: close");
    }
}

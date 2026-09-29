#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif
/* ---- `gps` capability (docs/PROTOCOL.md "`gps` command and status payloads", frozen
   2026-09-12) ---- poll-only status query, no scan lifecycle and no busy/error concept of
   its own (PROTOCOL.md: "a `gps` command never blocks on or conflicts with wifi_scan/
   ble_scan/wardriving"), so unlike wifi_scan/ble_scan there is no PendingCommandKind entry
   for it and handle_runtime_error() never routes anything here. */
static void post_gps_status(
    Esp32App* app, GpsFixState state, const feb_gps_result_payload_t* result) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventGpsStatus;
    event->u.gps.state = (uint8_t)state;
    if(state == GpsFixStateFix && result != NULL) {
        event->u.gps.lat_e7_offset = result->lat_e7_offset;
        event->u.gps.lon_e7_offset = result->lon_e7_offset;
        event->u.gps.fix_quality = result->fix_quality;
        event->u.gps.satellites = result->satellites;
        event->u.gps.hdop_e1 = result->hdop_e1;
        event->u.gps.utc_timestamp_s = result->utc_timestamp_s;
        event->u.gps.altitude_dm_offset = result->altitude_dm_offset;
        event->u.gps.speed_e1_kmh = result->speed_e1_kmh;
    }
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* `status` for `gps` (docs/PROTOCOL.md): single-shot, `state` one of "no_signal"/
   "acquiring"/"fix"; `result` present only for "fix". Routed here by profile_event_handler's
   status dispatch matching directly on the state text -- gps's three states are never
   ambiguous with wifi_scan/ble_scan's ("partial"/"complete") or wardriving's own
   ("started"/"data"/"stopped"), same convention wardriving's own dispatch already relies on. */
APP_FN void
    handle_gps_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    feb_cbor_status_t status = feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "gps status payload decode failed: %d; dropping", status);
        return;
    }
    GpsFixState state;
    if(text_matches(status_payload.state, status_payload.state_len, "no_signal")) {
        state = GpsFixStateNoSignal;
    } else if(text_matches(status_payload.state, status_payload.state_len, "acquiring")) {
        state = GpsFixStateAcquiring;
    } else if(text_matches(status_payload.state, status_payload.state_len, "fix")) {
        state = GpsFixStateFix;
    } else {
        FURI_LOG_W(
            TAG,
            "gps status: unexpected state '%.*s'; dropping",
            (int)status_payload.state_len,
            status_payload.state);
        return;
    }
    if(state != GpsFixStateFix) {
        post_gps_status(app, state, NULL);
        return;
    }
    if(!status_payload.has_result) {
        FURI_LOG_W(TAG, "gps status: state=fix but no result; dropping");
        return;
    }
    feb_gps_result_payload_t* result = &app_shared_status_result.gps;
    feb_cbor_status_t result_status = feb_cbor_decode_gps_result_payload(
        status_payload.result_span, status_payload.result_span_len, result);
    if(result_status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "gps status.result decode failed: %d; dropping", result_status);
        return;
    }
    post_gps_status(app, state, result);
}

/* Sends the `gps` `command` (capability="gps", fresh request_id, always-empty arguments per
   docs/PROTOCOL.md). Unlike wifi_scan/ble_scan (button-triggered) this is triggered by
   app_gps_poll_timer's periodic tick while the Wardriving screen is open, but the send itself
   still only ever runs on this app's own main thread (the timer callback just posts
   AppEventGpsPollTick; the main loop's handler for it calls this) -- same
   no-cross-thread-race argument as send_wifi_scan_command()'s own comment, extended to cover
   the timer thread as a third possible caller alongside the main thread and the BLE thread.
   Uses the shared app_cmd_payload_buf/app_cmd_ciphertext_buf/app_cmd_record_buf declared with
   wifi_scan's command scratch above -- same main-thread-only, single-in-flight
   reasoning. */
static uint64_t gps_next_request_id = 1;

APP_FN bool send_gps_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    uint8_t arguments_buf[2];
    size_t arguments_len = feb_cbor_encode_map_header(arguments_buf, sizeof(arguments_buf), 0);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "gps command: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "gps",
        .capability_len = sizeof("gps") - 1,
        .request_id = gps_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        app_cmd_payload_buf, sizeof(app_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "gps command: payload encode failed");
        return false;
    }
    size_t record_len = session_send_encrypted_command(
        "gps command",
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
    if(!send_pairing_record(profile, app_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "gps command: send failed");
        return false;
    }
    FURI_LOG_I(TAG, "gps command sent (request_id=%llu)", (unsigned long long)command.request_id);
    return true;
}

APP_FN void draw_gps_screen(Canvas* canvas, const Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "GPS");
    canvas_set_font(canvas, FontSecondary);

    bool has_fix = app->gps_status_known && app->gps_state == GpsFixStateFix;
    const char* fix_label = has_fix ? "Y" : "N";

    /* Row y-coordinates below: 22/32/42/52, matching the compact screen layout with no footer. */
    char line[64];
    snprintf(line, sizeof(line), "Fix: %s", fix_label);
    canvas_draw_str(canvas, 2, 22, line);

    if(has_fix) {
        /* Same lat/lon recovery formula as wardriving_csv.c's own conversion
           (docs/PROTOCOL.md's lat_e7_offset/lon_e7_offset field definitions) -- kept in sync
           by hand, since that module has no Furi dependency to share a helper through. */
        double lat =
            ((double)(int64_t)app->gps_lat_e7_offset - (double)900000000) / (double)10000000;
        double lon =
            ((double)(int64_t)app->gps_lon_e7_offset - (double)1800000000) / (double)10000000;
        snprintf(line, sizeof(line), "%.5f,%.5f", lat, lon);
    } else {
        snprintf(line, sizeof(line), "--");
    }
    canvas_draw_str(canvas, 2, 32, line);

    if(has_fix) {
        DateTime dt;
        datetime_timestamp_to_datetime((uint32_t)app->gps_utc_timestamp_s, &dt);
        snprintf(
            line,
            sizeof(line),
            "%04u-%02u-%02u %02u:%02u:%02u",
            dt.year,
            dt.month,
            dt.day,
            dt.hour,
            dt.minute,
            dt.second);
    } else {
        snprintf(line, sizeof(line), "--");
    }
    canvas_draw_str(canvas, 2, 42, line);

    /* Same offset-recovery convention as the Lat/Lon row above (cbor_gps.h's
       FEB_GPS_ALTITUDE_DM_OFFSET), decimeters -> meters with one decimal place retained.
       Speed (docs/WARDRIVING_REDESIGN.md, 2026-09-21) shares this same row -- the screen's
       fixed 22/32/42/52 row budget has no spare row, and this line already reserved a
       "Speed: --" placeholder for exactly this field; heading parsing from RMC remains
       backlogged (docs/BACKLOG.md). */
    if(has_fix) {
        double altitude_m =
            ((double)(int64_t)app->gps_altitude_dm_offset - (double)1000000) / (double)10;
        double speed_kmh = (double)app->gps_speed_e1_kmh / (double)10;
        snprintf(line, sizeof(line), "Alt: %.1fm  Spd: %.1f km/h", altitude_m, speed_kmh);
    } else {
        snprintf(line, sizeof(line), "Alt: --  Spd: --");
    }
    canvas_draw_str(canvas, 2, 52, line);
}


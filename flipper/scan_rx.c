#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif
static uint64_t wifi_scan_next_request_id = 1;
static uint64_t ble_scan_next_request_id = 1;
/* Per-AP display state, accumulated across one or more `status` records for the results
   view. Written directly from profile_event_handler's BleEventWorker call chain under
   app_wardriving_state_mutex (copy_wifi_scan_ap_locked(), HP-08 -- one queued event per `status`
   record now, not one per AP, to keep a large/fast batch from silently overflowing this app's
   8-deep queue), and read from both the main thread (input handling) and the GUI thread
   (draw_wifi_scan_results()) under the same mutex. Kept static and off the stack-resident
   Esp32App struct regardless: this app's own main-thread stack size isn't documented/pinned
   anywhere in this project (unlike BleEventWorker's 1280 bytes), so a several-KB array (32
   entries) is treated with the same caution rather than assumed safe as a local/struct-member. */
#define WIFI_SCAN_SSID_DISPLAY_LEN (FEB_WIFI_SCAN_SSID_MAX_LEN + 1)
#define WIFI_SCAN_PHY_DISPLAY_LEN 8
#define WIFI_SCAN_AUTH_DISPLAY_LEN 24
#define WIFI_SCAN_MAX_DISPLAY_APS FEB_WIFI_SCAN_MAX_APS_PER_RECORD

typedef struct {
    char ssid[WIFI_SCAN_SSID_DISPLAY_LEN];
    uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN];
    int32_t rssi_dbm;
    uint32_t channel;
    char phy[WIFI_SCAN_PHY_DISPLAY_LEN];
    char auth[WIFI_SCAN_AUTH_DISPLAY_LEN];
} WifiScanApDisplay;

static WifiScanApDisplay wifi_scan_aps[WIFI_SCAN_MAX_DISPLAY_APS];

/* Resets the count under app_wardriving_state_mutex (HP-08); stale entries past the new count are
   harmless since every reader bounds itself by the count read under the same lock. */
APP_FN void wifi_scan_ap_count_reset(void) {
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    wifi_scan_ap_count = 0;
    furi_mutex_release(app_wardriving_state_mutex);
}

/* Per-device display state for ble_scan, mirroring wifi_scan_aps/wifi_scan_ap_count above --
   same cross-thread argument (written under app_wardriving_state_mutex from BleEventWorker,
   read under it from the main/GUI threads), same off-stack-struct/static rationale. */
#define BLE_SCAN_NAME_DISPLAY_LEN (FEB_BLE_SCAN_NAME_MAX_LEN + 1)
#define BLE_SCAN_ADDR_TYPE_DISPLAY_LEN 8
#define BLE_SCAN_MAX_DISPLAY_DEVICES FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD

typedef struct {
    uint8_t address[FEB_BLE_SCAN_ADDRESS_LEN];
    bool has_name;
    char name[BLE_SCAN_NAME_DISPLAY_LEN];
    int32_t rssi_dbm;
    char addr_type[BLE_SCAN_ADDR_TYPE_DISPLAY_LEN];
} BleScanDeviceDisplay;

static BleScanDeviceDisplay ble_scan_devices[BLE_SCAN_MAX_DISPLAY_DEVICES];

/* Mirrors wifi_scan_ap_count_reset() above. */
APP_FN void ble_scan_device_count_reset(void) {
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    ble_scan_device_count = 0;
    furi_mutex_release(app_wardriving_state_mutex);
}
/* Sanitizes one decoded AP and copies it straight into wifi_scan_aps[] under
   app_wardriving_state_mutex (HP-08) -- ssid is raw bytes on the wire (docs/PROTOCOL.md: "not
   guaranteed valid UTF-8"), so every non-printable-ASCII byte is replaced with '.' here,
   once, rather than deferring sanitization to every later draw call. Caller must hold
   app_wardriving_state_mutex. Matches feb_wifi_scan_ap_stream_cb_t's signature (cbor_wifi_scan.h)
   so it can be passed directly as handle_wifi_scan_status()'s streaming-decode callback; `ctx`
   is unused (docs/HARDENING_BACKLOG.md H04 -- app_wardriving_state_mutex is already held around
   the whole streaming-decode call, not passed through per element). */
static void copy_wifi_scan_ap_locked(const feb_wifi_scan_ap_t* ap, void* ctx) {
    (void)ctx;
    if(wifi_scan_ap_count >= WIFI_SCAN_MAX_DISPLAY_APS) {
        return;
    }
    WifiScanApDisplay* slot = &wifi_scan_aps[wifi_scan_ap_count++];
    size_t ssid_len = ap->ssid_len > FEB_WIFI_SCAN_SSID_MAX_LEN ? FEB_WIFI_SCAN_SSID_MAX_LEN : ap->ssid_len;
    for(size_t i = 0; i < ssid_len; i++) {
        uint8_t b = ap->ssid[i];
        slot->ssid[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
    }
    slot->ssid[ssid_len] = '\0';
    memcpy(slot->bssid, ap->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    slot->rssi_dbm = (int32_t)ap->rssi_offset - 128;
    slot->channel = (uint32_t)ap->channel;
    copy_clamped_text(slot->phy, sizeof(slot->phy), ap->phy, ap->phy_len);
    copy_clamped_text(slot->auth, sizeof(slot->auth), ap->auth, ap->auth_len);
}

/* One post per `status` record instead of one per AP (HP-08): AppEventWifiScanAp now carries
   no payload (see AppEvent's own comment) and only tells the main loop to redraw -- the data
   itself is already in wifi_scan_aps[] by the time this is posted. */
static void post_wifi_scan_results_updated(Esp32App* app) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventWifiScanAp;
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

static void post_wifi_scan_complete(Esp32App* app) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventWifiScanDone;
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

APP_FN void post_wifi_scan_error(Esp32App* app, const char* message) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventWifiScanError;
    strncpy(event->u.error_message, message, sizeof(event->u.error_message) - 1);
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* `status` (docs/PROTOCOL.md's "`wifi_scan` command and status payloads"). `result` is
   generic at the outer codec layer (see cbor_codec.h) -- decoded further here, since this
   is the only capability that currently exists, into the wifi_scan-specific `{"aps": [...]}`
   shape. An unrecognized `state` (neither "partial" nor "complete") is dropped: the generic
   status codec does not validate that string, by design (see its header comment), so this
   dispatch layer is where PROTOCOL.md's two-state contract is actually enforced. */
APP_FN void
    handle_wifi_scan_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    feb_cbor_status_t status = feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "wifi_scan status payload decode failed: %d; dropping", status);
        return;
    }
    bool is_partial = text_matches(status_payload.state, status_payload.state_len, "partial");
    bool is_complete = text_matches(status_payload.state, status_payload.state_len, "complete");
    if(!is_partial && !is_complete) {
        FURI_LOG_W(
            TAG,
            "wifi_scan status: unexpected state '%.*s'; dropping",
            (int)status_payload.state_len,
            status_payload.state);
        return;
    }
    if(status_payload.has_result) {
        /* Streaming decode (docs/HARDENING_BACKLOG.md H04): copy_wifi_scan_ap_locked() is
           invoked once per AP, in wire order, only after the whole `result` payload has
           already been confirmed to decode cleanly (cbor_wifi_scan.h's own header comment on
           this decoder) -- a malformed AP anywhere in the batch drops the whole batch with
           zero copies, matching the former whole-array decode's all-or-nothing behavior.
           app_wardriving_state_mutex is held around the entire call rather than per AP, same
           critical-section shape as the former decode-then-loop split. */
        furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
        feb_cbor_status_t result_status = feb_cbor_decode_wifi_scan_result_payload_stream(
            status_payload.result_span,
            status_payload.result_span_len,
            copy_wifi_scan_ap_locked,
            NULL,
            NULL);
        furi_mutex_release(app_wardriving_state_mutex);
        if(result_status != FEB_CBOR_OK) {
            FURI_LOG_W(TAG, "wifi_scan status.result decode failed: %d; dropping", result_status);
            return;
        }
        post_wifi_scan_results_updated(app);
    }
    if(is_complete) {
        app_pending_command_kind = PendingCommandNone;
        post_wifi_scan_complete(app);
    }
}

/* ---- ble_scan capability (mirrors wifi_scan capability above, docs/PROTOCOL.md's
   "`ble_scan` command and status payloads") ---- */

/* Sanitizes one decoded device and copies it straight into ble_scan_devices[] under
   app_wardriving_state_mutex (HP-08), mirroring copy_wifi_scan_ap_locked() above -- `name`, while
   declared as a CBOR text string on the wire (docs/PROTOCOL.md), is still peer-controlled data
   with no structural guarantee every byte is printable/renderable by this canvas's font, so
   the same non-printable-ASCII-to-'.' treatment is applied here too. Caller must hold
   app_wardriving_state_mutex. Matches feb_ble_scan_device_stream_cb_t's signature
   (cbor_ble_scan.h) so it can be passed directly as handle_ble_scan_status()'s
   streaming-decode callback; `ctx` is unused (docs/HARDENING_BACKLOG.md H04 --
   app_wardriving_state_mutex is already held around the whole streaming-decode call, not passed
   through per element). */
static void copy_ble_scan_device_locked(const feb_ble_scan_device_t* device, void* ctx) {
    (void)ctx;
    if(ble_scan_device_count >= BLE_SCAN_MAX_DISPLAY_DEVICES) {
        return;
    }
    BleScanDeviceDisplay* slot = &ble_scan_devices[ble_scan_device_count++];
    memcpy(slot->address, device->address, FEB_BLE_SCAN_ADDRESS_LEN);
    slot->has_name = device->has_name;
    if(device->has_name) {
        size_t name_len =
            device->name_len > FEB_BLE_SCAN_NAME_MAX_LEN ? FEB_BLE_SCAN_NAME_MAX_LEN : device->name_len;
        for(size_t i = 0; i < name_len; i++) {
            uint8_t b = (uint8_t)device->name[i];
            slot->name[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
        }
        slot->name[name_len] = '\0';
    } else {
        slot->name[0] = '\0';
    }
    slot->rssi_dbm = (int32_t)device->rssi_offset - 128;
    copy_clamped_text(slot->addr_type, sizeof(slot->addr_type), device->addr_type, device->addr_type_len);
}

/* One post per `status` record instead of one per device (HP-08) -- see
   post_wifi_scan_results_updated()'s own comment. */
static void post_ble_scan_results_updated(Esp32App* app) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventBleScanDevice;
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

static void post_ble_scan_complete(Esp32App* app) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventBleScanDone;
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

APP_FN void post_ble_scan_error(Esp32App* app, const char* message) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventBleScanError;
    strncpy(event->u.error_message, message, sizeof(event->u.error_message) - 1);
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* `status` (docs/PROTOCOL.md's "`ble_scan` command and status payloads") -- same two-state
   ("partial"/"complete") contract as wifi_scan, enforced here for the same reason
   handle_wifi_scan_status() enforces it (the generic status codec does not validate `state`,
   by design). `result` decodes to the ble_scan-specific `{"devices": [...]}` shape. */
APP_FN void
    handle_ble_scan_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    feb_cbor_status_t status = feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "ble_scan status payload decode failed: %d; dropping", status);
        return;
    }
    bool is_partial = text_matches(status_payload.state, status_payload.state_len, "partial");
    bool is_complete = text_matches(status_payload.state, status_payload.state_len, "complete");
    if(!is_partial && !is_complete) {
        FURI_LOG_W(
            TAG,
            "ble_scan status: unexpected state '%.*s'; dropping",
            (int)status_payload.state_len,
            status_payload.state);
        return;
    }
    if(status_payload.has_result) {
        /* Streaming decode (docs/HARDENING_BACKLOG.md H04): same contract as
           handle_wifi_scan_status()'s own streaming-decode call above -- a malformed device
           anywhere in the batch drops the whole batch with zero copies. */
        furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
        feb_cbor_status_t result_status = feb_cbor_decode_ble_scan_result_payload_stream(
            status_payload.result_span,
            status_payload.result_span_len,
            copy_ble_scan_device_locked,
            NULL,
            NULL);
        furi_mutex_release(app_wardriving_state_mutex);
        if(result_status != FEB_CBOR_OK) {
            FURI_LOG_W(TAG, "ble_scan status.result decode failed: %d; dropping", result_status);
            return;
        }
        post_ble_scan_results_updated(app);
    }
    if(is_complete) {
        app_pending_command_kind = PendingCommandNone;
        post_ble_scan_complete(app);
    }
}

/* Sends the wifi_scan `command` (capability="wifi_scan", fresh request_id, always-empty
   arguments per docs/PROTOCOL.md). Unlike every other sender in this file, this one runs on
   this app's own main thread (triggered by a user Left-press on the main screen, or an
   Ok-press to re-trigger from inside the results screen, in the main loop below), not
   synchronously from inside a BLE-thread callback -- there is no incoming BLE event to key
   it off of, since "start a scan" is a user-initiated action, not a response to the peer.
   This is safe against the `app_session_key`/`app_session_seq_out`/`outgoing_message_id` statics
   this function shares with the BLE-thread-driven senders (capability_bootstrap() etc.)
   specifically because every call site gates this function on
   app->capability_has_wifi_scan, which can only become true after processing a real
   capability_response -- and that can only happen strictly after capability_bootstrap()'s
   own send (if it took the query-not-cached branch) has already returned on the BLE thread,
   since the response is itself a later, separate BLE event. If this gating condition is
   ever loosened, this reasoning needs re-examining. */
APP_FN bool send_wifi_scan_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    uint8_t arguments_buf[2];
    size_t arguments_len = feb_cbor_encode_map_header(arguments_buf, sizeof(arguments_buf), 0);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "wifi_scan command: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "wifi_scan",
        .capability_len = sizeof("wifi_scan") - 1,
        .request_id = wifi_scan_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        app_cmd_payload_buf, sizeof(app_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "wifi_scan command: payload encode failed");
        return false;
    }
    size_t record_len = session_send_encrypted_command(
        "wifi_scan command",
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
    app_pending_command_kind = PendingCommandWifiScan;
    if(!send_pairing_record(profile, app_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "wifi_scan command: send failed");
        return false;
    }
    FURI_LOG_I(TAG, "wifi_scan command sent (request_id=%llu)", (unsigned long long)command.request_id);
    return true;
}

/* Sends the ble_scan `command` (capability="ble_scan", fresh request_id, always-empty
   arguments per docs/PROTOCOL.md); mirrors send_wifi_scan_command() above exactly, including
   its main-thread/ordering-safety argument: this also runs on this app's own main thread
   (triggered by a user Right-press on the main screen, or an Ok-press to re-trigger from
   inside the results screen), not synchronously from inside a BLE-thread callback, and is
   safe against the `app_session_key`/`app_session_seq_out` statics it shares with the BLE-thread-driven
   senders (capability_bootstrap() etc.) specifically because every call site gates this
   function on app->capability_has_ble_scan, which can only become true after processing a
   real capability_response -- and that can only happen strictly after capability_bootstrap()'s
   own send (if it took the query-not-cached branch) has already returned on the BLE thread,
   since the response is itself a later, separate BLE event. If this gating condition is ever
   loosened, this reasoning needs re-examining. */
APP_FN bool send_ble_scan_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    uint8_t arguments_buf[2];
    size_t arguments_len = feb_cbor_encode_map_header(arguments_buf, sizeof(arguments_buf), 0);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "ble_scan command: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "ble_scan",
        .capability_len = sizeof("ble_scan") - 1,
        .request_id = ble_scan_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        app_cmd_payload_buf, sizeof(app_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "ble_scan command: payload encode failed");
        return false;
    }
    size_t record_len = session_send_encrypted_command(
        "ble_scan command",
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
    app_pending_command_kind = PendingCommandBleScan;
    if(!send_pairing_record(profile, app_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "ble_scan command: send failed");
        return false;
    }
    FURI_LOG_I(TAG, "ble_scan command sent (request_id=%llu)", (unsigned long long)command.request_id);
    return true;
}

/* wifi_scan results view (docs/PLAN.md's Wi-Fi scan capability follow-on step): a dedicated
   scrollable list, separate from the fixed-layout main screen above, showing every reported
   AP (scrolled with Up/Down, not truncated to a summary). `wifi_scan_aps`/`wifi_scan_ap_count`
   are this file's own statics (see their declaration comment), not Esp32App members. */
#define WIFI_SCAN_RESULTS_ROW_HEIGHT 10
#define WIFI_SCAN_RESULTS_MAX_ROWS 4
#define WIFI_SCAN_RESULTS_FIRST_ROW_Y 22
#define WIFI_SCAN_RESULTS_FOOTER_Y 62

APP_FN void draw_wifi_scan_results(Canvas* canvas, const Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    /* wifi_scan_aps[]/wifi_scan_ap_count are written from BleEventWorker under
       app_wardriving_state_mutex (HP-08); this draw callback runs on the GUI thread, so it takes
       the same mutex for the whole read, mirroring draw_mesh_log_screen()'s own pattern. */
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    size_t count = wifi_scan_ap_count;
    char header[32];
    if(app->wifi_scan_in_progress) {
        snprintf(header, sizeof(header), "Scanning...");
    } else {
        snprintf(header, sizeof(header), "Wifi scan: %u found", (unsigned)count);
    }
    canvas_draw_str(canvas, 2, 11, header);
    canvas_set_font(canvas, FontSecondary);

    uint8_t y = WIFI_SCAN_RESULTS_FIRST_ROW_Y;
    size_t max_rows = WIFI_SCAN_RESULTS_MAX_ROWS;
    if(app->wifi_scan_error_message[0] != '\0') {
        canvas_draw_str(canvas, 2, y, app->wifi_scan_error_message);
        y += WIFI_SCAN_RESULTS_ROW_HEIGHT;
        max_rows--;
    }

    for(size_t row = 0; row < max_rows; row++) {
        size_t index = app->wifi_scan_scroll_offset + row;
        if(index >= count) {
            break;
        }
        const WifiScanApDisplay* ap = &wifi_scan_aps[index];
        char line[48];
        snprintf(
            line,
            sizeof(line),
            "%s ch%lu %ldm",
            ap->ssid[0] != '\0' ? ap->ssid : "(hidden)",
            (unsigned long)ap->channel,
            (long)ap->rssi_dbm);
        canvas_draw_str(canvas, 2, (uint8_t)(y + row * WIFI_SCAN_RESULTS_ROW_HEIGHT), line);
    }

    char footer[32];
    if(count == 0) {
        snprintf(footer, sizeof(footer), "Back: exit view");
    } else {
        snprintf(
            footer,
            sizeof(footer),
            "%u/%u  Back: exit",
            (unsigned)(app->wifi_scan_scroll_offset + 1),
            (unsigned)count);
    }
    furi_mutex_release(app_wardriving_state_mutex);
    canvas_draw_str(canvas, 2, WIFI_SCAN_RESULTS_FOOTER_Y, footer);
}

/* ble_scan results view, mirroring draw_wifi_scan_results() above exactly. `address` is
   formatted as the conventional colon-separated hex pairs; `name` shows a placeholder when
   the peer advertised none (docs/PROTOCOL.md: `name` is an optional field, distinct from an
   advertised empty string). */
#define BLE_SCAN_RESULTS_ROW_HEIGHT 10
#define BLE_SCAN_RESULTS_MAX_ROWS 4
#define BLE_SCAN_RESULTS_FIRST_ROW_Y 22
#define BLE_SCAN_RESULTS_FOOTER_Y 62

APP_FN void draw_ble_scan_results(Canvas* canvas, const Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    /* Same cross-thread argument as draw_wifi_scan_results() above. */
    furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
    size_t count = ble_scan_device_count;
    char header[32];
    if(app->ble_scan_in_progress) {
        snprintf(header, sizeof(header), "Scanning...");
    } else {
        snprintf(header, sizeof(header), "Ble scan: %u found", (unsigned)count);
    }
    canvas_draw_str(canvas, 2, 11, header);
    canvas_set_font(canvas, FontSecondary);

    uint8_t y = BLE_SCAN_RESULTS_FIRST_ROW_Y;
    size_t max_rows = BLE_SCAN_RESULTS_MAX_ROWS;
    if(app->ble_scan_error_message[0] != '\0') {
        canvas_draw_str(canvas, 2, y, app->ble_scan_error_message);
        y += BLE_SCAN_RESULTS_ROW_HEIGHT;
        max_rows--;
    }

    for(size_t row = 0; row < max_rows; row++) {
        size_t index = app->ble_scan_scroll_offset + row;
        if(index >= count) {
            break;
        }
        const BleScanDeviceDisplay* device = &ble_scan_devices[index];
        char address_str[18];
        snprintf(
            address_str,
            sizeof(address_str),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            device->address[0],
            device->address[1],
            device->address[2],
            device->address[3],
            device->address[4],
            device->address[5]);
        /* Worst case: 17-byte address_str + ' ' + up to 31-byte name + ' ' + up to 5-byte
           signed rssi ("-128m") + NUL == 56 bytes; sized with margin (unlike wifi_scan's
           48-byte line, whose ssid/channel/rssi worst case is smaller) so this doesn't
           trip -Werror=format-truncation. The canvas still visually clips at screen width
           regardless of this buffer's capacity. */
        char line[64];
        snprintf(
            line,
            sizeof(line),
            "%s %s %ldm",
            address_str,
            device->has_name && device->name[0] != '\0' ? device->name : "(no name)",
            (long)device->rssi_dbm);
        canvas_draw_str(canvas, 2, (uint8_t)(y + row * BLE_SCAN_RESULTS_ROW_HEIGHT), line);
    }

    char footer[32];
    if(count == 0) {
        snprintf(footer, sizeof(footer), "Back: exit view");
    } else {
        snprintf(
            footer,
            sizeof(footer),
            "%u/%u  Back: exit",
            (unsigned)(app->ble_scan_scroll_offset + 1),
            (unsigned)count);
    }
    furi_mutex_release(app_wardriving_state_mutex);
    canvas_draw_str(canvas, 2, BLE_SCAN_RESULTS_FOOTER_Y, footer);
}


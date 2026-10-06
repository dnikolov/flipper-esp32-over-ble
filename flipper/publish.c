#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif
/* The exact fetch-and-run line from docs/WARDRIVING_PUBLISH.md's "Launch" section --
   string-literal concatenation with the pinned-commit macro, not a runtime snprintf, since
   both pieces are compile-time constants.

   HP-31: deletes any stale local copy first (SilentlyContinue -- a first-ever run with
   nothing to delete is not an error), then only runs the script if the download actually
   succeeded. `-ErrorAction Stop` alone is not enough in an interactive PowerShell 5.1
   session: a terminating error in one `;`-separated statement does NOT stop the later
   statements on the same line (unlike a script file), so `& ...` would still run against a
   stale/partial file after a failed download. `if ($?) { & ... }` gates on the previous
   statement's own success flag instead. */
static const char publish_bootstrap_command[] =
    "Remove-Item \"$env:TEMP\\publish_wardriving.ps1\" -ErrorAction SilentlyContinue; "
    "iwr -Uri 'https://raw.githubusercontent.com/dnikolov/flipper-esp32-over-ble/"
    WARDRIVING_PUBLISH_SCRIPT_COMMIT
    "/scripts/publish_wardriving.ps1' -OutFile \"$env:TEMP\\publish_wardriving.ps1\" -ErrorAction Stop; "
    "if ($?) { & \"$env:TEMP\\publish_wardriving.ps1\" -Mode ";
/* Typed right after the line above, then publish_bootstrap_tail closes the block: the host
   script's `-Mode All|Publish|Agps` (docs/AGPS.md "Host script / UI"). */
static const char publish_bootstrap_tail[] = " }";

static const char* publish_mode_name(SyncMode mode) {
    switch(mode) {
    case SyncModePublish:
        return "Publish";
    case SyncModeAgps:
        return "Agps";
    case SyncModeAll:
    default:
        return "All";
    }
}

static void publish_badusb_press_release(uint16_t keycode) {
    furi_hal_hid_kb_press(keycode);
    furi_hal_hid_kb_release(keycode);
}

/* Hardware-verified failure, 2026-09-28: typing back to back with no per-character delay
   (Unleashed ducky_string()'s 0ms default) dropped keystrokes mid-string in Windows Terminal,
   leaving an unterminated quote and a `>>` continuation prompt instead of running the
   bootstrap. Each character is now paced explicitly. */
#define PUBLISH_BADUSB_CHAR_DELAY_MS 10u
static void publish_badusb_type_string(const char* text) {
    for(size_t i = 0; text[i] != '\0'; i++) {
        uint16_t keycode = HID_ASCII_TO_KEY(text[i]);
        if(keycode != HID_KEYBOARD_NONE) {
            publish_badusb_press_release(keycode);
            furi_delay_ms(PUBLISH_BADUSB_CHAR_DELAY_MS);
        }
    }
}

/* Types the BadUSB bootstrap sequence via furi_hal_hid_kb_press/release directly, rather than
   chain-launching Unleashed's own bundled BadUSB app through the Loader service
   (docs/WARDRIVING_PUBLISH.md's Open Item 4). Investigated and rejected: loader_start()
   requires loader->app.thread to be NULL first (loader_do_is_locked(),
   applications/services/loader/loader.c) -- while this FAP is the running foreground app,
   that thread pointer is *this app's own thread*, so any loader_start() call from in here
   fails with LoaderStatusErrorAppStarted ("please close ... first"). loader_enqueue_launch()
   does work without that restriction, but only fires *after* this app fully exits -- which
   is incompatible with this screen's own requirement to stay resident and poll for the
   publish result once BadUSB has run. Direct HID typing is therefore the only option that
   keeps this app in the foreground throughout. Modeled on Unleashed's own
   applications/main/bad_usb/resources/badusb/examples/Install_qFlipper_windows.txt (GUI r ->
   powershell -> Enter opens a fresh, known-focused console before anything is typed into it,
   which is what makes blind keystroke injection safe to do unattended here).

   Runs synchronously on this app's own main thread (the input-dispatch loop in
   flipper_esp32_over_ble_app() below), never on BleEventWorker, so the 1280-byte stack
   budget does not apply to the few-hundred-byte publish_bootstrap_command buffer above.
   Returns false only if the USB personality switch itself failed (furi_hal_usb_set_config
   returning false) -- the caller shows an immediate failure rather than entering the
   "waiting for publish" state, since without HID the script's bootstrap was never typed. */
/* Hardware-verified fix, 2026-09-18: a direct HID -> usb_if_prev switch never actually
   re-enumerated as CDC/serial on the host -- the publish host script waited the full 20s
   port-discovery timeout and never saw the Flipper come back. Unleashed's own BadUSB app
   (applications/main/bad_usb/bad_usb_app.c's alloc/free, helpers/bad_usb_hid.c's init/deinit)
   never switches directly between two non-NULL USB interfaces either way -- it always detaches
   first (furi_hal_usb_set_config(NULL, NULL)) before attaching the next one. Matched that
   shape here on both transitions, with a short settle delay for the detach to actually take
   before the next attach. */
static bool publish_trigger_badusb(SyncMode mode) {
    FuriHalUsbInterface* usb_if_prev = furi_hal_usb_get_config();
    furi_hal_usb_set_config(NULL, NULL);
    furi_delay_ms(200);
    if(!furi_hal_usb_set_config(&usb_hid, NULL)) {
        FURI_LOG_E(TAG, "Publish: failed to switch USB to HID");
        furi_hal_usb_set_config(usb_if_prev, NULL);
        return false;
    }
    furi_delay_ms(2000);

    publish_badusb_press_release(HID_KEYBOARD_R | KEY_MOD_LEFT_GUI);
    furi_delay_ms(1000);
    /* HP-16: plain "powershell" launches under the host's default execution policy, which on
       a stock Windows account is Restricted -- the downloaded script then fails to run and
       the Flipper times out with no useful signal. -NoProfile also skips the user's own
       profile script, shaving a little launch time. */
    publish_badusb_type_string("powershell -NoProfile -ExecutionPolicy Bypass");
    publish_badusb_press_release(HID_KEYBOARD_RETURN);
    /* Windows 11 opens PowerShell inside Windows Terminal, which takes well over the old
       1200 ms to accept input -- keystrokes typed before that are silently lost (the head of
       the bootstrap line went missing, 2026-09-28). */
    furi_delay_ms(3500);
    publish_badusb_type_string(publish_bootstrap_command);
    publish_badusb_type_string(publish_mode_name(mode));
    publish_badusb_type_string(publish_bootstrap_tail);
    publish_badusb_press_release(HID_KEYBOARD_RETURN);
    furi_delay_ms(300);

    furi_hal_hid_kb_release_all();
    furi_hal_usb_set_config(NULL, NULL);
    furi_delay_ms(200);
    if(!furi_hal_usb_set_config(usb_if_prev, NULL)) {
        FURI_LOG_E(TAG, "Publish: failed to restore previous USB config");
    }
    return true;
}

static bool publish_str_eq(const char* str, size_t str_len, const char* literal) {
    size_t literal_len = strlen(literal);
    return str_len == literal_len && memcmp(str, literal, literal_len) == 0;
}

APP_FN uint32_t publish_parse_uint(const char* value, size_t value_len) {
    uint32_t result = 0;
    for(size_t i = 0; i < value_len; i++) {
        char c = value[i];
        if(c < '0' || c > '9') {
            break;
        }
        result = result * 10u + (uint32_t)(c - '0');
    }
    return result;
}

/* Parses wardriving_publish_result.txt (flat `key=value` lines, docs/WARDRIVING_PUBLISH.md
   "Result handling") -- the host script's own format, not CBOR, since this codebase has no
   JSON decoder and the host script deliberately avoided needing one either. `buf` is
   file-scope static (see its own declaration) since this whole call chain runs on this app's
   main thread with no reentrancy, matching this file's usual static-buffer convention. */
static void publish_parse_result(Esp32App* app, const char* buf, size_t len) {
    app->publish_imported = 0;
    app->publish_captured = 0;
    app->publish_updated = 0;
    app->publish_duplicates = 0;
    app->publish_no_gps = 0;
    app->publish_bad_rows = 0;
    app->publish_fail_message[0] = '\0';

    bool status_ok = false;
    bool status_fail = false;
    bool status_nothing = false;

    size_t pos = 0;
    while(pos < len) {
        size_t line_start = pos;
        while(pos < len && buf[pos] != '\n' && buf[pos] != '\r') {
            pos++;
        }
        size_t line_len = pos - line_start;
        while(pos < len && (buf[pos] == '\n' || buf[pos] == '\r')) {
            pos++;
        }
        if(line_len == 0) {
            continue;
        }

        const char* line = buf + line_start;
        const char* eq = memchr(line, '=', line_len);
        if(!eq) {
            continue;
        }
        size_t key_len = (size_t)(eq - line);
        const char* value = eq + 1;
        size_t value_len = line_len - key_len - 1;

        if(publish_str_eq(line, key_len, "status")) {
            status_ok = publish_str_eq(value, value_len, "ok");
            status_fail = publish_str_eq(value, value_len, "fail");
            status_nothing = publish_str_eq(value, value_len, "nothing_to_publish");
        } else if(publish_str_eq(line, key_len, "imported")) {
            app->publish_imported = publish_parse_uint(value, value_len);
        } else if(publish_str_eq(line, key_len, "captured")) {
            app->publish_captured = publish_parse_uint(value, value_len);
        } else if(publish_str_eq(line, key_len, "updated")) {
            app->publish_updated = publish_parse_uint(value, value_len);
        } else if(publish_str_eq(line, key_len, "duplicates")) {
            app->publish_duplicates = publish_parse_uint(value, value_len);
        } else if(publish_str_eq(line, key_len, "no_gps")) {
            app->publish_no_gps = publish_parse_uint(value, value_len);
        } else if(publish_str_eq(line, key_len, "bad_rows")) {
            app->publish_bad_rows = publish_parse_uint(value, value_len);
        } else if(publish_str_eq(line, key_len, "message")) {
            size_t copy_len = value_len < sizeof(app->publish_fail_message) - 1 ?
                                   value_len :
                                   sizeof(app->publish_fail_message) - 1;
            memcpy(app->publish_fail_message, value, copy_len);
            app->publish_fail_message[copy_len] = '\0';
        }
    }

    if(status_ok) {
        app->publish_outcome = PublishOutcomeOk;
    } else if(status_nothing) {
        app->publish_outcome = PublishOutcomeNothingToPublish;
    } else {
        app->publish_outcome = PublishOutcomeFail;
        if(!status_fail || app->publish_fail_message[0] == '\0') {
            strncpy(
                app->publish_fail_message,
                status_fail ? "Publish failed" : "Unrecognized result file",
                sizeof(app->publish_fail_message) - 1);
            app->publish_fail_message[sizeof(app->publish_fail_message) - 1] = '\0';
        }
    }
}

/* Returns true once this result is final (either parsed, or a real open failure) -- false
   means "not ready yet, keep polling". A confirmed real race, not a hypothetical one: the
   host script's `storage write_chunk` CLI command (applications/services/storage/
   storage_cli.c's storage_cli_write_chunk()) opens with FSOM_OPEN_APPEND, which makes the
   file exist at 0 bytes for the brief window between that open and the single
   storage_file_write() call that follows it -- a poll landing in that window must not be
   read as a final, unreadable-file failure. */
/* agps_sync_result.txt (docs/AGPS.md): first line `OK <windows> <last_valid_to>` or
   `ERR <message>`; an unparseable file is reported as a failure, not ignored. */
static void sync_parse_agps_result(Esp32App* app, const char* buf, size_t len) {
    bool ok = false;
    app->sync_agps_message[0] = '\0';
    if(agps_parse_sync_result(
           buf,
           len,
           &ok,
           &app->sync_agps_windows,
           &app->sync_agps_valid_to,
           app->sync_agps_message,
           sizeof(app->sync_agps_message))) {
        app->sync_agps_outcome = ok ? PublishOutcomeOk : PublishOutcomeFail;
        if(!ok && app->sync_agps_message[0] == '\0') {
            strncpy(app->sync_agps_message, "A-GPS sync failed", sizeof(app->sync_agps_message) - 1);
        }
    } else {
        app->sync_agps_outcome = PublishOutcomeFail;
        strncpy(
            app->sync_agps_message, "Unrecognized A-GPS result", sizeof(app->sync_agps_message) - 1);
    }
}

static bool publish_try_read_result(Esp32App* app, const char* path, bool agps) {
    static char buf[FEB_PUBLISH_RESULT_MAX_LEN];
    /* Polled every FEB_PUBLISH_POLL_PERIOD_MS for up to FEB_PUBLISH_POLL_TIMEOUT_MS, so this is
       the one open/close pair in this file that repeats on a timer rather than happening once
       per session -- worth the margin check for that alone, and the reason its log line needs
       the one-shot latch even more than the other two call sites do. Returning false here just
       means "result not readable yet"; the poll loop retries on its next tick, and the publish
       times out normally if the heap never recovers. Note publish_start() has already stopped
       this app's BLE profile for the duration of the transfer (H04's 2026-09-26 mitigation), so
       the heap is at its roomiest here -- a deferral at this call site would be a strong signal
       that something outside this app is holding the heap down. */
    static bool low_heap_logged;
    if(!storage_open_heap_margin_ok("publish result", &low_heap_logged)) {
        return false;
    }
    File* file = storage_file_alloc(app->storage);
    bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    size_t read_len = 0;
    if(ok) {
        uint64_t size = storage_file_size(file);
        size_t cap = size > sizeof(buf) ? sizeof(buf) : (size_t)size;
        read_len = storage_file_read(file, buf, cap);
    }
    storage_file_close(file);
    storage_file_free(file);

    if(!ok && agps) {
        app->sync_agps_outcome = PublishOutcomeFail;
        strncpy(
            app->sync_agps_message, "Result file unreadable", sizeof(app->sync_agps_message) - 1);
        return true;
    }
    if(!ok) {
        app->publish_outcome = PublishOutcomeFail;
        strncpy(
            app->publish_fail_message,
            "Result file unreadable",
            sizeof(app->publish_fail_message) - 1);
        app->publish_fail_message[sizeof(app->publish_fail_message) - 1] = '\0';
        return true;
    }
    if(read_len == 0) {
        return false;
    }
    if(agps) {
        sync_parse_agps_result(app, buf, read_len);
    } else {
        publish_parse_result(app, buf, read_len);
    }
    return true;
}

/* Ends the "waiting for publish" state and, if this publish run stopped the BLE profile to
   relieve heap pressure (see publish_start()'s own comment), restarts it now that the
   transfer has concluded one way or another. Shared by publish_poll_check()'s two terminal
   branches and the Publish screen's Back-press cancel handler. */
APP_FN void publish_finish_waiting(Esp32App* app) {
    furi_timer_stop(app_publish_poll_timer);
    app->publish_waiting = false;
    if(app->publish_bt_stopped) {
        /* CSV is still closed at this point (HP-09) -- catches a host-script archive/rename
           that just happened during the transfer. */
        wardriving_csv_count_refresh(app);
        start_profile(app);
        app->publish_bt_stopped = false;
    }
}

/* Called from the main loop's AppEventPublishPollTick handler, only while AppScreenPublish is
   showing "waiting for publish..." (see that handler's own guard). */
APP_FN void publish_poll_check(Esp32App* app) {
    static char result_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
    bool want_publish = app->sync_mode != SyncModeAgps;
    bool want_agps = app->sync_mode != SyncModePublish;
    if(want_publish && !app->sync_publish_done &&
       build_app_data_path(
           result_path, sizeof(result_path), FEB_WARDRIVING_PUBLISH_RESULT_FILENAME) &&
       storage_file_exists(app->storage, result_path) &&
       publish_try_read_result(app, result_path, false)) {
        app->sync_publish_done = true;
    }
    if(want_agps && !app->sync_agps_done &&
       build_app_data_path(result_path, sizeof(result_path), FEB_AGPS_SYNC_RESULT_FILENAME) &&
       storage_file_exists(app->storage, result_path) &&
       publish_try_read_result(app, result_path, true)) {
        app->sync_agps_done = true;
    }
    if((!want_publish || app->sync_publish_done) && (!want_agps || app->sync_agps_done)) {
        publish_finish_waiting(app);
        return;
    }
    app->publish_poll_elapsed_ms += FEB_PUBLISH_POLL_PERIOD_MS;
    uint32_t limit_ms = app->sync_mode == SyncModePublish ? FEB_PUBLISH_POLL_TIMEOUT_MS :
                        app->sync_mode == SyncModeAgps ?
                                                         FEB_SYNC_AGPS_TIMEOUT_MS :
                                                         FEB_PUBLISH_POLL_TIMEOUT_MS +
                                                             FEB_SYNC_AGPS_TIMEOUT_MS;
    if(app->publish_poll_elapsed_ms >= limit_ms) {
        publish_finish_waiting(app);
        if(want_publish && !app->sync_publish_done) {
            app->publish_outcome = PublishOutcomeTimeout;
        }
        if(want_agps && !app->sync_agps_done) {
            app->sync_agps_outcome = PublishOutcomeTimeout;
        }
    }
}

/* OK-press handler for the idle Publish screen (docs/WARDRIVING_PUBLISH.md "Publish flow"):
   deletes any stale result file first (so a leftover result from a previous run can never be
   mistaken for this run's), triggers BadUSB, then starts polling. A failed USB-personality
   switch shows an immediate failure instead of entering the waiting state, since the
   bootstrap was never typed in that case. */
APP_FN void publish_start(Esp32App* app, SyncMode mode) {
    app->sync_mode = mode;
    app->sync_publish_done = false;
    app->sync_agps_done = false;
    app->publish_outcome = PublishOutcomeNone;
    app->sync_agps_outcome = PublishOutcomeNone;
    app->publish_fail_message[0] = '\0';
    app->sync_agps_message[0] = '\0';

    char result_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
    if(mode != SyncModeAgps &&
       build_app_data_path(
           result_path, sizeof(result_path), FEB_WARDRIVING_PUBLISH_RESULT_FILENAME)) {
        storage_common_remove(app->storage, result_path);
    }
    if(mode != SyncModePublish &&
       build_app_data_path(result_path, sizeof(result_path), FEB_AGPS_SYNC_RESULT_FILENAME)) {
        storage_common_remove(app->storage, result_path);
    }

    if(!publish_trigger_badusb(mode)) {
        const char* failure = "Could not switch USB to HID";
        if(mode == SyncModeAgps) {
            app->sync_agps_outcome = PublishOutcomeFail;
            strncpy(app->sync_agps_message, failure, sizeof(app->sync_agps_message) - 1);
        } else {
            app->publish_outcome = PublishOutcomeFail;
            strncpy(app->publish_fail_message, failure, sizeof(app->publish_fail_message) - 1);
            app->publish_fail_message[sizeof(app->publish_fail_message) - 1] = '\0';
        }
        return;
    }

    /* Publishing needs no ESP32 connection at all -- drop this app's own BLE profile for the
       transfer to free the GATT stack's heap (docs/HARDENING_BACKLOG.md H04, 2026-09-26: a
       real out-of-memory crash during a large-CSV publish). */
    app->publish_bt_stopped = false;
    if(app->profile) {
        stop_ble_profile(app);
        app->publish_bt_stopped = true;
    }
    /* Both close functions take app_wardriving_state_mutex internally. Without this, the CSV/
       mesh-log FatFS handles stay open across the whole publish transfer, and the host
       script's CLI `storage` calls on the same paths block forever (HP-09). */
    wardriving_csv_close(app);
    mesh_log_close(app);

    app->publish_waiting = true;
    app->publish_poll_elapsed_ms = 0;
    furi_timer_stop(app_publish_poll_timer);
    furi_timer_start(app_publish_poll_timer, furi_ms_to_ticks(FEB_PUBLISH_POLL_PERIOD_MS));
}

static void wrap_field_rows(
    char rows[][64],
    size_t rows_cap,
    const char* label,
    const char* value,
    size_t* out_count) {
    *out_count = 0u;
    if(rows_cap == 0u) {
        return;
    }

    size_t value_len = strlen(value);
    if(value_len == 0u) {
        snprintf(rows[0], 64, "%s", label);
        *out_count = 1u;
        return;
    }

    size_t pos = 0u;
    size_t line_index = 0u;
    size_t label_len = strlen(label);
    while(pos < value_len && line_index < rows_cap) {
        size_t width = 22u - (line_index == 0u ? label_len : 2u);
        if(width > 20u) {
            width = 20u;
        }
        size_t chunk = value_len - pos;
        if(chunk > width) {
            chunk = width;
            while(chunk > 1u && value[pos + chunk - 1u] != ' ') {
                chunk--;
            }
            if(chunk == 1u && value[pos] != ' ') {
                chunk = width;
            }
        }
        snprintf(
            rows[line_index],
            64,
            "%s%.*s",
            line_index == 0u ? label : "  ",
            (int)chunk,
            value + pos);
        pos += chunk;
        while(pos < value_len && value[pos] == ' ') {
            pos++;
        }
        line_index++;
    }
    *out_count = line_index;
}

/* docs/WARDRIVING_PUBLISH.md "Publish flow" -- three distinct states, matching this app's own
   "don't conflate states" UI convention: idle instructions, waiting-with-poll, and outcome
   (one of PublishOutcome's four real values). Row y-coordinates match draw_gps_screen's own
   dense 22/32/42/52 layout, no separate footer row. */
/* Publish-mode outcome rows (the pre-Sync screen, unchanged). */
static void draw_publish_outcome(Canvas* canvas, Esp32App* app) {
    if(app->publish_outcome == PublishOutcomeOk) {
        char line[32];
        canvas_draw_str(canvas, 2, 22, "Publish OK");
        snprintf(line, sizeof(line), "imp=%lu dup=%lu",
                 (unsigned long)app->publish_imported, (unsigned long)app->publish_duplicates);
        canvas_draw_str(canvas, 2, 32, line);
        snprintf(line, sizeof(line), "cap=%lu upd=%lu",
                 (unsigned long)app->publish_captured, (unsigned long)app->publish_updated);
        canvas_draw_str(canvas, 2, 42, line);
        snprintf(line, sizeof(line), "nogps=%lu bad=%lu",
                 (unsigned long)app->publish_no_gps, (unsigned long)app->publish_bad_rows);
        canvas_draw_str(canvas, 2, 52, line);
        /* The host script writes a short note into `message=` even on a successful upload
           (e.g. archiving skipped/partial because new data arrived mid-transfer) --
           publish_parse_result() already copies it into publish_fail_message regardless of
           outcome (it isn't fail-only despite the field's name). Shown at the footer row,
           relying on the canvas's own width clipping like every other results screen in this
           file (see draw_ble_scan_results()'s identical comment); no new buffer, this just
           draws the already-parsed field directly. */
        if(app->publish_fail_message[0] != '\0') {
            canvas_draw_str(canvas, 2, 62, app->publish_fail_message);
        }
        return;
    }

    if(app->publish_outcome == PublishOutcomeNothingToPublish) {
        canvas_draw_str(canvas, 2, 22, "No new data");
        canvas_draw_str(canvas, 2, 32, "to publish.");
        canvas_draw_str(canvas, 2, 52, "Back: return");
        return;
    }

    if(app->publish_outcome == PublishOutcomeTimeout) {
        canvas_draw_str(canvas, 2, 22, "Timed out waiting");
        canvas_draw_str(canvas, 2, 32, "for publish result.");
        canvas_draw_str(canvas, 2, 52, "Back: return");
        return;
    }

    /* PublishOutcomeFail: word-wrap the free-text message (host-script text or this app's
       own short fixed string) with the same helper the Settings screen uses for its
       board/features rows -- an empty label just leaves every row width-22 for the message. */
    canvas_draw_str(canvas, 2, 22, "Publish failed:");
    char rows[3][64];
    size_t row_count = 0;
    wrap_field_rows(rows, 3, "", app->publish_fail_message, &row_count);
    for(size_t i = 0; i < row_count && i < 3; i++) {
        canvas_draw_str(canvas, 2, (uint8_t)(32 + i * HOME_ROW_HEIGHT), rows[i]);
    }
}

static void draw_sync_date(char* out, size_t cap, uint32_t unix_seconds) {
    DateTime dt;
    datetime_timestamp_to_datetime(unix_seconds, &dt);
    snprintf(out, cap, "%04u-%02u-%02u", dt.year, dt.month, dt.day);
}

/* One-line A-GPS result for the Sync screen: `OK <n> windows` / error text. */
static void draw_sync_agps_summary(Canvas* canvas, const Esp32App* app, uint8_t y_first) {
    char line[40];
    if(app->sync_agps_outcome == PublishOutcomeOk) {
        snprintf(line, sizeof(line), "A-GPS OK: %lu windows", (unsigned long)app->sync_agps_windows);
        canvas_draw_str(canvas, 2, y_first, line);
        char date[16];
        draw_sync_date(date, sizeof(date), app->sync_agps_valid_to);
        snprintf(line, sizeof(line), "valid until %s", date);
        canvas_draw_str(canvas, 2, (uint8_t)(y_first + HOME_ROW_HEIGHT), line);
    } else if(app->sync_agps_outcome == PublishOutcomeTimeout) {
        canvas_draw_str(canvas, 2, y_first, "A-GPS: timed out");
    } else {
        canvas_draw_str(canvas, 2, y_first, "A-GPS failed:");
        canvas_draw_str(canvas, 2, (uint8_t)(y_first + HOME_ROW_HEIGHT), app->sync_agps_message);
    }
}

/* Sync screen (renamed from Publish): menu / waiting / outcome, same three-state split as
   before. Publish mode keeps the old rows exactly; Agps and All add the A-GPS result. */
APP_FN void draw_publish_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Sync");
    canvas_set_font(canvas, FontSecondary);

    if(app->publish_waiting) {
        canvas_draw_str(canvas, 2, 22, "Waiting for sync");
        canvas_draw_str(canvas, 2, 32, "result...");
        char elapsed[32];
        uint32_t seconds = app->publish_poll_elapsed_ms / 1000u;
        snprintf(
            elapsed,
            sizeof(elapsed),
            "%lu:%02lu (watch PC)",
            (unsigned long)(seconds / 60u),
            (unsigned long)(seconds % 60u));
        canvas_draw_str(canvas, 2, 42, elapsed);
        canvas_draw_str(canvas, 2, 52, "Back: cancel");
        return;
    }

    bool have_outcome = app->publish_outcome != PublishOutcomeNone ||
                        app->sync_agps_outcome != PublishOutcomeNone;
    if(!have_outcome) {
        static const char* const labels[SyncModeCount] = {
            "Sync all", "Publish Wardriving Logs", "Sync A-GPS"};
        for(size_t i = 0; i < SyncModeCount; i++) {
            char line[32];
            snprintf(
                line, sizeof(line), "%s%s", app->sync_menu_index == (SyncMode)i ? "> " : "  ", labels[i]);
            canvas_set_font(canvas, app->sync_menu_index == (SyncMode)i ? FontPrimary : FontSecondary);
            canvas_draw_str(canvas, 2, (uint8_t)(22 + i * HOME_ROW_HEIGHT), line);
        }
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 62, "USB to PC. OK: run");
        return;
    }

    if(app->sync_mode == SyncModePublish) {
        draw_publish_outcome(canvas, app);
    } else if(app->sync_mode == SyncModeAgps) {
        draw_sync_agps_summary(canvas, app, 22);
        canvas_draw_str(canvas, 2, 52, "Back: return");
    } else {
        char line[40];
        if(app->publish_outcome == PublishOutcomeOk) {
            snprintf(
                line,
                sizeof(line),
                "Publish OK imp=%lu dup=%lu",
                (unsigned long)app->publish_imported,
                (unsigned long)app->publish_duplicates);
        } else if(app->publish_outcome == PublishOutcomeNothingToPublish) {
            snprintf(line, sizeof(line), "Publish: no new data");
        } else if(app->publish_outcome == PublishOutcomeTimeout) {
            snprintf(line, sizeof(line), "Publish: timed out");
        } else {
            snprintf(line, sizeof(line), "Publish failed");
        }
        canvas_draw_str(canvas, 2, 22, line);
        if(app->publish_outcome == PublishOutcomeFail && app->publish_fail_message[0] != '\0') {
            canvas_draw_str(canvas, 2, 32, app->publish_fail_message);
        }
        if(app->sync_agps_outcome != PublishOutcomeNone) {
            draw_sync_agps_summary(canvas, app, 42);
        }
        canvas_draw_str(canvas, 2, 62, "Back: return");
    }
}

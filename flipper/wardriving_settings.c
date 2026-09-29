#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif

/* wardriving_cooldown_ms (Esp32App) is not an enum -- it stores the wire's own
   wifi_interval_ms value directly (one of these three), matching the design doc's field
   naming. app_wardriving_cooldown_values/_labels below back the Left/Right cycling and the
   on-screen "5s"/"2s"/"0s" labels; index found by linear search since the persisted/wire
   value, not an index, is the field's own representation. */

const char* wardriving_mode_label(WardrivingMode mode) {
    switch(mode) {
    case WardrivingModeWifi:
        return "WiFi";
    case WardrivingModeBle:
        return "BLE";
    default:
        return "WiFi+BLE";
    }
}

/* Persisted token only -- WardrivingMode has no wire field of its own, it only decides
   which of wifi/ble land in the `sources` array (see send_wardriving_start_command()). */
static const char* wardriving_mode_token(WardrivingMode mode) {
    switch(mode) {
    case WardrivingModeWifi:
        return "wifi";
    case WardrivingModeBle:
        return "ble";
    default:
        return "wifi_ble";
    }
}

const char* wardriving_swelling_label(WardrivingSwelling swelling) {
    switch(swelling) {
    case WardrivingSwellingAggressive:
        return "Aggressive";
    case WardrivingSwellingSpeedBased:
        return "Speed-based";
    default:
        return "Normal";
    }
}

/* Doubles as the persisted token -- both reuse docs/PROTOCOL.md's own `wifi_swelling` wire
   strings ("normal"/"aggressive"/"speed_based") rather than a separate vocabulary. */
const char* wardriving_swelling_wire_value(WardrivingSwelling swelling) {
    switch(swelling) {
    case WardrivingSwellingAggressive:
        return "aggressive";
    case WardrivingSwellingSpeedBased:
        return "speed_based";
    default:
        return "normal";
    }
}

const char* wardriving_ble_mode_label(WardrivingBleMode mode) {
    return mode == WardrivingBleModePassive ? "Passive" : "Active";
}

/* Persisted token only -- WardrivingBleMode has no wire field of its own, it only picks
   between the existing "ble"/"ble_passive" `sources` strings. */
static const char* wardriving_ble_mode_token(WardrivingBleMode mode) {
    return mode == WardrivingBleModePassive ? "passive" : "active";
}

/* Doubles as the persisted token -- both reuse docs/PROTOCOL.md's own `country` wire
   strings ("BG"/"RoW"). */
const char* wardriving_country_wire_value(WardrivingCountry country) {
    return country == WardrivingCountryBg ? "BG" : "RoW";
}

const char* wardriving_wifi_band_label(WardrivingWifiBand band) {
    switch(band) {
    case WardrivingWifiBand5GhzFast:
        return "5GHz fast";
    case WardrivingWifiBand5GhzFull:
        return "5GHz full";
    default:
        return "2.4GHz";
    }
}

/* Doubles as the persisted token -- both reuse docs/PROTOCOL.md's own `wifi_band` wire
   strings ("2.4ghz"/"5ghz_fast"/"5ghz_full"). */
const char* wardriving_wifi_band_wire_value(WardrivingWifiBand band) {
    switch(band) {
    case WardrivingWifiBand5GhzFast:
        return "5ghz_fast";
    case WardrivingWifiBand5GhzFull:
        return "5ghz_full";
    default:
        return "2.4ghz";
    }
}

/* wardriving_settings.txt token parsers (docs/WARDRIVING_REDESIGN.md "Persistence") -- an
   unrecognized/missing token falls back to that field's own default (see
   wardriving_settings_load()), never a hard parse failure, since a stale or hand-edited file
   should degrade gracefully rather than block the app. */
static WardrivingMode wardriving_mode_from_token(const char* token, size_t len) {
    if(text_matches(token, len, "wifi")) return WardrivingModeWifi;
    if(text_matches(token, len, "ble")) return WardrivingModeBle;
    return WardrivingModeWifiBle;
}

static WardrivingSwelling wardriving_swelling_from_token(const char* token, size_t len) {
    if(text_matches(token, len, "aggressive")) return WardrivingSwellingAggressive;
    if(text_matches(token, len, "speed_based")) return WardrivingSwellingSpeedBased;
    return WardrivingSwellingNormal;
}

static WardrivingBleMode wardriving_ble_mode_from_token(const char* token, size_t len) {
    return text_matches(token, len, "passive") ? WardrivingBleModePassive : WardrivingBleModeActive;
}

static WardrivingCountry wardriving_country_from_token(const char* token, size_t len) {
    return text_matches(token, len, "BG") ? WardrivingCountryBg : WardrivingCountryRoW;
}

static WardrivingWifiBand wardriving_wifi_band_from_token(const char* token, size_t len) {
    if(text_matches(token, len, "5ghz_fast")) return WardrivingWifiBand5GhzFast;
    if(text_matches(token, len, "5ghz_full")) return WardrivingWifiBand5GhzFull;
    return WardrivingWifiBand24Ghz;
}

static uint32_t wardriving_cooldown_ms_from_token(const char* token, size_t len) {
    uint32_t value = 0;
    for(size_t i = 0; i < len; i++) {
        char c = token[i];
        if(c < '0' || c > '9') break;
        value = value * 10u + (uint32_t)(c - '0');
    }
    for(size_t i = 0; i < 3; i++) {
        if(app_wardriving_cooldown_values[i] == value) return value;
    }
    return 2000u;
}


/* app_wardriving_csv_row_count/_saved_rows/_saved_size back the Running-screen "Recs" count
   (docs/WARDRIVING_REDESIGN.md) -- declared here, ahead of wardriving_csv_file's own section
   further down, because wardriving_settings_load()/_save() need the saved pair before that
   point in the file. app_wardriving_csv_row_count is the live count, guarded by
   app_wardriving_state_mutex like the rest of the CSV export state; the saved pair is settings-
   file content, touched only on the main thread. */

/* Sets app's six wardriving settings fields to today's defaults (docs/WARDRIVING_REDESIGN.md
   "Persistence": "Default to today's old defaults if the file doesn't exist yet" -- matches
   the pre-redesign WardrivingSourceMode default, WardrivingSourceWifi2Ble: WiFi+BLE source,
   normal dwell, 2000ms cooldown, active BLE, world-safe (RoW) country, 2.4GHz-only band --
   the last per this project's "faster/safer default" bias, docs/PROTOCOL.md's `wifi_band`
   row, added 2026-09-26). Also seeds the saved CSV row-count pair to "unknown", which forces
   one recount on first refresh for a file predating these two keys. Called before attempting
   to load the persisted file, so a missing/corrupt/partially-readable file always leaves
   every field at a sane value rather than zero-initialized garbage. */
static void wardriving_settings_set_defaults(Esp32App* app) {
    app->wardriving_mode = WardrivingModeWifiBle;
    app->wardriving_swelling = WardrivingSwellingNormal;
    app->wardriving_cooldown_ms = 2000u;
    app->wardriving_ble_mode = WardrivingBleModeActive;
    app->wardriving_country = WardrivingCountryRoW;
    app->wardriving_wifi_band = WardrivingWifiBand24Ghz;
    app_wardriving_csv_saved_rows = 0;
    app_wardriving_csv_saved_size = UINT32_MAX;
}

/* Loads wardriving_settings.txt (flat `key=value` lines, same shape/parse style as
   publish_parse_result() below -- this codec has no JSON decoder). Missing file, unreadable
   file, or any unrecognized/absent individual key all degrade to that field's own default
   (set by wardriving_settings_set_defaults() first) rather than a hard failure -- a stale or
   hand-edited file should never block the app from starting. */
APP_FN void wardriving_settings_load(Esp32App* app) {
    wardriving_settings_set_defaults(app);

    static char path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
    if(!build_app_data_path(path, sizeof(path), FEB_WARDRIVING_SETTINGS_FILENAME)) {
        return;
    }
    File* file = storage_file_alloc(app->storage);
    bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    size_t read_len = 0;
    if(ok) {
        uint64_t size = storage_file_size(file);
        size_t cap = size > sizeof(app_wardriving_settings_buf) ? sizeof(app_wardriving_settings_buf) :
                                                               (size_t)size;
        read_len = storage_file_read(file, app_wardriving_settings_buf, cap);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(!ok || read_len == 0) {
        return;
    }

    size_t pos = 0;
    while(pos < read_len) {
        size_t line_start = pos;
        while(pos < read_len && app_wardriving_settings_buf[pos] != '\n' &&
              app_wardriving_settings_buf[pos] != '\r') {
            pos++;
        }
        size_t line_len = pos - line_start;
        while(pos < read_len &&
              (app_wardriving_settings_buf[pos] == '\n' || app_wardriving_settings_buf[pos] == '\r')) {
            pos++;
        }
        if(line_len == 0) {
            continue;
        }
        const char* line = app_wardriving_settings_buf + line_start;
        const char* eq = memchr(line, '=', line_len);
        if(!eq) {
            continue;
        }
        size_t key_len = (size_t)(eq - line);
        const char* value = eq + 1;
        size_t value_len = line_len - key_len - 1;

        if(text_matches(line, key_len, "mode")) {
            app->wardriving_mode = wardriving_mode_from_token(value, value_len);
        } else if(text_matches(line, key_len, "wifi_swelling")) {
            app->wardriving_swelling = wardriving_swelling_from_token(value, value_len);
        } else if(text_matches(line, key_len, "wifi_cooldown_ms")) {
            app->wardriving_cooldown_ms = wardriving_cooldown_ms_from_token(value, value_len);
        } else if(text_matches(line, key_len, "ble_mode")) {
            app->wardriving_ble_mode = wardriving_ble_mode_from_token(value, value_len);
        } else if(text_matches(line, key_len, "country")) {
            app->wardriving_country = wardriving_country_from_token(value, value_len);
        } else if(text_matches(line, key_len, "wifi_band")) {
            app->wardriving_wifi_band = wardriving_wifi_band_from_token(value, value_len);
        } else if(text_matches(line, key_len, "csv_rows")) {
            app_wardriving_csv_saved_rows = publish_parse_uint(value, value_len);
        } else if(text_matches(line, key_len, "csv_size")) {
            app_wardriving_csv_saved_size = publish_parse_uint(value, value_len);
        }
    }
}

/* Rewrites wardriving_settings.txt in full (docs/WARDRIVING_REDESIGN.md "Persistence":
   "rewritten immediately on every row-value change") -- one small file, no batching needed.
   Same atomic temp-file/verified-write/storage_file_sync()/close/rename sequence as
   pairing_storage_save()/capability_storage_save() above. Logs (not silently drops) a
   failure, matching this file's "wrap silent-failure storage APIs in positive confirmation"
   convention -- a failed save here just means the next app launch falls back to defaults,
   not a security-relevant loss. */
APP_FN void wardriving_settings_save(const Esp32App* app) {
    static char final_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
    static char tmp_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
    if(!build_app_data_path(final_path, sizeof(final_path), FEB_WARDRIVING_SETTINGS_FILENAME) ||
       !build_app_data_path(tmp_path, sizeof(tmp_path), FEB_WARDRIVING_SETTINGS_FILENAME ".tmp")) {
        FURI_LOG_E(TAG, "wardriving_settings_save: path build failed");
        return;
    }

    int written = snprintf(
        app_wardriving_settings_buf,
        sizeof(app_wardriving_settings_buf),
        "mode=%s\nwifi_swelling=%s\nwifi_cooldown_ms=%lu\nble_mode=%s\ncountry=%s\nwifi_band=%s\n"
        "csv_rows=%lu\ncsv_size=%lu\n",
        wardriving_mode_token(app->wardriving_mode),
        wardriving_swelling_wire_value(app->wardriving_swelling),
        (unsigned long)app->wardriving_cooldown_ms,
        wardriving_ble_mode_token(app->wardriving_ble_mode),
        wardriving_country_wire_value(app->wardriving_country),
        wardriving_wifi_band_wire_value(app->wardriving_wifi_band),
        (unsigned long)app_wardriving_csv_saved_rows,
        (unsigned long)app_wardriving_csv_saved_size);
    if(written <= 0 || (size_t)written >= sizeof(app_wardriving_settings_buf)) {
        FURI_LOG_E(TAG, "wardriving_settings_save: buffer too small");
        return;
    }
    size_t buf_len = (size_t)written;

    File* file = storage_file_alloc(app->storage);
    bool ok = storage_file_open(file, tmp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        size_t out_written = storage_file_write(file, app_wardriving_settings_buf, buf_len);
        ok = (out_written == buf_len) && storage_file_sync(file);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(!ok) {
        FURI_LOG_E(TAG, "wardriving_settings_save: write failed");
        storage_common_remove(app->storage, tmp_path);
        return;
    }

    storage_common_remove(app->storage, final_path);
    FS_Error rename_err = storage_common_rename(app->storage, tmp_path, final_path);
    if(rename_err != FSE_OK) {
        FURI_LOG_E(TAG, "wardriving_settings_save: rename failed: %d", rename_err);
        storage_common_remove(app->storage, tmp_path);
    }
}

/* AppEvent sits right at this file's ~100-byte static-storage threshold (104 bytes since
   the tagged-union rework -- see its own declaration) -- event is static, not stack-local,
   to keep it off the 1280-byte BleEventWorker stack (this function is reachable from
   profile_event_handler via the handle_pair_ and handle_hello/handle_client_auth
   callbacks). A static local with a designated initializer only runs that initializer once
   at program load, not per call (docs/SESSION_MEMORY.md's cmult() trap), so every field is
   explicitly reset here instead.

   app_shared_ble_event is one instance shared by every post_*() function below that only ever
   posts from inside profile_event_handler's call chain (BleEventWorker thread: synchronous,
   single-in-flight, never reentrant) -- safe to consolidate since furi_message_queue_put()
   copies the struct by value before any of these functions returns, so nothing depends on
   the buffer's contents surviving past that call. bt_status_callback/input_callback run on
   other system threads (Bt service/GuiSrv) and keep their own separate static AppEvent for
   that reason -- sharing across threads would be a real data race, not just an in-flight
   one. gps_poll_timer_callback/publish_poll_timer_callback share a third instance,
   timer_service_event (declared next to them) -- both run on the same single FreeRTOS Timer
   Service task, so the same single-in-flight argument applies to that pair specifically, even
   though it's a different thread than this one. */

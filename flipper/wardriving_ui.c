#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif
/* Wardriving running screen (docs/WARDRIVING_REDESIGN.md, 2026-09-21) -- unchanged content
   from the pre-redesign single screen's own running-state rendering: state text, Recs/
   Backlog-or-Live line, GPS fix suffix, and the existing single last-WiFi/last-BLE lines (no
   history list -- design doc decision 3). Reached only when wardriving_running_known &&
   wardriving_running (see the Home-menu OK-press handler and the AppEventWardrivingRunState
   handler, both of which route navigation using exactly that condition), so this function
   does not re-derive an "unknown" state of its own -- draw_wardriving_stopped_screen() below
   owns that case, since it's the screen a fresh/unconfirmed session actually lands on. */
APP_FN void draw_wardriving_running_screen(Canvas* canvas, const Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Wardriving: RUNNING");
    canvas_set_font(canvas, FontSecondary);

    /* Sized with margin over the two longest fields (wardriving_last_summary and
       wardriving_error_message, both up to ~40-48 real bytes) plus their literal prefixes,
       so -Werror=format-truncation's static worst-case analysis is satisfied -- see this
       project's docs/LESSONS.md for why a value that "can't really" overflow at runtime
       still needs a buffer GCC can prove is large enough. */
    char line[80];

    /* `gps` fix indicator (docs/PLAN.md "Real GPS driver...", decision 6): three-state, not
       binary -- "?" (never polled/no gps capability), "No sig"/"Acq"/"Fix" once polled at
       least once this session. Blank entirely when the board doesn't advertise `gps` at all,
       matching this file's existing capability-gating convention. */
    char gps_suffix[16];
    gps_suffix[0] = '\0';
    if(app->capability_has_gps) {
        const char* gps_label;
        if(!app->gps_status_known) {
            gps_label = "?";
        } else if(app->gps_state == GpsFixStateFix) {
            gps_label = "Fix";
        } else if(app->gps_state == GpsFixStateAcquiring) {
            gps_label = "Acq";
        } else {
            gps_label = "No sig";
        }
        snprintf(gps_suffix, sizeof(gps_suffix), "  GPS:%s", gps_label);
    }

    if(app->wardriving_backlog_remaining > 0) {
        snprintf(
            line,
            sizeof(line),
            "Recs: %lu  Backlog: %llu%s",
            (unsigned long)app->wardriving_csv_rows,
            (unsigned long long)app->wardriving_backlog_remaining,
            gps_suffix);
    } else {
        snprintf(
            line,
            sizeof(line),
            "Recs: %lu  Live%s",
            (unsigned long)app->wardriving_csv_rows,
            gps_suffix);
    }
    canvas_draw_str(canvas, 2, 22, line);

    if(app->wardriving_last_wifi_summary[0] != '\0') {
        snprintf(line, sizeof(line), "Last WiFi: %s", app->wardriving_last_wifi_summary);
        canvas_draw_str(canvas, 2, 32, line);
    }

    if(app->wardriving_last_ble_summary[0] != '\0') {
        snprintf(line, sizeof(line), "Last BLE: %s", app->wardriving_last_ble_summary);
        canvas_draw_str(canvas, 2, 42, line);
    }

    if(app->wardriving_error_message[0] != '\0') {
        snprintf(line, sizeof(line), "! %s", app->wardriving_error_message);
        canvas_draw_str(canvas, 2, 52, line);
    }

    canvas_draw_str(canvas, 2, 62, "OK: stop  Back: exit");
}

/* Stopped-screen settings rows (docs/WARDRIVING_REDESIGN.md) -- capability-gated only, not
   re-gated by each other's current value (design doc rationale #4): e.g. the WiFi Swelling
   row stays visible even while Mode=BLE is selected. Declared here, not with the
   WardrivingMode/etc. value enums up top, since this row-index concept is purely an artifact
   of this one screen's own rendering/input. */
typedef enum {
    WardrivingSettingsRowMode = 0,
    WardrivingSettingsRowSwelling,
    WardrivingSettingsRowCooldown,
    WardrivingSettingsRowBleMode,
    WardrivingSettingsRowCountry,
    WardrivingSettingsRowWifiBand,
    WardrivingSettingsRowCount,
} WardrivingSettingsRow;

#define WARDRIVING_SETTINGS_VISIBLE_ROWS 4

static bool wardriving_settings_row_visible(const Esp32App* app, WardrivingSettingsRow row) {
    switch(row) {
    case WardrivingSettingsRowMode:
        return app->capability_has_wifi_scan && app->capability_has_ble_scan;
    case WardrivingSettingsRowBleMode:
        return app->capability_has_ble_scan;
    case WardrivingSettingsRowSwelling:
    case WardrivingSettingsRowCooldown:
    case WardrivingSettingsRowCountry:
    case WardrivingSettingsRowWifiBand:
        return app->capability_has_wifi_scan;
    default:
        return false;
    }
}

/* Same "scroll follows selection" idiom as home_menu_scroll_into_view() above. */
static void wardriving_settings_scroll_into_view(Esp32App* app) {
    int total = 0;
    int rank = -1;
    for(int i = 0; i < WardrivingSettingsRowCount; i++) {
        if(!wardriving_settings_row_visible(app, (WardrivingSettingsRow)i)) continue;
        if((size_t)i == app->wardriving_settings_row) rank = total;
        total++;
    }
    if(rank < 0) return;

    size_t max_offset = (size_t)total > WARDRIVING_SETTINGS_VISIBLE_ROWS ?
                             (size_t)total - WARDRIVING_SETTINGS_VISIBLE_ROWS :
                             0;
    if(app->wardriving_settings_scroll_offset > max_offset) {
        app->wardriving_settings_scroll_offset = max_offset;
    }
    if((size_t)rank < app->wardriving_settings_scroll_offset) {
        app->wardriving_settings_scroll_offset = (size_t)rank;
    } else if((size_t)rank >= app->wardriving_settings_scroll_offset + WARDRIVING_SETTINGS_VISIBLE_ROWS) {
        app->wardriving_settings_scroll_offset = (size_t)rank - WARDRIVING_SETTINGS_VISIBLE_ROWS + 1;
    }
}

/* Same "clamp selection to a currently-visible item" idiom as home_menu_fix_selection() --
   called every draw, defensively, in case a capability set shrinks (a reconnect to a
   different board) while this screen happens to be open. */
static void wardriving_settings_fix_selection(Esp32App* app) {
    if(!wardriving_settings_row_visible(app, (WardrivingSettingsRow)app->wardriving_settings_row)) {
        for(int i = 0; i < WardrivingSettingsRowCount; i++) {
            if(wardriving_settings_row_visible(app, (WardrivingSettingsRow)i)) {
                app->wardriving_settings_row = (size_t)i;
                break;
            }
        }
    }
    wardriving_settings_scroll_into_view(app);
}

/* Same "step to the next visible item, wrap around" idiom as home_menu_step() -- relies on
   at least one row always being visible, which holds here since this screen is only
   reachable when capability_has_wardriving is true, and this capability implies at least one
   of wifi_scan/ble_scan is also advertised (docs/CAPABILITIES.md; send_wardriving_start_
   command()'s own source_count==0 guard depends on the same assumption). */
APP_FN void wardriving_settings_step(Esp32App* app, int delta) {
    int idx = (int)app->wardriving_settings_row;
    int count = WardrivingSettingsRowCount;
    while(true) {
        idx += delta;
        if(idx < 0) idx = count - 1;
        if(idx >= count) idx = 0;
        if(wardriving_settings_row_visible(app, (WardrivingSettingsRow)idx)) {
            app->wardriving_settings_row = (size_t)idx;
            break;
        }
    }
    wardriving_settings_scroll_into_view(app);
}

/* Left/Right on the Stopped screen's highlighted row -- cycles that row's own value with
   wraparound and persists the whole settings file immediately (docs/WARDRIVING_REDESIGN.md
   "Persistence": "rewritten immediately on every row-value change"). wardriving_cooldown_ms
   is not an enum (see its own Esp32App field comment), so it cycles through
   app_wardriving_cooldown_values[] by linear search instead of a modulo increment. */
APP_FN void wardriving_settings_cycle_row(Esp32App* app, int delta) {
    switch((WardrivingSettingsRow)app->wardriving_settings_row) {
    case WardrivingSettingsRowMode:
        app->wardriving_mode = (WardrivingMode)(
            ((int)app->wardriving_mode + delta + WardrivingModeCount) % WardrivingModeCount);
        break;
    case WardrivingSettingsRowSwelling:
        app->wardriving_swelling = (WardrivingSwelling)(
            ((int)app->wardriving_swelling + delta + WardrivingSwellingCount) %
            WardrivingSwellingCount);
        break;
    case WardrivingSettingsRowCooldown: {
        int idx = 1; /* falls back to 2000ms's index if the current value is somehow stale */
        for(int i = 0; i < 3; i++) {
            if(app_wardriving_cooldown_values[i] == app->wardriving_cooldown_ms) {
                idx = i;
                break;
            }
        }
        idx = (idx + delta + 3) % 3;
        app->wardriving_cooldown_ms = app_wardriving_cooldown_values[idx];
        break;
    }
    case WardrivingSettingsRowBleMode:
        app->wardriving_ble_mode = (WardrivingBleMode)(
            ((int)app->wardriving_ble_mode + delta + WardrivingBleModeCount) %
            WardrivingBleModeCount);
        break;
    case WardrivingSettingsRowCountry:
        app->wardriving_country = (WardrivingCountry)(
            ((int)app->wardriving_country + delta + WardrivingCountryCount) %
            WardrivingCountryCount);
        break;
    case WardrivingSettingsRowWifiBand:
        app->wardriving_wifi_band = (WardrivingWifiBand)(
            ((int)app->wardriving_wifi_band + delta + WardrivingWifiBandCount) %
            WardrivingWifiBandCount);
        break;
    default:
        return;
    }
    wardriving_settings_save(app);
}

static void wardriving_settings_row_text(
    const Esp32App* app,
    WardrivingSettingsRow row,
    char* label,
    size_t label_cap,
    char* value,
    size_t value_cap) {
    switch(row) {
    case WardrivingSettingsRowMode:
        snprintf(label, label_cap, "Mode");
        snprintf(value, value_cap, "%s", wardriving_mode_label(app->wardriving_mode));
        break;
    case WardrivingSettingsRowSwelling:
        snprintf(label, label_cap, "WiFi Swelling");
        snprintf(value, value_cap, "%s", wardriving_swelling_label(app->wardriving_swelling));
        break;
    case WardrivingSettingsRowCooldown: {
        int idx = 1;
        for(int i = 0; i < 3; i++) {
            if(app_wardriving_cooldown_values[i] == app->wardriving_cooldown_ms) {
                idx = i;
                break;
            }
        }
        snprintf(label, label_cap, "WiFi Cooldown");
        snprintf(value, value_cap, "%s", app_wardriving_cooldown_labels[idx]);
        break;
    }
    case WardrivingSettingsRowBleMode:
        snprintf(label, label_cap, "BLE Mode");
        snprintf(value, value_cap, "%s", wardriving_ble_mode_label(app->wardriving_ble_mode));
        break;
    case WardrivingSettingsRowCountry:
        snprintf(label, label_cap, "Country");
        snprintf(value, value_cap, "%s", wardriving_country_wire_value(app->wardriving_country));
        break;
    case WardrivingSettingsRowWifiBand:
        snprintf(label, label_cap, "WiFi Band");
        snprintf(value, value_cap, "%s", wardriving_wifi_band_label(app->wardriving_wifi_band));
        break;
    default:
        label[0] = '\0';
        value[0] = '\0';
        break;
    }
}

/* Wardriving stopped screen (docs/WARDRIVING_REDESIGN.md, 2026-09-21) -- replaces the old
   single screen's "Source: ..." Left/Right-cycled line with a real scrollable settings list,
   same 22/32/42/52 4-row window as draw_wifi_scan_results()/draw_ble_scan_results(). Shown
   whenever wardriving_running_known is false (this Flipper has no evidence either way yet,
   same "unknown" state the pre-redesign screen carried -- docs/LESSONS.md "UI must derive
   from real state") or the board is confirmed stopped; confirmed-running always lives on
   draw_wardriving_running_screen() instead (see that function's own comment). */
APP_FN void draw_wardriving_stopped_screen(Canvas* canvas, Esp32App* app) {
    wardriving_settings_fix_selection(app);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas, 2, 11, app->wardriving_running_known ? "Wardriving: stopped" : "Wardriving: unknown");
    canvas_set_font(canvas, FontSecondary);

    int rank = 0;
    for(int i = 0; i < WardrivingSettingsRowCount; i++) {
        if(!wardriving_settings_row_visible(app, (WardrivingSettingsRow)i)) continue;
        if((size_t)rank >= app->wardriving_settings_scroll_offset &&
           (size_t)rank < app->wardriving_settings_scroll_offset + WARDRIVING_SETTINGS_VISIBLE_ROWS) {
            char label[20];
            char value[20];
            wardriving_settings_row_text(
                app, (WardrivingSettingsRow)i, label, sizeof(label), value, sizeof(value));
            /* Sized for -Werror=format-truncation's static worst case: "> "(2) + label(<=19)
               + ": "(2) + value(<=19) + NUL == 43, with margin -- see docs/LESSONS.md for why
               a value that "can't really" overflow at runtime still needs a buffer GCC can
               prove is large enough. */
            char line[48];
            snprintf(
                line,
                sizeof(line),
                "%s%s: %s",
                (size_t)i == app->wardriving_settings_row ? "> " : "  ",
                label,
                value);
            uint8_t y = (uint8_t)(22 + (rank - (int)app->wardriving_settings_scroll_offset) * 10);
            canvas_draw_str(canvas, 2, y, line);
        }
        rank++;
    }

    /* Same "start"/"start (delayed)" footer label logic as before the redesign (docs/PLAN.md
       decision 6) -- unchanged: the action itself is always send_wardriving_start_command()
       regardless of gps_delayed, this never disables/greys out OK. */
    bool gps_delayed =
        app->capability_has_gps && !(app->gps_status_known && app->gps_state == GpsFixStateFix);
    const char* start_label = gps_delayed ? "start (delayed)" : "start";
    char footer_buf[40];
    snprintf(footer_buf, sizeof(footer_buf), "OK:%s L/R:val Back:exit", start_label);
    canvas_draw_str(canvas, 2, 62, footer_buf);
}


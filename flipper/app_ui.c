#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif
static const char* esp_status_text(const Esp32App* app) {
    switch(app->pairing_phase) {
    case PairingPhaseWaiting:
    case PairingPhaseExchanging:
    case PairingPhaseConfirming:
    case PairingPhaseSaving:
    case PairingPhaseAuthenticating:
    case PairingPhaseFailed:
        return "waiting";
    case PairingPhaseSessionActive:
    case PairingPhaseDone:
        return "session";
    case PairingPhaseNone:
    default:
        return "idle";
    }
}

/* Home screen layout: same proven 10px-pitch / y=62-footer convention as the
   WIFI_SCAN_RESULTS_ and BLE_SCAN_RESULTS_ constants above (see their declaration comments) --
   content rows run 22,32,42,52 (4 rows total budget) with the footer one more 10px stride
   below the last possible row, at 62. The header (has_saved_pairing + pairing-phase line,
   plus an optional capability line) consumes the first 2 or 3 of those 4 rows; whatever is
   left is the menu's visible window (1 row when the capability line is shown, since up to
   6 menu items -- Wardriving/Scan/GPS/Settings/About/Legacy -- can be visible at once and
   none of the 2/1 leftover rows fit them all, the menu must scroll rather than draw every
   visible item unconditionally. */
#define HOME_ROW_HEIGHT 10
#define HOME_FIRST_ROW_Y 22
#define HOME_FOOTER_Y 62
#define HOME_MAX_ROWS 4

static uint8_t home_menu_visible_rows(Esp32App* app) {
    UNUSED(app);
    return HOME_MAX_ROWS;
}

static bool home_menu_visible(Esp32App* app, HomeMenuItem item) {
    switch(item) {
    case HomeMenuWardriving:
        return app->pairing_phase == PairingPhaseSessionActive && app->capability_has_wardriving;
    case HomeMenuScan:
        return app->pairing_phase == PairingPhaseSessionActive &&
               (app->capability_has_wifi_scan || app->capability_has_ble_scan);
    case HomeMenuGps:
        return app->pairing_phase == PairingPhaseSessionActive && app->capability_has_gps;
    case HomeMenuMeshLog:
        /* Gated on capability_has_mesh_log, not capability_has_meshcore_scan -- this menu
           entry now opens the mesh_log backlog screen, not the old meshcore_scan live-poll
           table (docs/WARDRIVING_PUBLISH.md "Mesh node publishing"). */
        return app->pairing_phase == PairingPhaseSessionActive && app->capability_has_mesh_log;
    /* Publish is BLE-session-independent (docs/WARDRIVING_PUBLISH.md) -- reachable regardless
       of pairing/connection state, unlike Wardriving/Scan/Gps/MeshLog which require an active
       session. */
    case HomeMenuPublish:
        return true;
    default:
        return false;
    }
}

/* Keeps app->home_menu_index's rank within the currently-visible (filtered) item list inside
   the [scroll_offset, scroll_offset + visible_rows) window, the same "scroll follows
   selection" behavior draw_wifi_scan_results()/draw_ble_scan_results() get from their own
   scroll_offset + up/down clamping. Safe to call whenever the selection or the visible set
   (capability info, session state) may have changed -- it's a no-op if already in range. */
APP_FN void home_menu_scroll_into_view(Esp32App* app) {
    int total = 0;
    int rank = -1;
    for(int i = 0; i < HomeMenuCount; i++) {
        if(!home_menu_visible(app, (HomeMenuItem)i)) continue;
        if((HomeMenuItem)i == app->home_menu_index) rank = total;
        total++;
    }
    if(rank < 0) return;

    uint8_t visible_rows = home_menu_visible_rows(app);
    size_t max_offset = (size_t)total > visible_rows ? (size_t)total - visible_rows : 0;
    if(app->home_menu_scroll_offset > max_offset) {
        app->home_menu_scroll_offset = max_offset;
    }
    if((size_t)rank < app->home_menu_scroll_offset) {
        app->home_menu_scroll_offset = (size_t)rank;
    } else if((size_t)rank >= app->home_menu_scroll_offset + visible_rows) {
        app->home_menu_scroll_offset = (size_t)rank - visible_rows + 1;
    }
}

static void home_menu_step(Esp32App* app, int delta) {
    int idx = (int)app->home_menu_index;
    int count = HomeMenuCount;
    while(true) {
        idx += delta;
        if(idx < 0) idx = count - 1;
        if(idx >= count) idx = 0;
        if(home_menu_visible(app, (HomeMenuItem)idx)) {
            app->home_menu_index = (HomeMenuItem)idx;
            break;
        }
    }
    home_menu_scroll_into_view(app);
}

static void home_menu_fix_selection(Esp32App* app) {
    if(!home_menu_visible(app, app->home_menu_index)) {
        for(int i = 0; i < HomeMenuCount; i++) {
            if(home_menu_visible(app, (HomeMenuItem)i)) {
                app->home_menu_index = (HomeMenuItem)i;
                break;
            }
        }
    }
    home_menu_scroll_into_view(app);
}

static bool scan_menu_visible(Esp32App* app, ScanMenuItem item) {
    switch(item) {
    case ScanMenuWifi:
        return app->capability_has_wifi_scan;
    case ScanMenuBle:
        return app->capability_has_ble_scan;
    default:
        return false;
    }
}

static void scan_menu_step(Esp32App* app, int delta) {
    int idx = (int)app->scan_menu_index;
    int count = ScanMenuCount;
    while(true) {
        idx += delta;
        if(idx < 0) idx = count - 1;
        if(idx >= count) idx = 0;
        if(scan_menu_visible(app, (ScanMenuItem)idx)) {
            app->scan_menu_index = (ScanMenuItem)idx;
            return;
        }
    }
}

static void scan_menu_fix_selection(Esp32App* app) {
    if(!scan_menu_visible(app, app->scan_menu_index)) {
        for(int i = 0; i < ScanMenuCount; i++) {
            if(scan_menu_visible(app, (ScanMenuItem)i)) {
                app->scan_menu_index = (ScanMenuItem)i;
                return;
            }
        }
    }
}

static void draw_scan_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Scan");
    canvas_set_font(canvas, FontSecondary);

    int menu_index = 0;
    if(app->capability_has_wifi_scan) {
        char line[24];
        snprintf(line, sizeof(line), "%sWi-Fi scan", app->scan_menu_index == ScanMenuWifi ? "> " : "  ");
        canvas_draw_str(canvas, 2, 22 + 10 * menu_index, line);
        menu_index++;
    }
    if(app->capability_has_ble_scan) {
        char line[24];
        snprintf(line, sizeof(line), "%sBLE scan", app->scan_menu_index == ScanMenuBle ? "> " : "  ");
        canvas_draw_str(canvas, 2, 22 + 10 * menu_index, line);
        menu_index++;
    }

    canvas_draw_str(canvas, 2, 56, "Up/Down: move  OK: start  Back: return");
}

static void draw_home_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Home");
    canvas_set_font(canvas, FontSecondary);

    uint8_t y = HOME_FIRST_ROW_Y;
    char status_line[32];
    snprintf(
        status_line,
        sizeof(status_line),
        "Pairing:%s  ESP:%s",
        app->has_saved_pairing ? "Y" : "N",
        esp_status_text(app));
    canvas_draw_str(canvas, 2, y, status_line);
    y += HOME_ROW_HEIGHT;

    /* Leave the bottom section for the selectable menu to fill the remaining vertical space. */
    uint8_t menu_y = 32u;

    static const char* labels[HomeMenuCount] = {
        "Wardriving",
        "Publish",
        "Scan",
        "GPS",
        "Mesh Log",
    };

    uint8_t visible_rows = home_menu_visible_rows(app);
    int rank = 0;
    for(int i = 0; i < HomeMenuCount; i++) {
        if(!home_menu_visible(app, (HomeMenuItem)i)) continue;
        if((size_t)rank >= app->home_menu_scroll_offset &&
           (size_t)rank < app->home_menu_scroll_offset + visible_rows) {
            char line[32];
            snprintf(
                line,
                sizeof(line),
                "%s%s",
                app->home_menu_index == (HomeMenuItem)i ? "> " : "  ",
                labels[i]);
            canvas_set_font(
                canvas,
                app->home_menu_index == (HomeMenuItem)i ? FontPrimary : FontSecondary);
            canvas_draw_str(canvas, 2, menu_y, line);
            menu_y += HOME_ROW_HEIGHT;
        }
        rank++;
    }

    /* First-time-pairing affordance (formerly the Legacy screen's own hint line, moved here
       now that Legacy is gone) -- only while genuinely not-started (docs/LESSONS.md "UI must
       derive from real state": app->profile is non-NULL for the whole waiting/exchanging/
       confirming/authenticating ceremony, not just once a session is active, so this hint
       disappears the moment OK is pressed, not only once pairing finishes). Drawn at the same
       footer row every other results screen uses; safe from menu-row collision here since
       home_menu_visible() only shows Publish (of HomeMenuCount items) while !app->profile. */
    if(!app->profile) {
        canvas_draw_str(canvas, 2, HOME_FOOTER_Y, "OK: pair");
    }
}

APP_FN void draw_callback(Canvas* canvas, void* context) {
    Esp32App* app = context;
    /* Clear the entire viewport before every redraw; this ensures a screen transition cannot
       leave stale pixels behind when the new draw code only paints a subset of the canvas. */
    canvas_clear(canvas);

    if(app->connection_lost) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 12, "Connection lost");
    }

    if(app->screen == AppScreenHome) {
        if(!app->connection_lost) {
            home_menu_fix_selection(app);
        }
        draw_home_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenScan) {
        scan_menu_fix_selection(app);
        draw_scan_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenGps) {
        draw_gps_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenMeshLog) {
        draw_mesh_log_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenWifiScanResults) {
        draw_wifi_scan_results(canvas, app);
        return;
    }
    if(app->screen == AppScreenBleScanResults) {
        draw_ble_scan_results(canvas, app);
        return;
    }
    if(app->screen == AppScreenWardrivingStopped) {
        draw_wardriving_stopped_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenWardrivingRunning) {
        draw_wardriving_running_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenPublish) {
        draw_publish_screen(canvas, app);
        return;
    }

    home_menu_fix_selection(app);
    draw_home_screen(canvas, app);
}

APP_FN void input_callback(InputEvent* input, void* context) {
    Esp32App* app = context;
    /* static, not stack-local -- same 2026-09-12 stack-audit finding as bt_status_callback/
       gps_poll_timer_callback above: this runs on the GuiSrv thread (stack_size=2048,
       applications/services/gui/application.fam), a larger budget than Bt's/the Timer
       Service's but the same risk class as AppEvent keeps growing with new capabilities
       (104 bytes since the tagged-union rework, but it only ever grew before that).
       Deliberately its own static, not app_shared_ble_event (see that declaration's comment) --
       GuiSrv can run concurrently with BleEventWorker/Bt/Timer. Explicit reset below because
       a static initializer only runs once at load time, not per call. */
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventInput;
    event.u.input = *input;
    app_queue_put(app->queue, &event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* Returns to the main screen and discards any in-progress/completed scan results (docs/PLAN.md's
   Wi-Fi scan capability follow-on step: "results... cleared when the user leaves the results
   view"), for wifi_scan, ble_scan, and wardriving alike. Also called whenever the underlying
   session/connection goes away (disconnect, reconnect, profile teardown) -- a lost session can
   never deliver the rest of an in-flight scan's status records, so leaving stale partial
   results on screen would be misleading, regardless of which capability was in flight.
   For wardriving specifically, this is also the CSV export session boundary: closing the
   file here (not on a "stopped" ack -- see wardriving_csv_file's own declaration comment)
   and resetting wardriving_running_known to false, since this Flipper's knowledge of the
   ESP32's run state does not survive a lost session (docs/LESSONS.md "UI must derive from
   real state") -- the next authenticated session starts genuinely not knowing either way.
   mesh_log's own accumulator file shares this same session-boundary close (mesh_log_close()
   below), though it has no run-state/UI surface to reset alongside it. */
static void reset_scan_ui_state_impl(Esp32App* app, bool return_home) {
    if(return_home) {
        app->screen = AppScreenHome;
    }
    furi_timer_stop(app_gps_poll_timer);
    /* Publish is BLE-session-independent (it runs after the ESP32 is long gone, see
       docs/WARDRIVING_PUBLISH.md), so this reset path should never fire while it's in
       progress -- stopped here anyway, defensively, so a stray disconnect/reconnect can
       never leave an abandoned poll timer ticking forever against a screen nothing is
       reading from. Does not touch publish_outcome: return_home already forces the screen
       away, and the Home-menu entry point resets it fresh on next entry. */
    furi_timer_stop(app_publish_poll_timer);
    app->publish_waiting = false;
    app_pending_command_kind = PendingCommandNone;
    app->wifi_scan_in_progress = false;
    app->wifi_scan_complete = false;
    app->wifi_scan_scroll_offset = 0;
    app->wifi_scan_error_message[0] = '\0';
    wifi_scan_ap_count_reset();
    app->ble_scan_in_progress = false;
    app->ble_scan_complete = false;
    app->ble_scan_scroll_offset = 0;
    app->ble_scan_error_message[0] = '\0';
    ble_scan_device_count_reset();
    app->wardriving_running_known = false;
    app->wardriving_running = false;
    app->wardriving_backlog_remaining = 0;
    app->wardriving_last_wifi_summary[0] = '\0';
    app->wardriving_last_ble_summary[0] = '\0';
    app->wardriving_error_message[0] = '\0';
    /* UI cursor/scroll only (the five settings fields themselves are persisted, global
       config -- docs/WARDRIVING_REDESIGN.md "Persistence" -- and must NOT be reset here). */
    app->wardriving_settings_row = 0;
    app->wardriving_settings_scroll_offset = 0;
    app_wardriving_flush_led_active = false;
    if(app->notifications) {
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_set_only_blue_255);
    }
    wardriving_csv_close(app);
    mesh_log_close(app);
    /* This Flipper's knowledge of the ESP32's gps status does not survive a lost session
       either (same "UI must derive from real state" argument as wardriving_running_known
       above) -- the next authenticated session starts genuinely not knowing until the next
       poll tick's reply arrives. */
    app->gps_status_known = false;
    /* mesh_log_display_nodes/mesh_log_display_count are deliberately NOT reset here -- unlike
       gps_status_known/wardriving_running_known above, this list reflects file-backed,
       cross-session data (mesh/mesh_nodes_current.txt), not this connection's own transient
       knowledge, so a disconnect must not blank it. The Home menu's own "enter AppScreenMeshLog"
       handler reloads it fresh from disk on every entry regardless. */
}

static void reset_scan_ui_state(Esp32App* app) {
    reset_scan_ui_state_impl(app, true);
}

APP_FN void reset_scan_ui_state_keep_screen(Esp32App* app) {
    reset_scan_ui_state_impl(app, false);
}

APP_FN void stop_service(Esp32App* app) {
    furi_timer_stop(app_reassembly_timeout_timer);
    stop_ble_profile(app);
    pairing_reset_state();
    session_reset_state();
    reset_scan_ui_state_keep_screen(app);
    if(app->notifications) {
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_reset_blue);
    }
    app->pairing_phase = PairingPhaseNone;
}

/* Shared by the explicit OK-press path (no saved pairing yet) and the auto-connect path
   (docs/PLAN.md step 6: opening the FAP with any saved pairing record present starts
   advertising immediately, no OK-press needed -- that action is now reserved for the
   genuinely-new-pairing case only). Refuses to start a second profile while one is already
   waiting/active, per this app's existing UX convention -- callers must check
   `!app->profile` themselves; kept explicit at both call sites rather than hidden in here,
   matching how the rest of this file treats profile lifecycle transitions. */
APP_FN void start_profile(Esp32App* app) {
    FURI_LOG_I(TAG, "Starting custom BLE profile");
    pairing_reset_state();
    session_reset_state();
    app->profile = bt_profile_start(app->bt, &app_profile_callbacks, app);
    if(app->profile) {
        FURI_LOG_I(TAG, "Profile started: %p, BT active: %d", app->profile, furi_hal_bt_is_active());
        furi_hal_bt_stop_advertising();
        app->pairing_phase = PairingPhaseWaiting;
        notification_message(app->notifications, &sequence_blink_start_blue);
        furi_timer_start(
            app_reassembly_timeout_timer, furi_ms_to_ticks(REASSEMBLY_TIMEOUT_CHECK_PERIOD_MS));
        furi_hal_bt_start_advertising();
        FURI_LOG_I(TAG, "Advertising start requested, BT active: %d", furi_hal_bt_is_active());
    } else {
        FURI_LOG_E(TAG, "Unable to start BLE profile");
    }
}

APP_FN bool app_handle_input(Esp32App* app, const InputEvent* input) {
    if(input->type != InputTypeShort) {
        return true;
    }
    if(app->connection_lost) {
        if(input->key == InputKeyBack) {
            app->connection_lost = false;
            app->screen = AppScreenHome;
        }
    } else if(app->screen == AppScreenHome) {
        app->screen = AppScreenHome;
        if(input->key == InputKeyBack) {
            return false;
        } else if(input->key == InputKeyUp) {
            home_menu_step(app, -1);
        } else if(input->key == InputKeyDown) {
            home_menu_step(app, 1);
        } else if(
            input->key == InputKeyOk && !app->profile &&
            app->home_menu_index != HomeMenuPublish) {
            /* First-time-pairing trigger (formerly the Legacy screen's own
               `!app->profile` + OK call site, moved here since Legacy is gone) --
               the sole manual way to start the pairing ceremony on a never-paired
               board. Checked independently of, and before, the home_menu_index
               switch below: while unpaired, none of the session-gated rows are
               meaningful to select anyway -- except Publish, which is explicitly
               BLE-session-independent (docs/WARDRIVING_PUBLISH.md) and was always
               reachable pre-pairing before this pass; excluded here so selecting it
               still opens the Publish screen instead of being swallowed by the
               pairing trigger. Unrelated to and does not affect the automatic
               `if(app->has_saved_pairing) start_profile(&app)` at boot for an
               already-paired board. */
            start_profile(app);
        } else if(input->key == InputKeyOk) {
            switch(app->home_menu_index) {
            case HomeMenuWardriving:
                /* Navigating in from Home always lands on whichever screen matches
                   this Flipper's current knowledge of run state (docs/
                   WARDRIVING_REDESIGN.md) -- wardriving_running_known false (no
                   evidence yet this session) routes to Stopped, same as a
                   confirmed-stopped board; AppEventWardrivingRunState's own handler
                   switches screens if the pending status query below turns out to
                   report "running". */
                app->screen = (app->wardriving_running_known && app->wardriving_running) ?
                                 AppScreenWardrivingRunning :
                                 AppScreenWardrivingStopped;
                app->wardriving_settings_row = 0;
                app->wardriving_settings_scroll_offset = 0;
                if(app->pairing_phase == PairingPhaseSessionActive &&
                   app->capability_has_wardriving && !app->wardriving_running_known) {
                    send_wardriving_status_query(app);
                }
                /* Do not keep the GPS timer running while either Wardriving screen is
                   open. That poll stream is the source of the disconnects seen while a
                   capture is already live. */
                furi_timer_stop(app_gps_poll_timer);
                break;
            case HomeMenuScan:
                if(app->capability_has_wifi_scan && app->capability_has_ble_scan) {
                    app->scan_menu_index = ScanMenuWifi;
                    app->screen = AppScreenScan;
                } else if(app->capability_has_wifi_scan && !app->wifi_scan_in_progress) {
                    wifi_scan_ap_count_reset();
                    app->wifi_scan_scroll_offset = 0;
                    app->wifi_scan_complete = false;
                    app->wifi_scan_error_message[0] = '\0';
                    app->screen = AppScreenWifiScanResults;
                    app->wifi_scan_in_progress = send_wifi_scan_command(app);
                    if(!app->wifi_scan_in_progress) {
                        app->screen = AppScreenHome;
                    }
                } else if(app->capability_has_ble_scan && !app->ble_scan_in_progress) {
                    ble_scan_device_count_reset();
                    app->ble_scan_scroll_offset = 0;
                    app->ble_scan_complete = false;
                    app->ble_scan_error_message[0] = '\0';
                    app->screen = AppScreenBleScanResults;
                    app->ble_scan_in_progress = send_ble_scan_command(app);
                    if(!app->ble_scan_in_progress) {
                        app->screen = AppScreenHome;
                    }
                }
                break;
            case HomeMenuGps:
                app->screen = AppScreenGps;
                /* Same poll-while-open pattern as the Wardriving screen above (both
                   share app_gps_poll_timer -- they are never open at the same time, so
                   there is no contention) -- restarted rather than just started for
                   the same idempotent-reentry reason. */
                furi_timer_stop(app_gps_poll_timer);
                furi_timer_start(app_gps_poll_timer, furi_ms_to_ticks(GPS_POLL_PERIOD_MS));
                break;
            case HomeMenuMeshLog:
                app->screen = AppScreenMeshLog;
                /* No timer, unlike the GPS/Wardriving screens above -- mesh_log has no
                   command/query shape at all (cbor_mesh_log.h's own top comment), so
                   there is nothing to poll. Just reload the in-memory list from disk
                   and reset the scroll cursor fresh on every entry (docs/
                   WARDRIVING_PUBLISH.md "Mesh node publishing"); anything that streams
                   in afterward while this screen stays open is appended live by
                   handle_mesh_log_status() directly. */
                mesh_log_display_reload(app);
                app->mesh_log_scroll_offset = 0;
                break;
            case HomeMenuPublish:
                app->screen = AppScreenPublish;
                /* Fresh state on every entry, not just app start -- a previous
                   visit's outcome (or an abandoned wait) must not leak into this
                   one. */
                app->publish_outcome = PublishOutcomeNone;
                app->publish_waiting = false;
                furi_timer_stop(app_publish_poll_timer);
                break;
            default:
                break;
            }
        }
    } else if(app->screen == AppScreenScan) {
        if(input->key == InputKeyBack) {
            app->screen = AppScreenHome;
        } else if(input->key == InputKeyUp) {
            scan_menu_step(app, -1);
        } else if(input->key == InputKeyDown) {
            scan_menu_step(app, 1);
        } else if(input->key == InputKeyOk) {
            if(app->scan_menu_index == ScanMenuWifi && app->capability_has_wifi_scan && !app->wifi_scan_in_progress) {
                wifi_scan_ap_count_reset();
                app->wifi_scan_scroll_offset = 0;
                app->wifi_scan_complete = false;
                app->wifi_scan_error_message[0] = '\0';
                app->screen = AppScreenWifiScanResults;
                app->wifi_scan_in_progress = send_wifi_scan_command(app);
                if(!app->wifi_scan_in_progress) {
                    app->screen = AppScreenHome;
                }
            } else if(app->scan_menu_index == ScanMenuBle && app->capability_has_ble_scan && !app->ble_scan_in_progress) {
                ble_scan_device_count_reset();
                app->ble_scan_scroll_offset = 0;
                app->ble_scan_complete = false;
                app->ble_scan_error_message[0] = '\0';
                app->screen = AppScreenBleScanResults;
                app->ble_scan_in_progress = send_ble_scan_command(app);
                if(!app->ble_scan_in_progress) {
                    app->screen = AppScreenHome;
                }
            }
        }
    } else if(app->screen == AppScreenGps) {
        if(input->key == InputKeyBack) {
            app->screen = AppScreenHome;
            furi_timer_stop(app_gps_poll_timer);
        }
    } else if(app->screen == AppScreenMeshLog) {
        if(input->key == InputKeyBack) {
            app->screen = AppScreenHome;
        } else if(input->key == InputKeyUp) {
            if(app->mesh_log_scroll_offset > 0) {
                app->mesh_log_scroll_offset--;
            }
        } else if(input->key == InputKeyDown) {
            size_t visible_rows = MESH_LOG_RESULTS_MAX_ROWS;
            furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
            size_t count = mesh_log_display_count;
            furi_mutex_release(app_wardriving_state_mutex);
            if(count > visible_rows && app->mesh_log_scroll_offset < count - visible_rows) {
                app->mesh_log_scroll_offset++;
            }
        }
    } else if(app->screen == AppScreenWifiScanResults) {
        if(input->key == InputKeyBack) {
            reset_scan_ui_state(app);
        } else if(input->key == InputKeyUp) {
            if(app->wifi_scan_scroll_offset > 0) {
                app->wifi_scan_scroll_offset--;
            }
        } else if(input->key == InputKeyDown) {
            size_t visible_rows = WIFI_SCAN_RESULTS_MAX_ROWS;
            furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
            size_t count = wifi_scan_ap_count;
            furi_mutex_release(app_wardriving_state_mutex);
            if(count > visible_rows && app->wifi_scan_scroll_offset < count - visible_rows) {
                app->wifi_scan_scroll_offset++;
            }
        } else if(input->key == InputKeyOk && !app->wifi_scan_in_progress) {
            /* Re-trigger from inside the results view too, e.g. after a completed
               scan -- "Scan now" is a repeatable manual action, not one-shot. */
            wifi_scan_ap_count_reset();
            app->wifi_scan_scroll_offset = 0;
            app->wifi_scan_complete = false;
            app->wifi_scan_error_message[0] = '\0';
            app->wifi_scan_in_progress = send_wifi_scan_command(app);
        }
    } else if(app->screen == AppScreenBleScanResults) {
        if(input->key == InputKeyBack) {
            reset_scan_ui_state(app);
        } else if(input->key == InputKeyUp) {
            if(app->ble_scan_scroll_offset > 0) {
                app->ble_scan_scroll_offset--;
            }
        } else if(input->key == InputKeyDown) {
            size_t visible_rows = BLE_SCAN_RESULTS_MAX_ROWS;
            furi_mutex_acquire(app_wardriving_state_mutex, FuriWaitForever);
            size_t count = ble_scan_device_count;
            furi_mutex_release(app_wardriving_state_mutex);
            if(count > visible_rows && app->ble_scan_scroll_offset < count - visible_rows) {
                app->ble_scan_scroll_offset++;
            }
        } else if(input->key == InputKeyOk && !app->ble_scan_in_progress) {
            /* Re-trigger from inside the results view too, e.g. after a completed
               scan -- "Scan now" is a repeatable manual action, not one-shot. */
            ble_scan_device_count_reset();
            app->ble_scan_scroll_offset = 0;
            app->ble_scan_complete = false;
            app->ble_scan_error_message[0] = '\0';
            app->ble_scan_in_progress = send_ble_scan_command(app);
        }
    } else if(app->screen == AppScreenWardrivingRunning) {
        if(input->key == InputKeyBack) {
            /* Unlike wifi_scan/ble_scan's Back, this does NOT stop wardriving --
               capture runs autonomously server-side regardless of whether this
               screen is open (docs/CAPABILITIES.md), so leaving it is pure
               navigation, not a discard of unconfirmed state (there is none: start/
               stop are already-sent, already-acked actions by the time this screen
               reflects them). */
            app->screen = AppScreenHome;
            furi_timer_stop(app_gps_poll_timer);
        } else if(input->key == InputKeyOk) {
            send_wardriving_stop_command(app);
        }
    } else if(app->screen == AppScreenWardrivingStopped) {
        if(input->key == InputKeyBack) {
            /* Same "pure navigation, nothing to discard" reasoning as the Running
               screen's own Back above -- settings-row edits are already persisted
               to disk immediately on change (wardriving_settings_cycle_row()), not
               held as unconfirmed in-progress state. */
            app->screen = AppScreenHome;
            furi_timer_stop(app_gps_poll_timer);
        } else if(input->key == InputKeyOk) {
            send_wardriving_start_command(app);
        } else if(input->key == InputKeyUp) {
            wardriving_settings_step(app, -1);
        } else if(input->key == InputKeyDown) {
            wardriving_settings_step(app, 1);
        } else if(input->key == InputKeyLeft) {
            wardriving_settings_cycle_row(app, -1);
        } else if(input->key == InputKeyRight) {
            wardriving_settings_cycle_row(app, 1);
        }
    } else if(app->screen == AppScreenPublish) {
        if(input->key == InputKeyBack) {
            /* Discards any in-progress wait, matching this app's "Back never
               persists data" convention -- the result file (if the host script does
               eventually write one) is simply never read; nothing on the Flipper
               side is lost by cancelling. */
            publish_finish_waiting(app);
            app->screen = AppScreenHome;
        } else if(input->key == InputKeyOk && !app->publish_waiting) {
            publish_start(app);
        }
    }
    return true;
}

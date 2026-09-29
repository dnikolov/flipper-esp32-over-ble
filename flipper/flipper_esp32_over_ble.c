/* Unity build (TP-15 follow-up, 2026-09-29): this is the one real translation unit for the
   whole app -- application.fam lists only this file among the app's own sources. The other
   12 module .c files are #include-d below (any order: every cross-module symbol is declared
   in app_internal.h first, and static tentative-definition merging handles the data side),
   so the compiler sees the exact linkage/visibility the pre-split monolith had and can
   inline/elide across module boundaries the same way -Os did before the split. Each module
   .c file still reads standalone in an editor/clangd (its own #include "app_internal.h" up
   top) and refuses to be compiled on its own via the APP_UNITY_BUILD #error guard. */
#define APP_UNITY_BUILD
#include "app_internal.h"

#include "app_storage.c"
#include "wardriving_settings.c"
#include "publish.c"
#include "mesh_log_rx.c"
#include "gps_rx.c"
#include "scan_rx.c"
#include "wardriving_rx.c"
#include "wardriving_ui.c"
#include "session_flow.c"
#include "ble_transport.c"
#include "app_ui.c"

int32_t flipper_esp32_over_ble_app(void* context) {
    UNUSED(context);
    Esp32App app = {
        .queue = furi_message_queue_alloc(8, sizeof(AppEvent)),
        .screen = AppScreenHome,
        .pairing_phase = PairingPhaseNone,
        .has_saved_pairing = false,
        .connection_lost = false,
    };
    app.bt = furi_record_open(RECORD_BT);
    app.storage = furi_record_open(RECORD_STORAGE);
    app.notifications = furi_record_open(RECORD_NOTIFICATION);
    /* Defensive: also stop any blink left running by a prior crashed run of this app. */
    notification_message(app.notifications, &sequence_blink_stop);
    notification_message(app.notifications, &sequence_reset_blue);
    /* Must run before any_saved_pairing_exists()/pairing_storage_*() -- see
       resolve_pairings_dir_path()'s top comment. This app's own thread is the only safe
       place to resolve the "/data" alias; every later pairing-file path build reuses the
       cached result instead. */
    app_pairings_dir_ready = resolve_pairings_dir_path(app.storage);
    if(!app_pairings_dir_ready) {
        FURI_LOG_E(TAG, "Failed to resolve pairings directory path");
    }
    app_capabilities_dir_ready = resolve_capabilities_dir_path(app.storage);
    if(!app_capabilities_dir_ready) {
        FURI_LOG_E(TAG, "Failed to resolve capabilities directory path");
    }
    app_data_root_ready = resolve_app_data_root_path(app.storage);
    if(!app_data_root_ready) {
        FURI_LOG_E(TAG, "Failed to resolve app data root path");
    }
    app_wardriving_dir_ready = resolve_wardriving_dir_path(app.storage);
    if(!app_wardriving_dir_ready) {
        FURI_LOG_E(TAG, "Failed to resolve wardriving directory path");
    }
    app_mesh_dir_ready = resolve_mesh_dir_path(app.storage);
    if(!app_mesh_dir_ready) {
        FURI_LOG_E(TAG, "Failed to resolve mesh directory path");
    }
    wardriving_settings_load(&app);
    app.has_saved_pairing = any_saved_pairing_exists(app.storage);
    /* app_protocol_mutex must exist before this registration: bt_set_status_changed_callback()
       can fire bt_status_callback() (which acquires app_protocol_mutex to snapshot
       app_protocol_generation, HP-07/G10) at any point after this call, asynchronously on the
       "Bt" service thread, before the rest of this function's own setup has finished. */
    app_protocol_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(app_protocol_mutex);
    app_reassembly_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(app_reassembly_mutex);
    app_wardriving_state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(app_wardriving_state_mutex);
    wardriving_csv_count_refresh(&app);
    bt_set_status_changed_callback(app.bt, bt_status_callback, &app);
    app_reassembly_timeout_timer = furi_timer_alloc(
        reassembly_timeout_timer_callback, FuriTimerTypePeriodic, NULL);
    furi_check(app_reassembly_timeout_timer);
    app_gps_poll_timer = furi_timer_alloc(gps_poll_timer_callback, FuriTimerTypePeriodic, &app);
    furi_check(app_gps_poll_timer);
    app_publish_poll_timer =
        furi_timer_alloc(publish_poll_timer_callback, FuriTimerTypePeriodic, &app);
    furi_check(app_publish_poll_timer);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, &app);
    view_port_input_callback_set(view_port, input_callback, &app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    /* Baseline heap margin, logged once per launch (docs/HARDENING_BACKLOG.md H04 asked for
       exactly this number and never got it -- a launch that fails in the ELF loader never
       reaches this line, but a launch that succeeds and later dies mid-session does, so this
       is the reading that tells a future session how much room the app actually started
       with). Two numbers, not one: total free heap, and the largest contiguous block, which
       is the one that governs both the loader's own section allocations and every later
       storage_file_open() -- see storage_open_heap_margin_ok(). */
    FURI_LOG_I(
        TAG,
        "heap at launch: free %u, largest block %u",
        (unsigned)memmgr_get_free_heap(),
        (unsigned)memmgr_heap_get_max_free_block());

    /* docs/PLAN.md step 6: auto-connect when a saved pairing record already exists, no
       OK-press required -- the OK-press action below is reserved for the genuinely-new-
       pairing (no saved record at all) case. */
    if(app.has_saved_pairing) {
        start_profile(&app);
        view_port_update(view_port);
    }

    AppEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app.queue, &event, FuriWaitForever) != FuriStatusOk) continue;
        if(event.type == AppEventBtStatus) {
            if(app.profile) {
                if(event.u.bt_status.status == BtStatusConnected) {
                    app.connection_lost = false;
                    /* HP-07/G10: gated on app_protocol_generation, not unconditional -- see
                       protocol_reset_if_unchanged()'s own comment. If BleEventWorker has
                       already advanced a ceremony for this connection by the time this
                       (possibly stale/delayed) event is processed, the reset is skipped so it
                       can never tear that ceremony's in-flight state; the UI fields below
                       always update regardless (main thread stays UI-only either way). */
                    protocol_reset_if_unchanged(event.u.bt_status.generation);
                    reset_scan_ui_state_keep_screen(&app);
                    app.pairing_phase = PairingPhaseExchanging;
                    app.pairing_reason[0] = '\0';
                } else if(event.u.bt_status.status == BtStatusAdvertising) {
                    if(app.pairing_phase != PairingPhaseDone) {
                        app.connection_lost = false;
                        protocol_reset_if_unchanged(event.u.bt_status.generation);
                        reset_scan_ui_state_keep_screen(&app);
                        notification_message(app.notifications, &sequence_blink_start_blue);
                        app.pairing_phase = PairingPhaseWaiting;
                        app.pairing_reason[0] = '\0';
                    }
                } else if(event.u.bt_status.status == BtStatusUnavailable) {
                    stop_service(&app);
                    app.connection_lost = true;
                    app.pairing_phase = PairingPhaseFailed;
                    strncpy(app.pairing_reason, "connection lost", sizeof(app.pairing_reason) - 1);
                    app.pairing_reason[sizeof(app.pairing_reason) - 1] = '\0';
                }
            }
        } else if(event.type == AppEventPairingPhase) {
            app.pairing_phase = event.u.pairing.phase;
            if(event.u.pairing.phase == PairingPhaseFailed) {
                app.connection_lost = strcmp(event.u.pairing.reason, "connection lost") == 0;
                strncpy(app.pairing_reason, event.u.pairing.reason, sizeof(app.pairing_reason) - 1);
                app.pairing_reason[sizeof(app.pairing_reason) - 1] = '\0';
            } else {
                app.connection_lost = false;
                app.pairing_reason[0] = '\0';
            }
            if(event.u.pairing.phase == PairingPhaseDone) {
                app.has_saved_pairing = true;
            }
        } else if(event.type == AppEventSessionFatal) {
            bt_disconnect(app.bt);
            /* Unlike BtStatusUnavailable (stop_service(), which already calls this) and the
               reconnect paths above, this path previously left wardriving_running_known/
               wardriving_running/gps_status_known/etc. untouched -- a real hardware bug: a
               fatal decrypt/sequence-mismatch disconnect (e.g. the ESP32-side TX-fragment
               race fixed alongside this) left the Wardriving screen showing the last known
               "running" state as if it were still current after the session actually died.
               keep_screen matches this file's existing connection_lost-banner convention
               (stay on the current screen, don't snap back to Home). */
            reset_scan_ui_state_keep_screen(&app);
            /* session_reset_state() (BLE thread, before this event was posted) already
               cleared the app_wardriving_flush_led_active flag, but never touched the physical
               LED itself -- without this, a session-fatal disconnect that happens while the
               solid-green flush indicator is lit leaves the LED stuck green for the whole
               disconnected interval. Matches stop_service()'s own LED cleanup (the
               BtStatusUnavailable path just above), which this path otherwise skips since it
               doesn't call stop_service(). */
            if(app.notifications) {
                notification_message(app.notifications, &sequence_blink_stop);
                notification_message(app.notifications, &sequence_reset_blue);
            }
            app.connection_lost = true;
            app.pairing_phase = PairingPhaseFailed;
            strncpy(app.pairing_reason, "connection lost", sizeof(app.pairing_reason) - 1);
            app.pairing_reason[sizeof(app.pairing_reason) - 1] = '\0';
        } else if(event.type == AppEventCapabilityInfo) {
            app.has_capability_info = true;
            strncpy(app.capability_board, event.u.capability.board, sizeof(app.capability_board) - 1);
            app.capability_board[sizeof(app.capability_board) - 1] = '\0';
            strncpy(
                app.capability_features, event.u.capability.features, sizeof(app.capability_features) - 1);
            app.capability_features[sizeof(app.capability_features) - 1] = '\0';
            app.capability_has_wifi_scan = event.u.capability.has_wifi_scan;
            app.capability_has_ble_scan = event.u.capability.has_ble_scan;
            app.capability_has_wardriving = event.u.capability.has_wardriving;
            app.capability_has_gps = event.u.capability.has_gps;
            app.capability_has_meshcore_scan = event.u.capability.has_meshcore_scan;
            app.capability_has_mesh_log = event.u.capability.has_mesh_log;
            /* Force-jump to Wardriving on connect (docs/WARDRIVING_REDESIGN.md, decision 6):
               the moment a session's capability info arrives and the board advertises
               wardriving, the Home cursor is forced here unconditionally, even if the user
               was sitting on Publish at that moment -- in addition to, not instead of,
               home_menu_fix_selection()'s own clamp-to-first-visible-item safety net that
               already runs on every Home draw. */
            if(event.u.capability.has_wardriving) {
                app.home_menu_index = HomeMenuWardriving;
                home_menu_scroll_into_view(&app);
            }
            /* G08: capability_storage_save() moved off BleEventWorker -- a no-op if this
               particular AppEventCapabilityInfo came from capability_bootstrap()'s cache-load
               path instead of a fresh capability_response (see capability_storage_save_pending()'s
               own comment). */
            capability_storage_save_pending(&app);
        } else if(event.type == AppEventWifiScanAp) {
            /* No-op: wifi_scan_aps[]/wifi_scan_ap_count were already updated under
               app_wardriving_state_mutex by copy_wifi_scan_ap_locked() before this was posted
               (HP-08) -- this event exists only to make the main loop wake up and redraw
               (view_port_update() below runs unconditionally after every event). */
        } else if(event.type == AppEventWifiScanDone) {
            app.wifi_scan_in_progress = false;
            app.wifi_scan_complete = true;
        } else if(event.type == AppEventWifiScanError) {
            app.wifi_scan_in_progress = false;
            strncpy(
                app.wifi_scan_error_message,
                event.u.error_message,
                sizeof(app.wifi_scan_error_message) - 1);
            app.wifi_scan_error_message[sizeof(app.wifi_scan_error_message) - 1] = '\0';
        } else if(event.type == AppEventBleScanDevice) {
            /* No-op: same reasoning as AppEventWifiScanAp above -- ble_scan_devices[]/
               ble_scan_device_count were already updated under app_wardriving_state_mutex by
               copy_ble_scan_device_locked() before this was posted (HP-08). */
        } else if(event.type == AppEventBleScanDone) {
            app.ble_scan_in_progress = false;
            app.ble_scan_complete = true;
        } else if(event.type == AppEventBleScanError) {
            app.ble_scan_in_progress = false;
            strncpy(
                app.ble_scan_error_message,
                event.u.error_message,
                sizeof(app.ble_scan_error_message) - 1);
            app.ble_scan_error_message[sizeof(app.ble_scan_error_message) - 1] = '\0';
        } else if(event.type == AppEventWardrivingRunState) {
            app.wardriving_running_known = true;
            app.wardriving_running = event.u.wardriving_run_state.running;
            if(event.u.wardriving_run_state.is_fresh_start) {
                /* A genuine new "started" ack -- reset this session's own counters, distinct
                   from a `busy`-error-inferred "it was already running" correction (which
                   must NOT reset counts we may already be accumulating this connection).
                   wardriving_csv_rows is file-backed, not a session counter, so it is NOT
                   reset here -- see wardriving_csv_count_refresh(). */
                app.wardriving_backlog_remaining = 0;
                app.wardriving_last_wifi_summary[0] = '\0';
                app.wardriving_last_ble_summary[0] = '\0';
            }
            app.wardriving_error_message[0] = '\0';
            /* A running/stopped state change while already on one of the two Wardriving
               screens switches to the other (docs/WARDRIVING_REDESIGN.md) -- same re-render
               on this event the pre-redesign single screen already did, just expressed as a
               screen swap now that the two states are separate screens. */
            if(app.screen == AppScreenWardrivingStopped || app.screen == AppScreenWardrivingRunning) {
                app.screen = app.wardriving_running ? AppScreenWardrivingRunning :
                                                       AppScreenWardrivingStopped;
                app.wardriving_settings_row = 0;
                app.wardriving_settings_scroll_offset = 0;
            }
            /* G08: services handle_wardriving_status()'s "stopped" branch, which now only sets
               wardriving_csv_sync_requested instead of calling storage_file_sync() itself on
               BleEventWorker -- see wardriving_csv_drain_pending()'s own comment. A no-op (one
               mutex acquire) on every other AppEventWardrivingRunState. */
            wardriving_csv_drain_pending(&app);
        } else if(event.type == AppEventWardrivingBatch) {
            app.wardriving_csv_rows = event.u.wardriving_batch.csv_rows;
            app.wardriving_backlog_remaining = event.u.wardriving_batch.backlog_remaining;
            if(event.u.wardriving_batch.last_wifi_summary[0] != '\0') {
                strncpy(
                    app.wardriving_last_wifi_summary,
                    event.u.wardriving_batch.last_wifi_summary,
                    sizeof(app.wardriving_last_wifi_summary) - 1);
                app.wardriving_last_wifi_summary[sizeof(app.wardriving_last_wifi_summary) - 1] = '\0';
            }
            if(event.u.wardriving_batch.last_ble_summary[0] != '\0') {
                strncpy(
                    app.wardriving_last_ble_summary,
                    event.u.wardriving_batch.last_ble_summary,
                    sizeof(app.wardriving_last_ble_summary) - 1);
                app.wardriving_last_ble_summary[sizeof(app.wardriving_last_ble_summary) - 1] = '\0';
            }
            /* G08: this batch's actual dedup/format/write work (wardriving_record_stream_cb()
               only deep-copied into wardriving_csv_ring on BleEventWorker) happens here instead
               -- overwrites the row count above with the real post-drain value once it's
               known. */
            wardriving_csv_drain_pending(&app);
        } else if(event.type == AppEventWardrivingError) {
            strncpy(
                app.wardriving_error_message,
                event.u.error_message,
                sizeof(app.wardriving_error_message) - 1);
            app.wardriving_error_message[sizeof(app.wardriving_error_message) - 1] = '\0';
        } else if(event.type == AppEventGpsStatus) {
            app.gps_status_known = true;
            app.gps_state = (GpsFixState)event.u.gps.state;
            if(app.gps_state == GpsFixStateFix) {
                app.gps_lat_e7_offset = event.u.gps.lat_e7_offset;
                app.gps_lon_e7_offset = event.u.gps.lon_e7_offset;
                app.gps_fix_quality = event.u.gps.fix_quality;
                app.gps_satellites = event.u.gps.satellites;
                app.gps_hdop_e1 = event.u.gps.hdop_e1;
                app.gps_utc_timestamp_s = event.u.gps.utc_timestamp_s;
                app.gps_altitude_dm_offset = event.u.gps.altitude_dm_offset;
                app.gps_speed_e1_kmh = event.u.gps.speed_e1_kmh;
            }
        } else if(event.type == AppEventGpsPollTick) {
            /* GPS polling must not run while the Wardriving screen is open: the Wardriving
               capture is already a live BLE stream and the extra polling traffic can stall or
               disconnect the session. Only the dedicated GPS screen should keep the timer
               active. */
            if(app.screen == AppScreenGps && app.capability_has_gps) {
                send_gps_command(&app);
            }
        } else if(event.type == AppEventPublishPollTick) {
            if(app.screen == AppScreenPublish && app.publish_waiting) {
                publish_poll_check(&app);
            }
        } else if(event.type == AppEventPairingSaveRequest) {
            /* G08: pairing_storage_save() moved off BleEventWorker. */
            pairing_storage_save_pending(&app);
        } else if(event.type == AppEventMeshLogPending) {
            /* G08: mesh_log's write path moved off BleEventWorker the same way. */
            mesh_log_drain_pending(&app);
        } else if(event.type == AppEventInput) {
            if(!app_handle_input(&app, &event.u.input)) {
                running = false;
            }
        }
        view_port_update(view_port);
    }

    /* G08: pairing_storage_save()/capability_storage_save() are now async, posted through this
       same queue -- a request queued right before the user exits (Back at Home) would
       otherwise be silently discarded when the queue is freed below, without ever being
       serviced. Drain and service just those two request types here, before Storage/records
       are torn down; every other still-queued event type is discarded (its UI-side effect no
       longer matters once the app is exiting). No use-after-free risk: app.storage and every
       static path/hand-off buffer these two functions touch are still fully valid at this
       point, unchanged from mid-session use. */
    AppEvent drain_event;
    while(furi_message_queue_get(app.queue, &drain_event, 0) == FuriStatusOk) {
        if(drain_event.type == AppEventPairingSaveRequest) {
            pairing_storage_save_pending(&app);
        } else if(drain_event.type == AppEventCapabilityInfo) {
            capability_storage_save_pending(&app);
        }
    }

    bt_set_status_changed_callback(app.bt, NULL, NULL);
    if(app.profile) stop_service(&app);
    furi_timer_free(app_reassembly_timeout_timer);
    app_reassembly_timeout_timer = NULL;
    furi_timer_stop(app_gps_poll_timer);
    furi_timer_free(app_gps_poll_timer);
    app_gps_poll_timer = NULL;
    furi_timer_stop(app_publish_poll_timer);
    furi_timer_free(app_publish_poll_timer);
    app_publish_poll_timer = NULL;
    furi_mutex_free(app_reassembly_mutex);
    app_reassembly_mutex = NULL;
    furi_mutex_free(app_wardriving_state_mutex);
    app_wardriving_state_mutex = NULL;
    furi_mutex_free(app_protocol_mutex);
    app_protocol_mutex = NULL;
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_BT);
    furi_message_queue_free(app.queue);
    return 0;
}

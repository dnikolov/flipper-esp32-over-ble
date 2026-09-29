#include "feb_app_internal.h"

#if FEB_HAS_CLUSTER_WORKER
#include <stdlib.h>
#endif

/* `wardriving` capability (docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub
   reorder"; docs/PROTOCOL.md "`wardriving` command and status payloads"). Interval
   bounds/defaults (FEB_WARDRIVING_WIFI_INTERVAL_MIN_MS et al.) live in wardriving_validate.h,
   next to wardriving_resolve_start_intervals(), the pure/host-testable function that
   implements the "required vs. default when omitted" resolution -- see that header's top
   comment and feb_handle_wardriving_command()'s comment for the interpretation this codebase
   settled on (resolved 2026-09-09: an omitted interval field for a requested source
   substitutes the point-4 default, matching the v1 Flipper client's no-interval-entry-UI
   behavior). */
/* Same headroom rationale as FEB_WIFI_SCAN_STATUS_ENCODE_HEADROOM, applied to wardriving's
   status(state="data") record packing. */
#define FEB_WARDRIVING_STATUS_ENCODE_HEADROOM 32u
/* feb_wardriving_send_next_batch()'s peek-scratch buffer: one on-flash-payload-sized slot per
   possible batch entry (see wardriving_log.h's peek/scratch_buf contract). */
#define FEB_WARDRIVING_PEEK_SCRATCH_LEN (FEB_WARDRIVING_MAX_RECORDS_PER_BATCH * WD_RECORD_MAX_PAYLOAD)

/* wardriving's own start/stop state and configured cadence, independent of BLE connection
   state per docs/PROTOCOL.md ("continues across BLE disconnects, buffering results to an
   on-device flash log"). wardriving_{wifi,ble}_active track whether each source's capture
   loop is currently supposed to be running -- distinct from feb_wifi_scan_in_progress/
   feb_ble_scan_in_progress, which track whether the underlying radio resource is claimed *right
   now* (true for the source's entire enabled lifetime, not just mid-pass, since a manual
   scan must stay locked out for as long as wardriving holds that source, not just while a
   scan/window is actually in flight). */
bool feb_wardriving_wifi_active;
bool feb_wardriving_ble_active;
static bool wardriving_ble_passive;
uint32_t feb_wardriving_wifi_interval_ms;
uint32_t feb_wardriving_ble_window_ms;
uint32_t feb_wardriving_ble_interval_ms;

/* Backlog-flush gate state: a rolling window over the last FEB_WARDRIVING_FLUSH_WINDOW_SCANS
   passes' new-appended-record counts (see wardriving_validate.h), plus the bookkeeping to
   derive each pass's delta from wardriving_dedup_appended_total()'s monotonic counter and to
   sub-sample BLE-only runs down to one push per FEB_WARDRIVING_FLUSH_BLE_WINDOWS_PER_SAMPLE
   window closes. Shared through feb_app_internal.h (feb_cap_scan.c's feb_wifi_scan_done_cb()/
   feb_ble_scan_window_close_cb() also use it) rather than kept next to that function. */
wardriving_flush_window_t feb_wardriving_flush_window;
uint16_t feb_wardriving_flush_last_appended;
uint8_t feb_wardriving_flush_ble_windows;

#if FEB_HAS_CLUSTER_WORKER
/* Phase 9 cluster delegation: true when wardriving's Wi-Fi source is streaming from a
   present 2.4GHz cluster worker instead of this board's own local radio. Decided once, at
   feb_wardriving_start_internal() time (feb_cluster_worker_is_present() is a point-in-time
   check, same as the manual wifi_scan path already does), not re-checked for the rest of the
   run -- a worker that drops mid-run is not detected or fallen back from; see
   feb_wardriving_start_internal()'s own comment. */
bool feb_wardriving_wifi_delegated;
#endif

static wardriving_swelling_mode_t wardriving_wifi_swelling;
/* Only meaningful while wardriving_wifi_swelling == WARDRIVING_SWELLING_SPEED_BASED --
   tracks whether the aggressive 85ms dwell is currently selected, updated from a fresh
   location_get_fix() read on every feb_wardriving_wifi_interval_cb() re-arm (2 km/h hysteresis
   band, see that function). Reset to false (normal) at every feb_wardriving_start_internal(). */
static bool wardriving_swelling_aggressive_active;
struct ble_npl_callout feb_wardriving_wifi_interval_co;
struct ble_npl_callout feb_wardriving_ble_interval_co;
/* Last-saved on/off + Wi-Fi/BLE settings (wardriving_persist.h), loaded once at boot and
   kept in sync with every start/stop transition -- source of truth for both the boot
   autostart and the boot-button toggle's "last used settings" behavior. */
feb_wardriving_persisted_state_t feb_wardriving_persisted;
/* True while a wardriving status(state="data") batch's fragments are still going out (or
   another is chained behind it via TX_DONE_CONTINUE_WARDRIVING) -- gates
   feb_wardriving_maybe_kick_send() so a live capture event during an in-flight batch doesn't
   start a second, overlapping send (fragmentation is single-in-flight per
   docs/PROTOCOL.md#fragmentation); the in-flight chain itself re-checks the log for new
   data every time it finishes a batch, so nothing is lost by waiting.

   Root cause of the 2026-09-10 GATT-write-flood hardware bug (docs/LESSONS.md): this flag
   used to be cleared as soon as feb_queue_and_send_protected() returned true, i.e. as soon as
   the record's *first* fragment was handed to NimBLE -- not once feb_write_complete() confirmed
   every fragment of a multi-fragment record actually went out. wardriving's own ~30ms
   capture-completion cadence (unlike wifi_scan/ble_scan, which only ever get re-kicked by a
   slow, user-initiated `command`) could then fire again before the prior record's tail
   fragments finished sending, clobbering the single shared tx_fragment_* state with a second
   concurrent send and flooding NimBLE's write-buffer pool. Fixed by always chaining through
   TX_DONE_CONTINUE_WARDRIVING (see feb_wardriving_send_next_batch()) so this flag (and
   feb_wardriving_pending_drain_count below) are only ever touched once feb_write_complete() has
   confirmed full delivery of the record currently in flight. */
bool feb_wardriving_tx_in_flight;
/* Count of records included in the batch currently in flight, marked drained from the flash
   log only on the *next* feb_wardriving_send_next_batch() re-entry (which only happens after
   feb_write_complete() confirms every fragment of that batch was actually delivered) -- not at
   send time. Marking a record drained before delivery is confirmed would silently lose it
   forever on a mid-record write failure (the flash log's only copy would already be gone).
   Reset alongside feb_wardriving_tx_in_flight on every new connection (BLE_GAP_EVENT_CONNECT):
   an abandoned in-flight count from a prior, now-dead connection must not later mark-drain
   records that were never actually delivered. */
size_t feb_wardriving_pending_drain_count;
static void wardriving_flush_window_start(void);
static void wardriving_apply_wifi_swelling(wifi_scan_config_t *scan_cfg);

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder" / docs/PROTOCOL.md
   "`wardriving` command and status payloads". Re-arms the next Wi-Fi capture pass after
   feb_wardriving_wifi_interval_ms (0 = immediate/continuous) -- runs on the NimBLE host task
   (feb_wardriving_wifi_interval_co's queue), consistent with feb_handle_wardriving_command() and
   feb_handle_wifi_scan_command() both already calling esp_wifi_scan_start() from that same
   task.

   docs/WARDRIVING_REDESIGN.md (2026-09-21): when wardriving_wifi_swelling is
   WARDRIVING_SWELLING_SPEED_BASED, this is also where the next scan's dwell mode is
   decided -- a fresh location_get_fix() read (never cached, per location.h) compared
   against a 2 km/h hysteresis band (>=10.0 km/h switches to aggressive, <8.0 km/h switches
   back to normal; a no-fix/unknown state leaves wardriving_swelling_aggressive_active
   unchanged). wardriving_apply_wifi_swelling() then reads that flag (or the fixed
   normal/aggressive mode) to set scan_cfg's dwell before this scan starts.

   Phase 9 addition (FEB_HAS_CLUSTER_WORKER, 2026-09-26): when feb_wardriving_wifi_delegated,
   there is no local scan to re-trigger -- the worker streams continuously on its own cadence
   (its own SCAN_CONFIG_SET.interval_ms, set once at feb_wardriving_start_internal()) -- so
   this callout's job becomes periodically flushing whatever scan_result frames the cluster
   RX task has appended since the last flush, by re-triggering feb_wifi_scan_done_co (the same
   handoff feb_wifi_scan_done_handler() uses for the local-radio source). The no-fix skip
   applies to the non-delegated path only. */
void feb_wardriving_wifi_interval_cb(struct ble_npl_event *ev)
{
    wifi_scan_config_t scan_cfg;
    esp_err_t err;
    feb_location_t fix;
    feb_location_state_t loc_state;

    (void)ev;
    if (!feb_wardriving_wifi_active) {
        return;
    }
#if FEB_HAS_CLUSTER_WORKER
    if (feb_wardriving_wifi_delegated) {
        ble_npl_callout_reset(&feb_wifi_scan_done_co, 0);
        return;
    }
#endif
    loc_state = location_get_fix(&fix);
    if (loc_state != FEB_LOCATION_FIX) {
        /* No fix this cycle -- skip starting a new scan, but still re-arm so scanning
           resumes once a fix comes back. Clamped to FEB_WARDRIVING_NO_FIX_RETRY_FLOOR_MS
           rather than the raw configured interval: that interval can legitimately be 0
           ("aggressive"/continuous), and re-arming at 0ms here produced a zero-delay refire
           loop that starved CPU0's IDLE task and tripped the task watchdog (HP-02,
           hardware-reproduced on the C5 2026-09-27). */
        uint32_t retry_ms = (feb_wardriving_wifi_interval_ms < FEB_WARDRIVING_NO_FIX_RETRY_FLOOR_MS) ?
                             FEB_WARDRIVING_NO_FIX_RETRY_FLOOR_MS : feb_wardriving_wifi_interval_ms;

        ESP_LOGW(TAG, "wardriving: skipping wifi scan start, no GPS fix yet");
        feb_wardriving_maybe_kick_send(feb_connection_handle);
        ble_npl_callout_reset(&feb_wardriving_wifi_interval_co,
                              ble_npl_time_ms_to_ticks32(retry_ms));
        return;
    }
    if (wardriving_wifi_swelling == WARDRIVING_SWELLING_SPEED_BASED) {
        if (fix.speed_e1_kmh >= 100u) {
            wardriving_swelling_aggressive_active = true;
        } else if (fix.speed_e1_kmh < 80u) {
            wardriving_swelling_aggressive_active = false;
        }
    }
    memset(&scan_cfg, 0, sizeof(scan_cfg));
    wardriving_apply_wifi_swelling(&scan_cfg);
    if (feb_app_hooks->wifi_scan_cfg_ext != NULL) {
        feb_app_hooks->wifi_scan_cfg_ext(&scan_cfg);
    }
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wardriving: re-trigger esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        feb_wardriving_self_stop("internal_error");
    }
}

/* Same re-arming role as feb_wardriving_wifi_interval_cb() above, for the BLE source: opens the
   next discovery window after the configured gap (ble_interval_ms - ble_window_ms, or 0 for
   back-to-back per docs/PROTOCOL.md's window<=interval invariant).

   Unlike the Wi-Fi side, this discovery window is not skipped when there's no GPS fix (HP-03):
   it is the same NimBLE discovery procedure feb_gap_event()'s BLE_GAP_EVENT_DISC handler uses to
   find the Flipper's advertisement for reconnect (feb_start_scan() piggybacks on whichever
   discovery is already running rather than starting a second one), so skipping it here stalls
   reconnect for as long as there's no fix -- reproduced live on the C5 2026-09-27 (autostarted
   wardriving, no GPS fix, Flipper never found/connected for the rest of the session).
   feb_ble_scan_window_close_cb() still discards results with no fix to attach them to at window
   close; only the scan itself is unconditional. */
void feb_wardriving_ble_interval_cb(struct ble_npl_event *ev)
{
    struct ble_gap_disc_params params = {0};
    int rc;

    (void)ev;
    if (!feb_wardriving_ble_active) {
        return;
    }
    feb_ble_scan_raw_count = 0;
     /* Passive capture is safe while the authenticated connection is present. During reconnect,
         use active discovery so the Flipper's service can still be found (a passive-only re-arm
         was shown to miss the reconnect match in live testing). */
     params.passive = wardriving_ble_passive &&
                            feb_runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED;
    params.filter_duplicates = 0;
    params.itvl = 0;
    params.window = 0;
    rc = ble_gap_disc(feb_own_addr_type, BLE_HS_FOREVER, &params, feb_gap_event, NULL);
    if (rc == BLE_HS_EBUSY) {
        /* GAP master state is owned by someone else right now -- not feb_start_scan() (guarded
           against feb_wardriving_ble_active, docs/LESSONS.md 2026-09-10), but this can still
           happen transiently when feb_gap_event()'s own merged-reconnect path
           (BLE_GAP_EVENT_DISC's "found v2 peer, connecting" branch) has just cancelled
           discovery to issue ble_gap_connect(). That resolves itself once the connect
           attempt succeeds or fails, so retry opening this window after the same short
           window duration rather than tearing wardriving down over a transient condition. */
        ble_npl_callout_reset(&feb_wardriving_ble_interval_co,
                              ble_npl_time_ms_to_ticks32(feb_wardriving_ble_window_ms));
        return;
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "wardriving: re-trigger ble_gap_disc failed: %d", rc);
        feb_wardriving_self_stop("internal_error");
        return;
    }
    ble_npl_callout_reset(&feb_ble_scan_done_co, ble_npl_time_ms_to_ticks32(feb_wardriving_ble_window_ms));
}

static void wardriving_sync_status_led(void)
{
    feb_status_led_set_wardriving_active(feb_wardriving_wifi_active || feb_wardriving_ble_active);
}

/* Shared teardown for every wardriving stop path (explicit `stop` command, self-stop on
   error, boot-button toggle-off) -- stops whichever source(s) are active and syncs the
   status LED. Does not touch persisted state or send any BLE notification; callers own
   that. Runs on the NimBLE host task only (see feb_wardriving_self_stop()'s comment below for
   why) -- with one board exception: the Heltec's radio kill switch calls this from its touch
   task (HARDENING_PLAN.md HP-04, heltec/main/killswitch_glue.c), which is why the
   cluster-delegated branch's flush-buffer release goes through the cluster glue's spinlock
   (feb_cluster_wardriving_end()). With FEB_HAS_CLUSTER_WORKER, a delegated Wi-Fi source tells
   the worker to go idle instead of stopping a local scan that was never running. */
void feb_wardriving_stop_internal(void)
{
    if (feb_wardriving_wifi_active) {
        feb_wardriving_wifi_active = false;
        feb_wifi_scan_in_progress = false;
        ble_npl_callout_stop(&feb_wardriving_wifi_interval_co);
#if FEB_HAS_CLUSTER_WORKER
        if (feb_wardriving_wifi_delegated) {
            feb_wardriving_wifi_delegated = false;
            feb_cluster_wardriving_end();
        } else
#endif
        {
            esp_err_t serr = esp_wifi_scan_stop();

            if (serr != ESP_OK && serr != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_scan_stop failed while stopping wardriving: %s",
                         esp_err_to_name(serr));
            }
        }
    }
    if (feb_wardriving_ble_active) {
        feb_wardriving_ble_active = false;
        feb_ble_scan_in_progress = false;
        ble_npl_callout_stop(&feb_wardriving_ble_interval_co);
        {
            int derr = ble_gap_disc_cancel();

            if (derr != 0 && derr != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "ble_gap_disc_cancel failed while stopping wardriving: %d", derr);
            }
        }
    }
    wardriving_sync_status_led();
}

/* Sets scan_cfg->scan_time.active.min/max per the currently-stored WiFi swelling mode
   (docs/WARDRIVING_REDESIGN.md, added 2026-09-21). "normal": scan_cfg is left as the caller
   zeroed it (today's default). "aggressive": fixed 85ms active dwell per channel.
   "speed_based": mirrors "aggressive" while wardriving_swelling_aggressive_active is true,
   "normal" otherwise -- the caller (feb_wardriving_wifi_interval_cb()) is responsible for
   refreshing that flag from a fresh GPS read before calling this; feb_wardriving_start_internal()
   always calls this right after resetting the flag to false (speed_based starts at normal
   dwell per the design doc). */
static void wardriving_apply_wifi_swelling(wifi_scan_config_t *scan_cfg)
{
    bool aggressive = (wardriving_wifi_swelling == WARDRIVING_SWELLING_AGGRESSIVE) ||
                      (wardriving_wifi_swelling == WARDRIVING_SWELLING_SPEED_BASED &&
                       wardriving_swelling_aggressive_active);

    if (aggressive) {
        scan_cfg->scan_time.active.min = 85;
        scan_cfg->scan_time.active.max = 85;
    }
}

/* Shared start path for every wardriving start trigger (explicit `start` command, boot
   autostart, boot-button toggle-on). Assumes the caller already validated/resolved
   whatever's specific to its own trigger (wire-format checks for a command, none needed
   for the other two); re-checks the busy/already-running guards itself regardless, since
   autostart and the button don't go through feb_handle_wardriving_command()'s pre-checks.
   Rolls back atomically on a partial failure, same as the original inline logic this was
   extracted from. Runs on the NimBLE host task only.

   want_ble_passive implies the BLE source is wanted even when want_ble is false (BL30):
   sources=["ble_passive"] alone must still open BLE discovery, just with the passive flag
   set once authenticated (see wardriving_ble_passive/params.passive below) -- every check in
   this function that gates on "is BLE requested" reads (want_ble || want_ble_passive), not
   want_ble alone.

   FEB_HAS_CLUSTER_WORKER (Phase 9, 2026-09-26): want_wifi delegates to a present 2.4GHz
   cluster worker instead of scanning locally, per docs/CLUSTER.md's "Composite behaviors"
   wardriving bullet. Presence (feb_cluster_worker_is_present()) is checked exactly once, here,
   at start -- not re-checked for the rest of the run. Chosen over continuous re-checking
   because (a) it matches this function's own existing convention of reading every other piece
   of config (country, swelling, interval) once at start rather than re-sampling it mid-run,
   and (b) a worker that drops mid-run would need a live switch back to a cold local
   esp_wifi_scan_start() plus discarding whatever the worker had already streamed -- real
   complexity for an event this design has no detection path for anyway (WORKER_HELLO absence
   is only ever consulted at a decision point, never polled continuously). A worker that is
   present at start but disappears later will simply stop producing scan_result frames;
   feb_wardriving_wifi_interval_co keeps flushing an empty batch every interval until
   wardriving is stopped -- degraded (no results), not broken. */
bool feb_wardriving_start_internal(bool want_wifi, bool want_ble, bool want_ble_passive,
                                      uint32_t wifi_interval_ms, uint32_t ble_window_ms,
                                      uint32_t ble_interval_ms,
                                      wardriving_swelling_mode_t wifi_swelling,
                                      wardriving_country_t country
#if FEB_WIFI_DUAL_BAND
                                      , uint8_t wifi_band
#endif
                                      )
{
    if (feb_wardriving_wifi_active || feb_wardriving_ble_active) {
        return false;
    }
    if ((want_wifi && feb_wifi_scan_in_progress) ||
        ((want_ble || want_ble_passive) && feb_ble_scan_in_progress)) {
        return false;
    }

    if (want_wifi) {
#if !FEB_HAS_CLUSTER_WORKER
        wifi_scan_config_t scan_cfg;
        esp_err_t err;
        esp_err_t country_err;

        memset(&scan_cfg, 0, sizeof(scan_cfg));
#endif
        feb_wifi_scan_in_progress = true;
        feb_wifi_scan_active_source = WIFI_SCAN_SOURCE_WARDRIVING;
        feb_wardriving_wifi_interval_ms = wifi_interval_ms;
        wardriving_wifi_swelling = wifi_swelling;
        wardriving_swelling_aggressive_active = false;
#if FEB_HAS_CLUSTER_WORKER

        wifi_ap_record_t *flush_buf = feb_cluster_worker_is_present() ?
                                      malloc(FEB_WIFI_SCAN_RAW_MAX * sizeof(wifi_ap_record_t)) :
                                      NULL;

        if (flush_buf != NULL) {
            feb_wardriving_wifi_delegated = true;
            feb_wifi_scan_raw_count = 0;
            /* wifi_swelling's numeric value is a direct 1:1 cast to feb_cluster_dwell_mode_t
               (wardriving_swelling_mode_t's comment at its own definition). No country field
               exists on this wire message (docs/CLUSTER.md) -- regulatory country is a
               local-radio-only concept here, not forwarded to the worker. */
            feb_cluster_wardriving_begin(flush_buf, (uint8_t)wifi_swelling, (uint16_t)wifi_interval_ms);
            feb_wardriving_wifi_active = true;
            wardriving_flush_window_start();
            wardriving_sync_status_led();
            ble_npl_callout_reset(&feb_wardriving_wifi_interval_co,
                                  ble_npl_time_ms_to_ticks32(wifi_interval_ms));
            ESP_LOGI(TAG, "wardriving: 2.4GHz cluster worker present, delegating wifi capture");
        } else {
            /* Also reached if a worker is present but the flush-buffer allocation above
               failed -- falls back to the local-radio path below exactly as if no worker
               were present at all, rather than aborting wardriving start over a transient
               allocation failure. */
            wifi_scan_config_t scan_cfg;
            esp_err_t err;
            esp_err_t country_err;

            feb_wardriving_wifi_delegated = false;
            memset(&scan_cfg, 0, sizeof(scan_cfg));
#endif

        /* Applied once, here, at start -- not re-applied on every WiFi re-arm (a
           radio-global setting per docs/WARDRIVING_REDESIGN.md), so any later manual
           wifi_scan observes whatever country wardriving last set. Best-effort: a failure
           here doesn't abort wardriving start, since the radio already has a usable
           (world-safe) default country from ESP-IDF's own init. */
        country_err = esp_wifi_set_country_code(
            country == WARDRIVING_COUNTRY_BG ? "BG" : "01", false);
        if (country_err != ESP_OK) {
            ESP_LOGW(TAG, "wardriving: esp_wifi_set_country_code failed: %s",
                     esp_err_to_name(country_err));
        }
#if FEB_WIFI_DUAL_BAND
        /* Board-owned radio-level band mode (e.g. esp_wifi_set_band_mode()), applied once at
           start like the country code right above -- see the C5 board_config.h's own
           wardriving_start_ext comment. Not wired on single-band boards. */
        if (feb_app_hooks->wardriving_start_ext != NULL) {
            feb_app_hooks->wardriving_start_ext(wifi_band);
        }
#endif

        wardriving_apply_wifi_swelling(&scan_cfg);
        if (feb_app_hooks->wifi_scan_cfg_ext != NULL) {
            feb_app_hooks->wifi_scan_cfg_ext(&scan_cfg);
        }
        err = esp_wifi_scan_start(&scan_cfg, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wardriving: esp_wifi_scan_start failed: %s", esp_err_to_name(err));
            feb_wifi_scan_in_progress = false;
            return false;
        }
        feb_wardriving_wifi_active = true;
        wardriving_flush_window_start();
        wardriving_sync_status_led();
#if FEB_HAS_CLUSTER_WORKER
        }
#endif
    }

    if (want_ble || want_ble_passive) {
        struct ble_gap_disc_params params = {0};
        int rc;

        feb_ble_scan_in_progress = true;
        feb_ble_scan_active_source = BLE_SCAN_SOURCE_WARDRIVING;
        feb_ble_scan_raw_count = 0;
        feb_wardriving_ble_window_ms = ble_window_ms;
        feb_wardriving_ble_interval_ms = ble_interval_ms;
        wardriving_ble_passive = want_ble_passive;

        /* Active, not passive -- see feb_wardriving_ble_interval_cb()'s comment: this window is
           also the merged reconnect-scan pass while wardriving's BLE source owns discovery,
           and a passive-only pass here was found to never catch the reconnect match. */
        params.passive = wardriving_ble_passive &&
                 feb_runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED;
        params.filter_duplicates = 0;
        params.itvl = 0;
        params.window = 0;
        rc = ble_gap_disc(feb_own_addr_type, BLE_HS_FOREVER, &params, feb_gap_event, NULL);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "wardriving: ble_gap_disc start failed: %d", rc);
            feb_ble_scan_in_progress = false;
            if (want_wifi) {
                /* Roll back the Wi-Fi source already started above, so start is atomic --
                   either every requested source comes up, or none does. */
                feb_wardriving_wifi_active = false;
                feb_wifi_scan_in_progress = false;
#if FEB_HAS_CLUSTER_WORKER
                ble_npl_callout_stop(&feb_wardriving_wifi_interval_co);
                if (feb_wardriving_wifi_delegated) {
                    feb_wardriving_wifi_delegated = false;
                    feb_cluster_wardriving_end();
                } else {
                    esp_wifi_scan_stop();
                }
#else
                esp_wifi_scan_stop();
#endif
                wardriving_sync_status_led();
            }
            return false;
        }
        ble_npl_callout_reset(&feb_ble_scan_done_co, ble_npl_time_ms_to_ticks32(feb_wardriving_ble_window_ms));
        feb_wardriving_ble_active = true;
        wardriving_flush_window_start();
        wardriving_sync_status_led();
    }

    return true;
}

/* Stops whichever wardriving source(s) are active and, if connected+authenticated, sends
   the docs/PROTOCOL.md-specified unsolicited error + status(state="stopped") pair (request_id
   0, the same "ESP32-initiated, not a reply to a specific command" sentinel used for
   backlog-drain status records) -- covers both an explicit internal self-stop trigger (a
   persistent flash-write failure; see FEB_WARDRIVING_FLASH_FAILURE_LIMIT) and a scan/discovery
   restart failure partway through a run. A no-op if wardriving wasn't running. Runs on the
   NimBLE host task only (called from feb_wifi_scan_done_cb()/feb_ble_scan_window_close_cb() and the
   interval callbacks above, all on that task) -- never from feb_wifi_scan_done_handler() or
   feb_ble_scan_catalog_advertisement() directly, since this touches feb_connection_handle/
   tx_fragment_* state owned by the NimBLE host task (see wardriving_{wifi,ble}_self_stop_pending's
   comment for the hand-off). */
void feb_wardriving_self_stop(const char *error_code)
{
    bool was_active = feb_wardriving_wifi_active || feb_wardriving_ble_active;

    feb_wardriving_stop_internal();
    if (!was_active) {
        return;
    }
    /* Deliberately does NOT clear/persist feb_wardriving_persisted.enabled: this is a
       transient runtime fault (flash-write failure, scan re-trigger failure), not the
       user turning wardriving off, so the saved intent survives and boot autostart will
       retry with the same settings next boot. Only the explicit `stop` command and the
       boot-button toggle-off (both real user intent) clear it. */
    ESP_LOGE(TAG, "wardriving self-stopped (%s)", error_code);

    if (feb_connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        feb_runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        return;
    }
    {
        feb_error_payload_t err = {0};
        size_t payload_len;

        err.code = error_code;
        err.code_len = strlen(error_code);
        err.has_request_id = 0;
        payload_len = feb_cbor_encode_error_payload(feb_pairing_payload_encode_buf,
                                                    sizeof(feb_pairing_payload_encode_buf), &err);
        if (payload_len == 0 ||
            !feb_queue_and_send_protected(feb_connection_handle, "error", strlen("error"),
                                      feb_pairing_payload_encode_buf, payload_len,
                                      TX_DONE_SEND_WARDRIVING_STOPPED)) {
            ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

/* Called once per start command (both source starts in the same command may each call this;
   harmless) so a fresh run doesn't inherit the previous run's window state. State declared
   near feb_wardriving_wifi_active/feb_wardriving_ble_active above (also used by
   feb_cap_scan.c's feb_wifi_scan_done_cb()/feb_ble_scan_window_close_cb()). */
static void wardriving_flush_window_start(void)
{
    wardriving_flush_window_reset(&feb_wardriving_flush_window);
    feb_wardriving_flush_last_appended = wardriving_dedup_appended_total();
    feb_wardriving_flush_ble_windows = 0;
}
/* wardriving_send_backlog_count_update()'s dedup tracker, reset to UINT64_MAX (an impossible
   pending count, forcing a fresh report) on every new connection -- the Flipper resets its own
   displayed count to 0 on every disconnect/reconnect (flipper_esp32_over_ble.c's
   reset_scan_ui_state_impl()), so this must not persist a stale "already reported this count"
   memory across a session boundary, or a reconnect where the pending count happens to match
   what was last reported before the disconnect would leave the Flipper stuck showing 0
   indefinitely. */
uint64_t feb_wardriving_last_reported_backlog = UINT64_MAX;

/* Shared scratch for both wardriving_send_backlog_count_update() and feb_wardriving_send_next_batch()
   below -- static, not stack-local, for the same NimBLE-host-task stack-budget reason those
   functions' own comments explain (2026-09-10 stack-overflow bug). A second full-size
   feb_wardriving_status_result_payload_t (~3.6 KB, dominated by its 32-entry records array)
   would not fit in DRAM on every board (hardware-confirmed: overflowed the Heltec build by
   3392 bytes 2026-09-27) even though the count-update path only ever touches two scalar
   fields. Sharing is safe because the two functions are mutually exclusive in time:
   wardriving_send_backlog_count_update() only runs while !feb_wardriving_tx_in_flight, and
   feb_wardriving_send_next_batch() is the only thing that sets feb_wardriving_tx_in_flight true. */
static feb_wardriving_status_result_payload_t wardriving_result_scratch;
static feb_status_payload_t wardriving_status_payload_scratch;
static uint8_t wardriving_result_buf_scratch[FEB_CBOR_MAX_PAYLOAD];

/* Sends a lightweight status(state="data") update with an empty records array -- just enough
   to move the Flipper's displayed backlog count, without actually draining anything. Used by
   feb_wardriving_maybe_kick_send() below while the real flush is gated off, so pausing the flush
   doesn't also freeze the on-screen number. record_count=0 + backlog_remaining=pending encodes
   and decodes cleanly (an empty records array is not a special case on either side of the
   wire). Returns whether the update actually reached the wire (HP-22); the caller must not
   advance feb_wardriving_last_reported_backlog on a false return, or a dropped update (encode
   failure, or the fragment queue being full) leaves the Flipper's displayed count stale with
   no way to notice, since the dedup check would then treat that pending value as "already
   reported". */
static bool wardriving_send_backlog_count_update(uint16_t conn_handle, size_t pending)
{
    size_t result_len;
    size_t payload_len;

    memset(&wardriving_result_scratch, 0, sizeof(wardriving_result_scratch));
    wardriving_result_scratch.backlog_remaining = pending;
    result_len = feb_cbor_encode_wardriving_status_result_payload(
        wardriving_result_buf_scratch, sizeof(wardriving_result_buf_scratch), &wardriving_result_scratch);
    if (result_len == 0) {
        return false;
    }

    memset(&wardriving_status_payload_scratch, 0, sizeof(wardriving_status_payload_scratch));
    wardriving_status_payload_scratch.request_id = 0;
    wardriving_status_payload_scratch.state = "data";
    wardriving_status_payload_scratch.state_len = strlen("data");
    wardriving_status_payload_scratch.result_span = wardriving_result_buf_scratch;
    wardriving_status_payload_scratch.result_span_len = result_len;
    wardriving_status_payload_scratch.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf),
                                                 &wardriving_status_payload_scratch);
    if (payload_len == 0) {
        return false;
    }
    return feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len);
}

/* Kicks off a wardriving status(state="data") send if a session is connected+authenticated,
   there is buffered data to send, and no batch is already in flight -- called both right
   after a live capture pass appends new records and once at session-establish time to start
   draining any pre-existing backlog (docs/PROTOCOL.md's "Unsolicited backlog drain"). Once
   started, feb_wardriving_send_next_batch()'s own TX_DONE_CONTINUE_WARDRIVING chaining picks up
   anything appended later without needing another call here -- that chain re-enters
   feb_wardriving_send_next_batch() directly, never this function, so the gate below never
   interrupts a drain already in flight.

   Gate: avoids the BLE batch-send of raw record *data* competing with active WiFi scanning on
   the shared 2.4GHz radio while driving -- open whenever there's no GPS fix, the backlog has
   grown past FEB_WARDRIVING_FLUSH_BACKLOG_THRESHOLD, wardriving isn't actually scanning on
   either source, or feb_wardriving_flush_window (fed once per pass, see wardriving_flush_window_t)
   says the last FEB_WARDRIVING_FLUSH_WINDOW_SCANS passes were quiet. wardriving_send_backlog_
   count_update() above keeps the Flipper's displayed backlog count current regardless of this
   gate, so pausing the data flush doesn't also freeze the on-screen number. */
void feb_wardriving_maybe_kick_send(uint16_t conn_handle)
{
    feb_location_t fix;
    feb_location_state_t loc_state;
    bool gate_open;
    size_t pending;

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || feb_runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        return;
    }

    pending = wardriving_log_pending_count();
    loc_state = location_get_fix(&fix);
    gate_open = (loc_state != FEB_LOCATION_FIX) ||
                (pending > FEB_WARDRIVING_FLUSH_BACKLOG_THRESHOLD) ||
                (!feb_wardriving_wifi_active && !feb_wardriving_ble_active) ||
                feb_wardriving_flush_window.open;

    if (!gate_open || feb_wardriving_tx_in_flight || pending == 0) {
        if (!feb_wardriving_tx_in_flight && (uint64_t)pending != feb_wardriving_last_reported_backlog) {
            if (wardriving_send_backlog_count_update(conn_handle, pending)) {
                feb_wardriving_last_reported_backlog = pending;
            }
        }
        return;
    }

    feb_wardriving_tx_in_flight = true;
    feb_status_led_set(FEB_STATUS_LED_FLUSHING);
    feb_wardriving_send_next_batch(conn_handle);
}

/* Builds and sends one wardriving status(state="data") record from the oldest still-pending
   flash-log records, mirroring feb_wifi_scan_send_next_batch()/feb_ble_scan_send_next_batch()'s
   trial-encode-and-back-off packing exactly (see that function's comment for why
   peeked/peek_scratch/trial are static, not stack-local -- same NimBLE host task, same
   reasoning). Shares wardriving_result_scratch/wardriving_status_payload_scratch/
   wardriving_result_buf_scratch with wardriving_send_backlog_count_update() above -- see that
   pair's declaration comment. Unlike wifi_scan/ble_scan's own chaining, this always chains
   through TX_DONE_CONTINUE_WARDRIVING -- even for the batch that drains the last pending
   record -- rather than only while backlog_remaining > 0: the re-entry this produces is what
   confirms full delivery of the batch before marking it drained from the flash log and
   clearing feb_wardriving_tx_in_flight (see feb_wardriving_pending_drain_count's comment; fixes the
   2026-09-10 GATT-write-flood bug in docs/LESSONS.md). */
void feb_wardriving_send_next_batch(uint16_t conn_handle)
{
    static feb_wardriving_record_t peeked[FEB_WARDRIVING_MAX_RECORDS_PER_BATCH];
    static uint8_t peek_scratch[FEB_WARDRIVING_PEEK_SCRATCH_LEN];
    /* Same reasoning as wardriving_result_scratch above (and feb_ble_scan_send_next_batch()'s own
       `trial`) -- this holds a full feb_wardriving_status_result_payload_t (a 32-entry
       feb_wardriving_record_t array, ~2.6 KB) per trial-encode iteration. Originally declared
       as a loop-local, which measured at -fstack-usage's 2608 bytes for this whole function --
       nearly 2/3 of the nimble_host task's 4096-byte budget in one frame, on top of the
       CBOR-encode/GATT-write call chain this function itself makes. Root cause of the
       2026-09-10 hardware stack-overflow crash (docs/LESSONS.md); moved to static to match the
       sibling function's already-correct pattern. */
    static feb_wardriving_status_result_payload_t trial;
    size_t peeked_count;
    size_t include_count;
    size_t remaining_after;
    size_t result_len;
    size_t payload_len;
    size_t pending_now;

    /* The previous batch's records are only safe to evict from the flash log once
       feb_write_complete() has confirmed every one of its fragments actually went out --
       this function is only ever re-entered (via TX_DONE_CONTINUE_WARDRIVING) after that
       point, never earlier. See feb_wardriving_pending_drain_count's comment. */
    if (feb_wardriving_pending_drain_count > 0) {
        wardriving_log_mark_drained(feb_wardriving_pending_drain_count);
        feb_wardriving_pending_drain_count = 0;
    }

    peeked_count = wardriving_log_peek_pending(peeked, FEB_WARDRIVING_MAX_RECORDS_PER_BATCH,
                                               peek_scratch, sizeof(peek_scratch));
    if (peeked_count == 0) {
        feb_wardriving_tx_in_flight = false;
        feb_status_led_set(FEB_STATUS_LED_CONNECTED);
        return;
    }

    memset(&wardriving_result_scratch, 0, sizeof(wardriving_result_scratch));
    include_count = 0;
    pending_now = wardriving_log_pending_count();
    while (include_count < peeked_count) {
        size_t trial_len;

        trial = wardriving_result_scratch;
        trial.records[trial.record_count] = peeked[include_count];
        trial.record_count++;
        trial.backlog_remaining = (pending_now >= trial.record_count) ?
                                  (pending_now - trial.record_count) : 0;
        trial_len = feb_cbor_encode_wardriving_status_result_payload(
            wardriving_result_buf_scratch, sizeof(wardriving_result_buf_scratch), &trial);
        if (trial_len == 0 || trial_len + FEB_WARDRIVING_STATUS_ENCODE_HEADROOM > FEB_CBOR_MAX_PAYLOAD) {
            if (wardriving_result_scratch.record_count == 0) {
                /* A single record's own encoding is already too large to ever fit -- should
                   be unreachable given WD_RECORD_MAX_PAYLOAD's derivation, but drop it rather
                   than spin forever re-peeking the same record every batch. Still counted in
                   include_count so wardriving_log_mark_drained() consumes it below and moves
                   the oldest-pending pointer past it. */
                ESP_LOGE(TAG, "wardriving: single record too large to encode; dropping it");
                include_count++;
                continue;
            }
            break;
        }
        wardriving_result_scratch = trial;
        include_count++;
    }

    remaining_after = (pending_now >= include_count) ? (pending_now - include_count) : 0;
    wardriving_result_scratch.backlog_remaining = remaining_after;
    result_len = feb_cbor_encode_wardriving_status_result_payload(
        wardriving_result_buf_scratch, sizeof(wardriving_result_buf_scratch), &wardriving_result_scratch);

    memset(&wardriving_status_payload_scratch, 0, sizeof(wardriving_status_payload_scratch));
    wardriving_status_payload_scratch.request_id = 0; /* unsolicited/live sentinel --
        docs/PROTOCOL.md: no wire distinction from a backlog-drain batch; backlog_remaining is
        how a receiver tells them apart */
    wardriving_status_payload_scratch.state = "data";
    wardriving_status_payload_scratch.state_len = strlen("data");
    wardriving_status_payload_scratch.result_span = wardriving_result_buf_scratch;
    wardriving_status_payload_scratch.result_span_len = result_len;
    wardriving_status_payload_scratch.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf),
                                                 &wardriving_status_payload_scratch);
    if (result_len == 0) {
        ESP_LOGE(TAG, "wardriving status(data) result encode failed (records=%u, peeked=%u)",
                 (unsigned)wardriving_result_scratch.record_count, (unsigned)peeked_count);
    } else if (payload_len == 0) {
        ESP_LOGE(TAG, "wardriving status(data) wrapper encode failed (result_len=%u)",
                 (unsigned)result_len);
    }
    /* Always chain through TX_DONE_CONTINUE_WARDRIVING, even when remaining_after == 0:
       feb_write_complete() only runs feb_tx_done_action once every fragment of *this* record has
       gone out, so this is what keeps feb_wardriving_tx_in_flight/feb_wardriving_pending_drain_count
       honest about the record actually being fully delivered rather than just its first
       fragment queued (2026-09-10 GATT-write-flood bug, see feb_wardriving_tx_in_flight's
       comment). The re-entry this triggers finds nothing left pending and clears the flag
       itself when this really was the last batch -- see the peeked_count == 0 branch above. */
    if (payload_len == 0) {
        ESP_LOGE(TAG, "failed to build wardriving status(data) record");
        feb_wardriving_tx_in_flight = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    if (!feb_queue_and_send_protected(conn_handle, "status", strlen("status"),
                                  feb_pairing_payload_encode_buf, payload_len,
                                  TX_DONE_CONTINUE_WARDRIVING)) {
        ESP_LOGE(TAG, "failed to queue wardriving status(data) record (payload_len=%u)",
                 (unsigned)payload_len);
        feb_wardriving_tx_in_flight = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    feb_wardriving_pending_drain_count = include_count;
    ESP_LOGI(TAG, "sending wardriving status(data) (%u record(s), backlog_remaining=%u)",
             (unsigned)wardriving_result_scratch.record_count, (unsigned)remaining_after);
}

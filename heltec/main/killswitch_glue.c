#include "feb_app_internal.h"
#include "board_hooks.h"
#include "radio_killswitch.h"
#include "status_display.h"

/* Touch-pad (GPIO2) radio kill-switch: radio_kill_switch_enabled gates feb_start_scan() (and is
   checked before any BLE reconnect/scan attempt) so a shutdown-in-progress cannot race a
   fresh scan being armed; it doubles as "nimble_port_init() currently owns the BLE stack"
   (only ever set true right after a successful nimble_port_freertos_init(), in app_main() or
   feb_radio_kill_switch_toggle()'s own "enable" branch -- see that function for why a single
   flag is safe to reuse for both meanings, and for why its own shutdown-cleanup runs directly
   on the touch task rather than being handed off to the NimBLE host task the way
   wardriving_button_toggle_ev does -- a DRAM-budget trade-off, not an oversight). */
static volatile bool radio_kill_switch_enabled;

/* feb_app_hooks_t.radio_permitted: feb_start_scan() starts nothing while the radio is off. */
bool feb_board_radio_permitted(void)
{
    return radio_kill_switch_enabled;
}

/* app_main(): sets the flag once nimble_port_freertos_init() has (or has not) been called. */
void feb_board_radio_enabled_set(bool enabled)
{
    radio_kill_switch_enabled = enabled;
}

/* Declared in radio_killswitch.h. Called only from the touch debounce task, never the NimBLE
   host task -- nimble_port_stop() blocks waiting for the host task's own event loop to reach
   and process a stop event it posts, so calling it from the host task would deadlock (see
   radio_kill_switch_enabled's comment above). Handles both boot-time states: a boot with the
   persisted flag "off" never calls nimble_port_init()/start_wifi_subsystem() at all (see
   app_main()), so radio_kill_switch_enabled starts false and the very first touch-to-enable
   takes the "enable" branch directly, with no stack to stop first -- radio_kill_switch_enabled
   is only ever set true right after a nimble_port_init()/nimble_port_freertos_init() call
   actually succeeds (here or in app_main()), so its old value at entry doubles as "is the BLE
   stack currently up", with no separate flag needed.

   The shutdown-side cleanup below (cancel discovery, terminate any live connection, stop a
   manual/wardriving scan in flight) runs directly on this task rather than being handed off to
   the NimBLE host task the way wardriving's own boot-button toggle does its equivalent work --
   a deliberate trade-off, not an oversight: this board's DRAM budget is already down to a
   double-digit number of free bytes (docs/BACKLOG.md BL23/BL24), and the raw ble_npl_event +
   semaphore this would otherwise need to hand off to the host task safely pushed a real
   idf.py build over budget. radio_kill_switch_enabled is still flipped first (so feb_start_scan()
   and any other radio_kill_switch_enabled-gated code cannot re-arm a scan mid-teardown), and
   ble_gap_disc_cancel()/ble_gap_terminate() are NimBLE APIs that are safe to call from any
   task (they take ble_hs's own internal lock) -- most of what's raced against the host task
   here is this file's own plain bool/uint32 globals (feb_wifi_scan_in_progress,
   feb_wardriving_wifi_active, etc.), for the brief window between a touch gesture and
   nimble_port_stop() actually halting the host task. A physical touch is a rare, operator-paced
   event, not a hot path, and every one of those globals is a simple flag with no invariant that
   a stale read could corrupt (worst case: one redundant or skipped stop call, already tolerated
   elsewhere via ESP_ERR_WIFI_NOT_STARTED/BLE_HS_EALREADY handling) -- an accepted, explicitly
   flagged risk given the hard memory ceiling, not a silent shortcut.

   One exception, corrected here (HARDENING_PLAN.md HP-04, was previously claimed not to
   exist): feb_wardriving_stop_internal()'s cluster-delegated branch frees a malloc'd buffer
   (wardriving_cluster_flush_records) that feb_wifi_scan_done_cb() (host task) can be
   concurrently reading -- not a "stale flag read", a real use-after-free. Narrowed (not
   perfectly closed, see feb_wardriving_stop_internal()'s own comment) by moving the pointer
   read/NULL under cluster_link_spinlock and freeing only a local copy after releasing it. */
void feb_radio_kill_switch_toggle(void)
{
    bool enable = !radio_kill_switch_enabled;

    if (!enable) {
        radio_kill_switch_enabled = false;

        if (feb_wardriving_wifi_active || feb_wardriving_ble_active) {
            feb_wardriving_stop_internal();
        }
        if (feb_wifi_scan_in_progress) {
            esp_err_t serr = esp_wifi_scan_stop();

            if (serr != ESP_OK && serr != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_scan_stop failed during radio kill-switch shutdown: %s",
                         esp_err_to_name(serr));
            }
            feb_wifi_scan_in_progress = false;
        }
        {
            int derr = ble_gap_disc_cancel();

            if (derr != 0 && derr != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "ble_gap_disc_cancel failed during radio kill-switch shutdown: %d",
                         derr);
            }
        }
        feb_ble_scan_in_progress = false;
        if (feb_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        feb_board_control_ready_clear();
        /* Every callout feb_board_host_synced() (re-)initializes against nimble_port_get_dflt_eventq()
           -- stopped explicitly rather than relying on nimble_port_deinit()'s own teardown,
           since a still-pending timer posting to a queue about to be deinitialized/
           reinitialized from under it is not a risk worth taking. */
        ble_npl_callout_stop(&feb_reassembly_timeout_co);
        ble_npl_callout_stop(&feb_wifi_scan_done_co);
        ble_npl_callout_stop(&feb_cluster_scan_done_co);
        ble_npl_callout_stop(&feb_cluster_scan_timeout_co);
        ble_npl_callout_stop(&feb_ble_scan_done_co);
        ble_npl_callout_stop(&feb_wardriving_wifi_interval_co);
        ble_npl_callout_stop(&feb_wardriving_ble_interval_co);
        ble_npl_callout_stop(&feb_reconnect_co);

        {
            int rc = nimble_port_stop();

            if (rc != 0) {
                ESP_LOGE(TAG, "radio kill-switch: nimble_port_stop failed: %d", rc);
            } else {
                esp_err_t derr = nimble_port_deinit();

                if (derr != ESP_OK) {
                    ESP_LOGE(TAG, "radio kill-switch: nimble_port_deinit failed: %s",
                             esp_err_to_name(derr));
                }
            }
        }
        {
            esp_err_t werr = esp_wifi_stop();

            if (werr != ESP_OK && werr != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "radio kill-switch: esp_wifi_stop failed: %s", esp_err_to_name(werr));
            }
        }
        feb_status_display_set_ble_state(FEB_DISPLAY_BLE_RADIO_OFF);
        ESP_LOGW(TAG, "radio kill-switch: Wi-Fi/BLE stopped");
    } else {
        esp_err_t werr;

        /* HP-05: calls the init half (a no-op after its first-ever successful run, whether
           that was app_main() at boot or an earlier kill-switch cycle) and the restartable
           start half separately, so a failure here is visible instead of the previous single
           start_wifi_subsystem() silently returning before esp_wifi_start() on every re-enable. */
        feb_board_wifi_subsystem_init();
        werr = feb_board_wifi_subsystem_start();
        if (werr != ESP_OK) {
            ESP_LOGE(TAG, "radio kill-switch: Wi-Fi restart failed (%s); BLE will still be "
                          "restarted below, but Wi-Fi scanning/wardriving remain unavailable "
                          "until the next kill-switch cycle or reboot", esp_err_to_name(werr));
            feb_status_display_set_ble_state(FEB_DISPLAY_BLE_WIFI_RESTART_FAILED);
        }
        {
            esp_err_t nerr = nimble_port_init();

            if (nerr != ESP_OK) {
                ESP_LOGE(TAG, "radio kill-switch: nimble_port_init failed: %s",
                         esp_err_to_name(nerr));
                feb_radio_kill_switch_persist(false);
                return;
            }
        }
        ble_hs_cfg.sync_cb = feb_board_host_synced;
        nimble_port_freertos_init(feb_board_nimble_host_task);
        radio_kill_switch_enabled = true;
        ESP_LOGI(TAG, "radio kill-switch: Wi-Fi/BLE restarting");
    }
    feb_radio_kill_switch_persist(enable);
}

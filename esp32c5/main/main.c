#include "feb_app_internal.h"
#include "wifi_band.h"

/* docs/PLAN.md Phase 8 step 3: base BLE transport/pairing/session-auth layer and the
   `wifi_scan`/`ble_scan`/`gps`/`wardriving` capabilities now live in the shared
   components/feb_app_core (docs/SOURCE_SPLIT.md 5.2/5.3 step 3) -- this file keeps only what's
   genuinely this board's own: Wi-Fi subsystem bring-up (dual-band), the NimBLE host-sync/
   boot sequence, the feature list, and the command dispatcher the core calls back into. See
   components/feb_app_core/feb_app_core.h's board-contract comment and
   esp32/main/main.c (the C6, this board's original porting reference) for the equivalent
   file. Any wire-format/crypto logic below must stay byte-for-byte identical to the C6/Heltec
   builds and docs/PROTOCOL.md.

   Unlike the C6/Heltec, this board has no onboard pushbutton
   (docs/hardware/olimex-mod-esp32-c5/README.md), so board_config.h sets FEB_HAS_BOOT_BUTTON 0
   and there is no board_hooks.c/factory_reset.c here -- docs/BACKLOG.md BL15 tracks this as a
   deliberate scope cut, not an oversight. This is also the first board with a native
   dual-band (2.4GHz+5GHz) Wi-Fi 6 radio (board_config.h's FEB_WIFI_DUAL_BAND 1); wifi_band.c
   implements the feb_app_hooks_t band-selection hooks the core calls at every
   esp_wifi_scan_start() site and at wardriving start (docs/PROTOCOL.md's `wifi_band` row). */

static const feb_app_hooks_t feb_board_hooks = {
    .wifi_scan_cfg_ext = feb_board_wifi_band_apply_scan_cfg,
    .wardriving_start_ext = feb_board_wifi_band_start_ext,
};

/* Set the first time host_synced() runs its autostart-from-persisted-state block, so a
   later NimBLE resync (ble_hs_reset() re-invokes sync_cb) never re-attempts it -- see
   host_synced()'s comment. */
static bool wardriving_autostart_attempted;
static void nimble_host_task(void *arg);
static void start_wifi_subsystem(void);

const char *const feb_features[] = {"wifi_scan", "ble_scan", "gps", "wardriving"};
const size_t feb_feature_count = sizeof(feb_features) / sizeof(feb_features[0]);

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": capability-name
   dispatch. Ported unchanged (dispatch order) from the pre-split esp32c5/main/main.c. */
void feb_handle_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    if (cmd->capability_len == strlen("wifi_scan") &&
        memcmp(cmd->capability, "wifi_scan", cmd->capability_len) == 0) {
        feb_handle_wifi_scan_command(conn_handle, cmd);
    } else if (cmd->capability_len == strlen("ble_scan") &&
              memcmp(cmd->capability, "ble_scan", cmd->capability_len) == 0) {
        feb_handle_ble_scan_command(conn_handle, cmd);
    } else if (cmd->capability_len == strlen("wardriving") &&
              memcmp(cmd->capability, "wardriving", cmd->capability_len) == 0) {
        feb_handle_wardriving_command(conn_handle, cmd);
    } else if (cmd->capability_len == strlen("gps") &&
              memcmp(cmd->capability, "gps", cmd->capability_len) == 0) {
        feb_handle_gps_command(conn_handle, cmd);
    } else if (!feb_send_protected_error(conn_handle, "unsupported_capability", strlen("unsupported_capability"),
                                     1, cmd->request_id)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* docs/PROTOCOL.md "`wifi_scan` command and status payloads": esp_netif/default event
   loop/esp_wifi initialize once at boot, STA mode, never connecting to anything, and stay
   resident for the device's whole lifetime. Not lazy-initialized on first wifi_scan command.
   Ported from esp32/main/main.c, plus one addition this chip needs that neither the C6 nor the
   Heltec do: this is the first board in this project with a native dual-band (2.4GHz+5GHz)
   Wi-Fi 6 radio (docs/hardware/olimex-mod-esp32-c5/README.md), and its band mode defaults to
   WIFI_BAND_MODE_AUTO (2.4G+5G) on a chip with CONFIG_SOC_WIFI_SUPPORT_5G (confirmed against
   this board's generated sdkconfig). **Scope reversal 2026-09-25** (docs/PLAN.md Phase 8 "Step
   3"): the original 2.4GHz-only restriction is lifted -- esp_wifi_set_band_mode() is called
   with WIFI_BAND_MODE_AUTO explicitly (rather than relying on the chip's own default, which is
   already WIFI_BAND_MODE_AUTO, to keep this call self-documenting and to keep a single place to
   revert if the scope is ever narrowed again) right after esp_wifi_start() succeeds (the API
   requires WiFi already started; ESP_ERR_WIFI_NOT_STARTED otherwise). No wire-protocol change:
   docs/PROTOCOL.md's wifi_scan `channel` field is a bare channel number and 2.4GHz (1-14) and
   5GHz (36+) channel numbers never overlap. **2026-09-26 update**: a runtime band-switching
   command now exists after all -- docs/PROTOCOL.md's `wifi_band` row on wardriving's `start`
   action -- see wifi_band.c's feb_board_wifi_band_start_ext()/feb_board_wifi_band_apply_scan_cfg().
   The esp_wifi_set_band_mode(AUTO) call below remains this board's boot-time default (matching
   wifi_band.c's own WIFI_BAND_5GHZ_FULL default) until a wardriving `start` command's
   `wifi_band` field changes it.

   Radio-coexistence caveat: unlike the C6 (whose concurrent Wi-Fi-scan + BLE-connection
   behavior was validated in an earlier step) and matching the Heltec's own accepted gap, this
   chip's Wi-Fi 6 + BLE 5 + 802.15.4 single-radio combo has never been swept for concurrent
   Wi-Fi scan + active BLE connection stability -- a real, currently-untested gap for this
   port, not a borrowed-and-verified number; flagged per docs/BASELINES.md's MOD-ESP32-C5 entry.
   This gap now also covers sustained wardriving-driven concurrent Wi-Fi+BLE load (not just a
   manual one-shot scan) -- see docs/BACKLOG.md BL16. */
static void start_wifi_subsystem(void)
{
    esp_err_t err = esp_netif_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return;
    }
    (void)esp_netif_create_default_wifi_sta();

    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return;
        }
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              &feb_wifi_scan_done_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi scan-done handler registration failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_band_mode(AUTO) failed: %s -- dual-band scanning may not "
                      "be available", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "wifi band mode set to dual-band (2.4GHz + 5GHz)");
    }
}

static void host_synced(void)
{
    int rc = ble_hs_id_infer_auto(0, &feb_own_addr_type);

    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        return;
    }
    ble_npl_callout_init(&feb_reassembly_timeout_co, nimble_port_get_dflt_eventq(),
                        feb_reassembly_timeout_cb, NULL);
    ble_npl_callout_reset(&feb_reassembly_timeout_co,
                          ble_npl_time_ms_to_ticks32(FEB_REASSEMBLY_CHECK_INTERVAL_MS));
    ble_npl_callout_init(&feb_wifi_scan_done_co, nimble_port_get_dflt_eventq(), feb_wifi_scan_done_cb, NULL);
    ble_npl_callout_init(&feb_ble_scan_done_co, nimble_port_get_dflt_eventq(), feb_ble_scan_window_close_cb, NULL);
    ble_npl_callout_init(&feb_wardriving_wifi_interval_co, nimble_port_get_dflt_eventq(),
                        feb_wardriving_wifi_interval_cb, NULL);
    ble_npl_callout_init(&feb_wardriving_ble_interval_co, nimble_port_get_dflt_eventq(),
                        feb_wardriving_ble_interval_cb, NULL);
    ble_npl_callout_init(&feb_reconnect_co, nimble_port_get_dflt_eventq(), feb_reconnect_timer_cb, NULL);

    /* docs/BACKLOG.md "Per-board wardriving autostart setting": resume before feb_start_scan()
       below, so its existing "never run a second reconnect scan while wardriving's BLE
       source owns discovery" guard (feb_wardriving_ble_active) already sees the right value on
       its very first call. Guarded by wardriving_autostart_attempted so this only ever runs
       on the first host_synced() call -- see esp32/main/main.c's fuller comment on why (NimBLE
       resync after ble_hs_reset() must not re-attempt this). Unlike the C6/Heltec, there is no
       boot-button-toggle path to also guard here (docs/BACKLOG.md BL15). */
    if (!wardriving_autostart_attempted) {
        wardriving_autostart_attempted = true;
        if (feb_wardriving_persisted.enabled) {
            /* Autostart reuses the persisted wifi_swelling/country (wardriving_persist.h v2) but
               deliberately stays on 2.4GHz: dual-band + BLE-connection coexistence has never
               been swept on this board (BACKLOG.md BL16). wifi_band is still persisted, so
               switching to feb_wardriving_persisted.wifi_band is a one-line change once that
               sweep passes. */
            if (!feb_wardriving_start_internal(feb_wardriving_persisted.want_wifi, feb_wardriving_persisted.want_ble,
                                           feb_wardriving_persisted.want_ble_passive,
                                           feb_wardriving_persisted.wifi_interval_ms,
                                           feb_wardriving_persisted.ble_window_ms,
                                           feb_wardriving_persisted.ble_interval_ms,
                                           (wardriving_swelling_mode_t)feb_wardriving_persisted.wifi_swelling,
                                           (wardriving_country_t)feb_wardriving_persisted.country,
                                           0)) {
                ESP_LOGW(TAG, "wardriving autostart failed; leaving stopped");
            } else {
                ESP_LOGI(TAG, "wardriving autostarted from persisted state (wifi=%d ble=%d ble_passive=%d)",
                         (int)feb_wardriving_persisted.want_wifi, (int)feb_wardriving_persisted.want_ble,
                         (int)feb_wardriving_persisted.want_ble_passive);
            }
        }
    }

    ESP_LOGI(TAG, "starting v2 service-filtered scan");
    feb_start_scan();
}

static void nimble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t err;

    feb_app_core_init(&feb_board_hooks);

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    feb_status_led_init();
    /* docs/BACKLOG.md BL15: no onboard pushbutton on this board -- no feb_factory_reset_start()
       call here, unlike the C6/Heltec. */

    location_init();
    wardriving_log_init();
    wardriving_persist_load(&feb_wardriving_persisted);

    feb_compute_board_id();
    if (feb_load_pairing_secret(feb_stored_pairing_secret)) {
        feb_boot_mode = FEB_BOOT_MODE_RUNTIME_AUTH;
        ESP_LOGI(TAG, "board_id=%s: stored pairing_secret found; attempting runtime auth "
                      "(no pairing window opened)", feb_board_id_buf);
    } else {
        feb_boot_mode = FEB_BOOT_MODE_PAIRING;
        esp_fill_random(feb_pairing_epoch, sizeof(feb_pairing_epoch));
        feb_pairing_window_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "board_id=%s: no stored pairing_secret; pairing window open for %u ms",
                 feb_board_id_buf, (unsigned)FEB_PAIRING_WINDOW_MS);
    }

    start_wifi_subsystem();

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = host_synced;
    nimble_port_freertos_init(nimble_host_task);
}

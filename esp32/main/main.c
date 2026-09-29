#include "feb_app_internal.h"
#include "board_hooks.h"
#include "factory_reset.h"

const char *const feb_features[] = {"wifi_scan", "ble_scan", "wardriving", "gps"};
const size_t feb_feature_count = sizeof(feb_features) / sizeof(feb_features[0]);

/* All NULL: the C6 needs no runtime hook (its boot button is compile-time, FEB_HAS_BOOT_BUTTON). */
static const feb_app_hooks_t feb_board_hooks = {0};

/* Set the first time host_synced() runs its autostart-from-persisted-state block, so a
   later NimBLE resync (ble_hs_reset() re-invokes sync_cb) never re-attempts it -- see
   host_synced()'s comment. */
static bool wardriving_autostart_attempted;
static void nimble_host_task(void *arg);
static void start_wifi_subsystem(void);

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": capability-name
   dispatch. A lookup table isn't earned yet at four entries (docs/SESSION_MEMORY.md's design
   note). */
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

/* docs/PLAN.md "Wi-Fi scan capability" step: esp_netif/default event loop/esp_wifi
   initialize once at boot, STA mode, never connecting to anything, and stay resident for the
   device's whole lifetime -- matching the future `wardriving` capability's always-on-radio
   need and step 4's already-validated Wi-Fi/BLE coexistence behavior. Not lazy-initialized on
   first wifi_scan command. */
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
    /* A raw event, not a 7th callout -- see wardriving_button_toggle_ev's comment (the
       existing 6 callouts above already sit at ESP-IDF's hard NimBLE host callout-pool
       ceiling once NimBLE's own internal host procedures are counted in). */
    feb_board_button_events_init();

    /* docs/BACKLOG.md "Per-board wardriving autostart setting": resume before feb_start_scan()
       below, so its existing "never run a second reconnect scan while wardriving's BLE
       source owns discovery" guard (feb_wardriving_ble_active) already sees the right value
       on its very first call.

       Guarded by wardriving_autostart_attempted so this only ever runs on the *first*
       host_synced() call. NimBLE re-invokes sync_cb after any ble_hs_reset() (HCI
       timeout, controller fault), and without this guard a resync landing while
       wardriving was already running would hit feb_wardriving_start_internal()'s own
       already-active guard, read that as "autostart failed", and persist enabled=false --
       silently and permanently disabling autostart over a transient host-level event that
       has nothing to do with wardriving. */
    if (!wardriving_autostart_attempted) {
        wardriving_autostart_attempted = true;
        if (feb_wardriving_persisted.enabled) {
            if (!feb_wardriving_start_internal(feb_wardriving_persisted.want_wifi, feb_wardriving_persisted.want_ble,
                                           feb_wardriving_persisted.want_ble_passive,
                                           feb_wardriving_persisted.wifi_interval_ms,
                                           feb_wardriving_persisted.ble_window_ms,
                                           feb_wardriving_persisted.ble_interval_ms,
                                           /* wardriving_persist.h (version 2) now persists the
                                              last `start` command's wifi_swelling/country (see
                                              board_hooks.c's matching comment) -- autostart
                                              reuses them instead of an unconfigured-radio
                                              default. */
                                           (wardriving_swelling_mode_t)feb_wardriving_persisted.wifi_swelling,
                                           (wardriving_country_t)feb_wardriving_persisted.country)) {
                /* Does NOT clear/persist enabled -- same rationale as feb_wardriving_self_stop():
                   a failed radio start is not the user turning wardriving off, so the
                   saved intent survives for the next boot attempt. */
                ESP_LOGW(TAG, "wardriving autostart failed; leaving stopped");
            } else {
                ESP_LOGI(TAG, "wardriving autostarted from persisted state (wifi=%d ble=%d ble_passive=%d)",
                         (int)feb_wardriving_persisted.want_wifi, (int)feb_wardriving_persisted.want_ble,
                         (int)feb_wardriving_persisted.want_ble_passive);
            }
        }
    }
    feb_board_control_ready_set();

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
    feb_factory_reset_start();

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

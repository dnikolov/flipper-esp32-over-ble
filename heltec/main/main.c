#include "feb_app_internal.h"
#include "board_hooks.h"
#include "factory_reset.h"
#include "lora_shared_radio.h"
#include "mesh_log.h"
#include "radio_killswitch.h"
#include "status_display.h"

/* docs/SOURCE_SPLIT.md 5.2/5.3 step 4: the base BLE transport/pairing/session-auth layer and
   the `wifi_scan`/`ble_scan`/`gps`/`wardriving` capabilities now live in the shared
   components/feb_app_core -- see its feb_app_core.h board-contract comment and
   esp32/main/main.c for the equivalent C6 file. This file keeps what is genuinely this
   board's own: Wi-Fi subsystem bring-up (split init/start for the radio kill switch), the
   NimBLE host-sync/boot sequence, the feature list, the command dispatcher, and the
   feb_app_hooks_t table. The rest of this board's glue: cluster_glue.c (Phase 9 cluster
   worker), mesh_caps.c (meshcore/meshtastic capabilities, mesh_log drain),
   killswitch_glue.c (touch-pad radio kill switch), board_hooks.c (PRG button/factory reset).

   Port history. docs/PLAN.md Phase 4 step 3: base BLE transport/pairing/session-auth layer ported from
   esp32/main/main.c onto this board's classic-ESP32 NimBLE central role, reusing
   components/feb_protocol/ unchanged. Any wire-format/crypto logic below must stay
   byte-for-byte identical to the C6 build and docs/PROTOCOL.md -- see that file for the
   equivalent implementation and its own inline rationale, not repeated here where behavior
   is unchanged.

   Phase 4 capability-porting pass: `wifi_scan` and `ble_scan` ported 2026-09-17 (manual-scan
   paths only); `gps` ported 2026-09-23 once the user rewired the bench-tested GPS module from
   GPIO36 to GPIO17 (location.c/nmea_parser.c, copied from esp32/main/ with only the
   board-specific pins in location.c changed -- see docs/hardware/heltec-wifi-lora-32-v2/
   README.md); it is a poll-only status query with no scan-duration lifecycle and no
   dependency on the Wi-Fi/BLE radio, so it carried none of wardriving's coexistence-bound
   risk.

   `wardriving` ported 2026-09-23, by explicit user request (docs/PLAN.md Phase 4 step 5's
   coexistence sweep was explicitly skipped 2026-09-16 and has NOT been done retroactively --
   this port reuses the C6's already-validated interval defaults
   (wifi_interval_ms=5000/ble_window_ms=100/ble_interval_ms=500) as an unvalidated-but-
   conservative starting point on this board's structurally different Wi-Fi4+BT-Classic/BLE4.2
   combo radio, not a borrowed guarantee -- see docs/BASELINES.md's Heltec entry and
   components/feb_app_core's feb_wardriving_start_internal()/feb_wardriving_wifi_interval_cb()
   comments). All five
   wardriving_*.c/h support files (record format, validation, dedup, raw-flash circular log,
   persisted settings) are copied unchanged from esp32/main/ -- they are board-agnostic engine
   code with no pin/radio-specific logic of their own; see heltec/partitions.csv for this
   board's own "wardrive" data partition (sized for its 8MB flash, not the C6's 4MB). The
   NVS-persisted per-run settings (WiFi swelling, country, sources, intervals) and the GPS
   fix-dependency record-level discard (docs/PLAN.md's "Real GPS driver..." section) are wired
   identically to the C6. feb_features[] and feb_handle_command()'s dispatch now reflect
   wifi_scan/ble_scan/gps/wardriving. */

const char *const feb_features[] = {"wifi_scan", "ble_scan", "gps", "wardriving", "meshcore_scan", "meshtastic_scan", "mesh_log"};
const size_t feb_feature_count = sizeof(feb_features) / sizeof(feb_features[0]);

/* feb_app_hooks_t.on_ble_state: feb_ble_link_state_t mirrors the OLED's BLE-line states
   value-for-value. */
_Static_assert((int)FEB_BLE_LINK_DISCONNECTED == (int)FEB_DISPLAY_BLE_DISCONNECTED, "link state");
_Static_assert((int)FEB_BLE_LINK_SCANNING == (int)FEB_DISPLAY_BLE_SCANNING, "link state");
_Static_assert((int)FEB_BLE_LINK_PAIRING_MODE == (int)FEB_DISPLAY_BLE_PAIRING_MODE, "link state");
_Static_assert((int)FEB_BLE_LINK_CONNECTING == (int)FEB_DISPLAY_BLE_CONNECTING, "link state");
_Static_assert((int)FEB_BLE_LINK_PAIRING == (int)FEB_DISPLAY_BLE_PAIRING, "link state");
_Static_assert((int)FEB_BLE_LINK_AUTHENTICATING == (int)FEB_DISPLAY_BLE_AUTHENTICATING, "link state");
_Static_assert((int)FEB_BLE_LINK_AUTHENTICATED == (int)FEB_DISPLAY_BLE_AUTHENTICATED, "link state");

static void board_on_ble_state(feb_ble_link_state_t state)
{
    feb_status_display_set_ble_state((feb_display_ble_state_t)state);
}

static const feb_app_hooks_t feb_board_hooks = {
    .on_tx_done_ext = feb_mesh_log_send_next_batch,
    .on_ble_state = board_on_ble_state,
    .radio_permitted = feb_board_radio_permitted,
    .on_connect = feb_mesh_log_on_connect,
    .on_disconnect = feb_mesh_log_on_disconnect,
    .on_authenticated = feb_mesh_log_maybe_kick_send,
};

static bool wardriving_autostart_attempted;

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": capability-name
   dispatch. */
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
    } else if (cmd->capability_len == strlen("meshcore_scan") &&
              memcmp(cmd->capability, "meshcore_scan", cmd->capability_len) == 0) {
        feb_handle_meshcore_command(conn_handle, cmd);
    } else if (cmd->capability_len == strlen("meshtastic_scan") &&
              memcmp(cmd->capability, "meshtastic_scan", cmd->capability_len) == 0) {
        feb_handle_meshtastic_command(conn_handle, cmd);
    } else if (!feb_send_protected_error(conn_handle, "unsupported_capability", strlen("unsupported_capability"),
                              1, cmd->request_id)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* docs/PROTOCOL.md "`wifi_scan` command and status payloads": esp_netif/default event
   loop/esp_wifi initialize once at boot, STA mode, never connecting to anything, and stay
   resident for the device's whole lifetime. Not lazy-initialized on first wifi_scan command.
   See this file's top-of-file comment and docs/BASELINES.md/.claude/agents/heltec-developer.md
   for this board's untested-versus-validated coexistence status: unlike the C6 (whose Wi-Fi 6
   + BLE 5 + 802.15.4 radio had its concurrent-scan behavior validated in an earlier step), this
   board's Wi-Fi 4 + BT Classic/BLE 4.2 combo radio has never been exercised running a Wi-Fi
   scan concurrently with an active BLE connection to the Flipper -- that is a real,
   currently-untested gap for this port, not a borrowed-and-verified number.

   Split into an idempotent one-time init half and a restartable start half
   (HARDENING_PLAN.md HP-05/HP-29): esp_event_loop_create_default()/esp_wifi_init() are not
   safe to call a second time (the former returns ESP_ERR_INVALID_STATE), but the original
   single function re-ran both of them on every radio kill-switch OFF->ON cycle -- the second
   call always failed and returned before ever reaching esp_wifi_start(), so Wi-Fi stayed dead
   after a kill-switch re-enable until a power cycle, silently (nothing on the OLED or the wire
   showed it). wifi_subsystem_init_done guards the init half so it runs at most once per boot,
   regardless of whether that first call comes from app_main() (radio persisted on) or from
   feb_radio_kill_switch_toggle()'s enable branch (radio persisted off at boot, so the very
   first touch-to-enable is this device's first-ever Wi-Fi init). */
static bool wifi_subsystem_init_done = false;

void feb_board_wifi_subsystem_init(void)
{
    esp_err_t err;

    if (wifi_subsystem_init_done) {
        return;
    }
    err = esp_netif_init();
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
    wifi_subsystem_init_done = true;
}

/* Restartable half: esp_wifi_set_mode()/esp_wifi_start() are designed to be called again
   after a matching esp_wifi_stop(), unlike feb_board_wifi_subsystem_init()'s steps above. Returns
   esp_err_t (HP-05) so a caller can detect and report failure instead of the original void
   function's silent "logged and returns", which let feb_radio_kill_switch_toggle() persist
   enabled=true and log "restarting" even when Wi-Fi never actually came back. */
esp_err_t feb_board_wifi_subsystem_start(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Boot-time convenience: full bring-up in one call, used only by app_main(). The kill-switch's
   re-enable path calls feb_board_wifi_subsystem_init()/feb_board_wifi_subsystem_start() directly
   instead, so it can react to start()'s failure (see feb_radio_kill_switch_toggle()). */
static void start_wifi_subsystem(void)
{
    feb_board_wifi_subsystem_init();
    (void)feb_board_wifi_subsystem_start();
}

void feb_board_host_synced(void)
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
    ble_npl_callout_init(&feb_cluster_scan_done_co, nimble_port_get_dflt_eventq(), feb_cluster_scan_done_cb, NULL);
    ble_npl_callout_init(&feb_cluster_scan_timeout_co, nimble_port_get_dflt_eventq(),
                        feb_cluster_scan_timeout_cb, NULL);
    ble_npl_callout_init(&feb_ble_scan_done_co, nimble_port_get_dflt_eventq(), feb_ble_scan_window_close_cb, NULL);
    ble_npl_callout_init(&feb_wardriving_wifi_interval_co, nimble_port_get_dflt_eventq(),
                        feb_wardriving_wifi_interval_cb, NULL);
    ble_npl_callout_init(&feb_wardriving_ble_interval_co, nimble_port_get_dflt_eventq(),
                        feb_wardriving_ble_interval_cb, NULL);
    ble_npl_callout_init(&feb_reconnect_co, nimble_port_get_dflt_eventq(), feb_reconnect_timer_cb, NULL);
    /* A raw event, not a 7th callout -- see board_hooks.c's wardriving_button_toggle_ev comment
       and esp32/main/board_hooks.c's fuller one on the same mechanism. */
    feb_board_button_events_init();

    /* docs/BACKLOG.md "Per-board wardriving autostart setting", ported unchanged from
       esp32/main/main.c -- see that file's fuller comment (resume before feb_start_scan() below,
       guarded against a later NimBLE resync re-attempting it). */
    if (!wardriving_autostart_attempted) {
        wardriving_autostart_attempted = true;
        if (feb_wardriving_persisted.enabled) {
            if (!feb_wardriving_start_internal(feb_wardriving_persisted.want_wifi, feb_wardriving_persisted.want_ble,
                                           feb_wardriving_persisted.want_ble_passive,
                                           feb_wardriving_persisted.wifi_interval_ms,
                                           feb_wardriving_persisted.ble_window_ms,
                                           feb_wardriving_persisted.ble_interval_ms,
                                           /* wardriving_persist.h (version 2) now persists the
                                              last `start` command's wifi_swelling/country --
                                              autostart reuses them instead of an unconfigured-
                                              radio default. */
                                           (wardriving_swelling_mode_t)feb_wardriving_persisted.wifi_swelling,
                                           (wardriving_country_t)feb_wardriving_persisted.country)) {
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

void feb_board_nimble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t err;
    bool radio_on;

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

    /* Loaded here, before start_wifi_subsystem()/nimble_port_init() below, so a reboot with
       the touch-pad kill-switch persisted "off" comes up with Wi-Fi/BLE already suppressed
       rather than started-then-stopped. */
    radio_on = feb_radio_kill_switch_load_persisted();
    /* Left false here regardless of radio_on -- only set true below once
       nimble_port_freertos_init() has actually been called, so it always accurately reflects
       "the BLE stack is up" (see feb_radio_kill_switch_toggle()'s own comment on why that
       invariant matters). */
    feb_board_radio_enabled_set(false);

    feb_status_led_init();
    feb_factory_reset_start();
    feb_status_display_start();
    feb_radio_kill_switch_start();

    location_init();
    feb_cluster_link_init();
    /* mesh_log_init() must run before lora_shared_radio_init() starts the LoRa RX task below --
       that task can call mesh_log_record_sighting() as soon as it starts receiving, and this
       ordering closes what would otherwise be a narrow startup race (a frame arriving before
       mesh_log_init() has created its mutex/found its partition). mesh_log_record_sighting()
       already guards on `ml_mutex == NULL` defensively either way, so this is belt-and-braces,
       not fixing an observed crash. */
    mesh_log_init();
    lora_shared_radio_init();
    wardriving_log_init();
    wardriving_persist_load(&feb_wardriving_persisted);
    /* docs/WARDRIVING_PUBLISH.md "Mesh node publishing": mesh_log's dedup table sizing was
       supposed to be picked from a real esp_get_free_heap_size() reading on this board with
       Wi-Fi+BLE+LoRa+GPS all already initialized (this line exists for that measurement) --
       no physical board was available this session to actually read it, so this build uses
       the flash-scan dedup fallback instead of a heap-allocated table (see mesh_log.c's top
       comment); this log line is left in place so the next hardware session can read the real
       number and reconsider. */
    ESP_LOGI(TAG, "free heap after storage/radio init: %u bytes", (unsigned)esp_get_free_heap_size());

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

    if (radio_on) {
        start_wifi_subsystem();

        err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(err));
            return;
        }
        ble_hs_cfg.sync_cb = feb_board_host_synced;
        nimble_port_freertos_init(feb_board_nimble_host_task);
        feb_board_radio_enabled_set(true);
    } else {
        feb_status_display_set_ble_state(FEB_DISPLAY_BLE_RADIO_OFF);
        ESP_LOGW(TAG, "radio kill-switch: booting with Wi-Fi/BLE suppressed (persisted off)");
    }
}

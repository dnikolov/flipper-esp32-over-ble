#ifndef FEB_APP_CORE_H
#define FEB_APP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi_types.h"

#include "board_config.h"
#include "cbor_codec.h"

/* Shared application core (docs/SOURCE_SPLIT.md 5.2): BLE central, pairing/runtime auth,
   protected TX queue, and the wifi_scan/ble_scan/gps/wardriving capabilities, consumed by each
   board's main component.

   Board contract. The board's main component must provide:
   - board_config.h (FEB_BOARD_MODEL, FEB_FIRMWARE_VERSION, FEB_BOARD_ID_PREFIX, FEB_LOG_TAG,
     FEB_HAS_BOOT_BUTTON, FEB_WIFI_DUAL_BAND, FEB_DIAG_WIFI_HEAP_LOG, FEB_HAS_LINK_HOOKS,
     FEB_HAS_CLUSTER_WORKER), reached through the FEB_APP_CORE_BOARD_DIR build property set in
     the board project's top-level CMakeLists.txt;
   - with FEB_HAS_CLUSTER_WORKER, the feb_cluster_*() functions declared in
     feb_app_internal.h (an alternative Wi-Fi scan source);
   - status_led.h/location.h with the feb_status_led_*()/location_*() API;
   - the definitions declared below (feb_features[], feb_feature_count, feb_handle_command());
   - host_synced()/app_main(): NimBLE callouts and events are initialized there, in the
     board's own fixed order (BLE_HOST_CO_COUNT is a hard cap of 8; the core creates none). */

#if FEB_HAS_LINK_HOOKS
/* BLE link state passed to feb_app_hooks_t.on_ble_state. */
typedef enum {
    FEB_BLE_LINK_DISCONNECTED = 0,
    FEB_BLE_LINK_SCANNING,
    FEB_BLE_LINK_PAIRING_MODE,
    FEB_BLE_LINK_CONNECTING,
    FEB_BLE_LINK_PAIRING,
    FEB_BLE_LINK_AUTHENTICATING,
    FEB_BLE_LINK_AUTHENTICATED,
} feb_ble_link_state_t;
#endif

/* Board behavior hooks. The board defines exactly one `const` instance (lives in flash) and
   passes it to feb_app_core_init() before starting NimBLE. Every member is optional: NULL
   means the call site is a no-op. New members are appended; existing ones never change
   shape. */
typedef struct {
    /* write_complete()'s TX_DONE_BOARD_EXT continuation (a board-owned chained send). */
    void (*on_tx_done_ext)(uint16_t conn_handle);
    /* Last edit to every wifi_scan_config_t right before esp_wifi_scan_start(): manual
       wifi_scan, wardriving start, and wardriving re-arm (C5: dual-band channel_bitmap). */
    void (*wifi_scan_cfg_ext)(wifi_scan_config_t *scan_cfg);
#if FEB_WIFI_DUAL_BAND
    /* Wardriving start, Wi-Fi source, after the country code is applied (C5: band mode).
       Not wired on single-band boards; the C5 switchover adds the call site together with
       the `wifi_band` parse branch in feb_cap_wardriving_cmd.c. */
    void (*wardriving_start_ext)(uint8_t wifi_band);
#endif
#if FEB_HAS_LINK_HOOKS
    /* BLE link-lifecycle hooks. Compiled out entirely (members and call sites) when
       FEB_HAS_LINK_HOOKS is 0, so boards without them carry no extra code. */
    /* Every BLE link-state transition: scan start/failure, connect attempt, connect,
       runtime-authenticated, disconnect. */
    void (*on_ble_state)(feb_ble_link_state_t state);
    /* feb_start_scan() starts nothing while this returns false. */
    bool (*radio_permitted)(void);
    /* BLE_GAP_EVENT_CONNECT, after the core's per-connection wardriving TX state reset. */
    void (*on_connect)(void);
    /* BLE_GAP_EVENT_DISCONNECT, after the core's per-connection state reset and before an
       in-flight manual scan is stopped. */
    void (*on_disconnect)(void);
    /* Runtime session just authenticated, right after the wardriving backlog kick. */
    void (*on_authenticated)(uint16_t conn_handle);
#endif
} feb_app_hooks_t;

void feb_app_core_init(const feb_app_hooks_t *hooks);

extern const char *const feb_features[];
extern const size_t feb_feature_count;
#define FEB_FEATURE_COUNT feb_feature_count

/* Capability-name dispatch, defined by the board (its command table). */
void feb_handle_command(uint16_t conn_handle, const feb_command_payload_t *cmd);

#endif

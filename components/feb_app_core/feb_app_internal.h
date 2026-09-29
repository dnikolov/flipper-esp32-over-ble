#ifndef FEB_APP_INTERNAL_H
#define FEB_APP_INTERNAL_H

/* Shared state and cross-module prototypes of the feb_app_core component. Every
   symbol declared here is touched only from the NimBLE host task unless its own
   definition comment says otherwise (cross-task state stays behind accessors). */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "cbor_codec.h"
#include "framing.h"
#include "location.h"
#include "pairing.h"
#include "pairing_crypto.h"
#include "session.h"
#include "session_crypto.h"
#include "status_led.h"
#include "wardriving_log.h"
#include "wardriving_record_format.h"
#include "wardriving_validate.h"
#include "wardriving_dedup.h"
#include "wardriving_persist.h"
#include "feb_app_core.h"

static const char *TAG = FEB_LOG_TAG;
#define FEB_REASSEMBLY_CHECK_INTERVAL_MS (FEB_REASSEMBLY_TIMEOUT_MS / 2)

/* docs/PLAN.md "Wi-Fi scan capability" step. Self-imposed static-buffer bound for the raw
   esp_wifi_scan_get_ap_records() fetch -- distinct from docs/PROTOCOL.md's 32-AP *wire* cap
   (FEB_WIFI_SCAN_MAX_APS_PER_RECORD in cbor_codec.h). A larger raw-fetch buffer than the
   wire cap is needed to honor PROTOCOL.md's "report the 32 strongest by RSSI" rule
   correctly: ESP-IDF does not guarantee scan results are RSSI-ordered, so with a raw buffer
   sized exactly to the wire cap there would be no way to tell whether the first 32 returned
   are actually the 32 strongest of everything the radio found. 64 is a pragmatic bound (real
   environments essentially never return more distinct BSSIDs than this in one scan); if the
   radio ever does find more than 64, only the first 64 as returned by the driver (in
   undefined order) are candidates for the top-32 selection -- an accepted, documented
   limitation rather than unbounded/dynamic allocation from radio-reported data. */
#define FEB_WIFI_SCAN_RAW_MAX 64u

typedef enum {
    PAIRING_STATE_IDLE = 0,
    PAIRING_STATE_INIT_SENT,
    PAIRING_STATE_CONFIRM_SENT,
    PAIRING_STATE_COMPLETE_SENT,
} pairing_state_t;

typedef enum {
    TX_DONE_NONE = 0,
    TX_DONE_AWAIT_PAIR_REPLY,
    TX_DONE_SEND_PAIR_COMPLETE,
    TX_DONE_DISCONNECT,
    TX_DONE_AWAIT_HELLO_ACK,
    TX_DONE_RUNTIME_AUTHENTICATED,
    TX_DONE_CONTINUE_WIFI_SCAN, /* another wifi_scan status batch is queued behind this one */
    TX_DONE_CONTINUE_BLE_SCAN,  /* another ble_scan status batch is queued behind this one */
    TX_DONE_CONTINUE_WARDRIVING, /* feb_wardriving_send_next_batch()'s own re-entry point -- always
                                     used (not just when a follow-up batch is already known to
                                     exist) so full delivery of the just-sent batch is confirmed
                                     before it's marked drained; see feb_wardriving_send_next_batch()
                                     and feb_wardriving_pending_drain_count's comments. */
    TX_DONE_SEND_WARDRIVING_STOPPED, /* feb_wardriving_self_stop()'s error record just went out;
                                         send the accompanying status(state="stopped") next */
    TX_DONE_BOARD_EXT, /* routed to feb_app_hooks->on_tx_done_ext(); unused on C6 */
} tx_done_action_t;

/* docs/PLAN.md step 6: reset no longer unconditionally opens a pairing window. A stored
   pairing_secret found at boot means runtime auth is attempted first; the board only falls
   back to FEB_BOOT_MODE_PAIRING if the Flipper rejects it with unknown_board (see
   BLE_GAP_EVENT_DISCONNECT below), not on this same connection. */
typedef enum {
    FEB_BOOT_MODE_PAIRING = 0,
    FEB_BOOT_MODE_RUNTIME_AUTH,
} feb_boot_mode_t;

typedef enum {
    RUNTIME_AUTH_STATE_IDLE = 0,
    RUNTIME_AUTH_STATE_HELLO_SENT,
    RUNTIME_AUTH_STATE_AUTHENTICATED,
} runtime_auth_state_t;

/* Set by feb_fail_runtime_auth()/feb_handle_runtime_auth_unknown_board() just before
   ble_gap_terminate(), read (and reset to NORMAL) by BLE_GAP_EVENT_DISCONNECT to decide
   which of the three docs/PLAN.md step 6 outcomes applies. */
typedef enum {
    DISCONNECT_REASON_NORMAL = 0,
    DISCONNECT_REASON_UNKNOWN_BOARD,
    DISCONNECT_REASON_AUTH_FAILED,
} disconnect_reason_t;

/* docs/PROTOCOL.md: "must never wrap. A new BLE session is required before 2^24 - 1
   protected records are sent." feb_session_build_nonce() truncates sequence to its low
   24 bits and does not itself enforce this cap (session.h) -- both directions of a single
   session must stay strictly below this value or the AES-GCM nonce repeats. */
#define FEB_SESSION_SEQUENCE_MAX 0xFFFFFFu

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": wardriving's Wi-Fi/BLE
   capture sources deliberately reuse the manual wifi_scan/ble_scan capabilities' own
   feb_wifi_scan_in_progress/feb_ble_scan_in_progress busy flags and done-callout plumbing below,
   rather than a second parallel set of state -- this is what makes the bidirectional busy
   guard (docs/PROTOCOL.md: manual scan rejected busy while wardriving's matching source is
   active, and vice versa) hold by construction instead of needing a separate cross-check:
   there is exactly one "who currently owns the Wi-Fi radio scan" flag and one "who currently
   owns the NimBLE discovery state machine" flag, each shared by both callers. Each *_active_source
   selects which of the two callers' completion behavior (feb_wifi_scan_done_cb()/
   feb_ble_scan_window_close_cb()) runs when a pass finishes. */
typedef enum {
    WIFI_SCAN_SOURCE_MANUAL = 0,
    WIFI_SCAN_SOURCE_WARDRIVING,
} wifi_scan_source_t;

typedef enum {
    BLE_SCAN_SOURCE_MANUAL = 0,
    BLE_SCAN_SOURCE_WARDRIVING,
} ble_scan_source_t;

/* WiFi per-channel scan dwell-time ("swelling") control and regulatory country code
   (docs/WARDRIVING_REDESIGN.md, added 2026-09-21) -- set once at feb_wardriving_start_internal()
   and read back by feb_wardriving_wifi_interval_cb() on every re-arm. */
typedef enum {
    WARDRIVING_SWELLING_NORMAL = 0,
    WARDRIVING_SWELLING_AGGRESSIVE = 1,
    WARDRIVING_SWELLING_SPEED_BASED = 2,
} wardriving_swelling_mode_t;

typedef enum {
    WARDRIVING_COUNTRY_ROW = 0,
    WARDRIVING_COUNTRY_BG = 1,
} wardriving_country_t;

extern uint8_t feb_own_addr_type;
extern uint16_t feb_connection_handle;
extern uint16_t feb_write_value_handle;
extern uint16_t feb_negotiated_att_mtu;
extern struct ble_npl_callout feb_reassembly_timeout_co;
extern struct ble_npl_callout feb_reconnect_co;
extern char feb_board_id_buf[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
extern size_t feb_board_id_len;
extern uint8_t feb_pairing_epoch[FEB_PAIRING_EPOCH_LEN];
extern uint32_t feb_pairing_window_start_ms;
extern bool feb_pairing_window_closed;
extern pairing_state_t feb_pairing_state;
extern tx_done_action_t feb_tx_done_action;
extern feb_boot_mode_t feb_boot_mode;
extern uint8_t feb_stored_pairing_secret[FEB_PAIRING_SECRET_LEN];
extern runtime_auth_state_t feb_runtime_auth_state;
extern disconnect_reason_t feb_pending_disconnect_reason;
extern uint8_t feb_rt_session_id[FEB_SESSION_ID_LEN];
extern uint8_t feb_rt_session_key[FEB_SESSION_KEY_LEN];
extern uint8_t feb_runtime_auth_failure_count;
extern uint32_t feb_hello_ack_start_ms;
extern uint32_t feb_pair_reply_wait_start_ms;
extern uint64_t feb_rt_tx_sequence;
extern wifi_scan_source_t feb_wifi_scan_active_source;
extern ble_scan_source_t feb_ble_scan_active_source;
extern bool feb_wardriving_wifi_active;
extern bool feb_wardriving_ble_active;
extern uint32_t feb_wardriving_wifi_interval_ms;
extern uint32_t feb_wardriving_ble_window_ms;
extern uint32_t feb_wardriving_ble_interval_ms;
extern wardriving_flush_window_t feb_wardriving_flush_window;
extern uint16_t feb_wardriving_flush_last_appended;
extern uint8_t feb_wardriving_flush_ble_windows;
extern struct ble_npl_callout feb_wardriving_wifi_interval_co;
extern struct ble_npl_callout feb_wardriving_ble_interval_co;
extern feb_wardriving_persisted_state_t feb_wardriving_persisted;
extern bool feb_wardriving_tx_in_flight;
extern size_t feb_wardriving_pending_drain_count;
extern volatile bool feb_wifi_scan_in_progress;
extern struct ble_npl_callout feb_wifi_scan_done_co;
extern uint16_t feb_ble_scan_raw_count;
extern volatile bool feb_ble_scan_in_progress;
extern struct ble_npl_callout feb_ble_scan_done_co;
extern uint8_t feb_pairing_payload_encode_buf[FEB_CBOR_MAX_PAYLOAD];
extern uint8_t feb_tx_fragment_total;
extern uint8_t feb_tx_fragment_next;
extern uint8_t feb_pending_protected_tx_head;
extern uint8_t feb_pending_protected_tx_count;
extern bool feb_tx_dispatching_completion;
extern uint64_t feb_wardriving_last_reported_backlog;
extern const feb_app_hooks_t *feb_app_hooks;


int feb_gap_event(struct ble_gap_event *event, void *arg);
bool feb_ble_addr_is_paired_peer(const uint8_t addr[FEB_BLE_SCAN_ADDRESS_LEN]);
int feb_write_complete(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg);
void feb_begin_pairing(uint16_t conn_handle);
void feb_handle_pair_reply(uint16_t conn_handle, const feb_pairing_envelope_t *envelope);
void feb_finish_pairing_after_confirm(uint16_t conn_handle);
void feb_fail_pairing_ceremony(uint16_t conn_handle, feb_pairing_error_t err);
void feb_begin_runtime_auth(uint16_t conn_handle);
void feb_handle_hello_ack(uint16_t conn_handle, const feb_unencrypted_record_t *envelope);
void feb_fail_runtime_auth(uint16_t conn_handle);
void feb_handle_runtime_auth_unknown_board(uint16_t conn_handle);
bool feb_connecting_permitted(void);
void feb_handle_capability_query(uint16_t conn_handle);
void feb_handle_wifi_scan_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
void feb_handle_ble_scan_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
void feb_handle_gps_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
void feb_wifi_scan_send_next_batch(uint16_t conn_handle);
void feb_wifi_scan_done_cb(struct ble_npl_event *ev);
void feb_wifi_scan_done_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void feb_ble_scan_catalog_advertisement(const struct ble_gap_disc_desc *disc);
void feb_ble_scan_send_next_batch(uint16_t conn_handle);
void feb_ble_scan_window_close_cb(struct ble_npl_event *ev);
void feb_handle_wardriving_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
void feb_wardriving_send_next_batch(uint16_t conn_handle);
void feb_wardriving_maybe_kick_send(uint16_t conn_handle);
void feb_wardriving_self_stop(const char *error_code);
void feb_wardriving_wifi_interval_cb(struct ble_npl_event *ev);
void feb_wardriving_ble_interval_cb(struct ble_npl_event *ev);
bool feb_wardriving_start_internal(bool want_wifi, bool want_ble, bool want_ble_passive,
                                      uint32_t wifi_interval_ms, uint32_t ble_window_ms,
                                      uint32_t ble_interval_ms,
                                      wardriving_swelling_mode_t wifi_swelling,
                                      wardriving_country_t country
#if FEB_WIFI_DUAL_BAND
                                      , uint8_t wifi_band
#endif
                                      );
void feb_wardriving_stop_internal(void);
void feb_compute_board_id(void);
bool feb_load_pairing_secret(uint8_t out[FEB_PAIRING_SECRET_LEN]);
void feb_pairing_attempt_zeroize(void);
void feb_runtime_auth_zeroize(void);
void feb_wipe_pairing_secrets(void);
uint32_t feb_runtime_auth_backoff_delay_ms(void);
void feb_start_scan(void);
void feb_reconnect_timer_cb(struct ble_npl_event *ev);
void feb_send_next_tx_fragment(uint16_t conn_handle);
bool feb_start_next_pending_protected_send(void);
bool feb_queue_and_send_protected(uint16_t conn_handle, const char *type, size_t type_len,
                                     const uint8_t *payload, size_t payload_len,
                                     tx_done_action_t next_action);
bool feb_send_protected(uint16_t conn_handle, const char *type, size_t type_len,
                           const uint8_t *payload, size_t payload_len);
bool feb_send_protected_error(uint16_t conn_handle, const char *code, size_t code_len,
                                 int has_request_id, uint64_t request_id);
void feb_reassembly_timeout_cb(struct ble_npl_event *ev);

#if FEB_HAS_LINK_HOOKS
bool feb_pairing_window_is_open(void);

static inline void feb_hook_ble_state(feb_ble_link_state_t state)
{
    if (feb_app_hooks->on_ble_state != NULL) {
        feb_app_hooks->on_ble_state(state);
    }
}
#endif

#if FEB_HAS_CLUSTER_WORKER
/* Phase 9 cluster worker (docs/CLUSTER.md) as an alternative Wi-Fi scan source for both the
   manual wifi_scan capability and wardriving's Wi-Fi source. The board's cluster glue appends
   worker scan_result frames into feb_wifi_scan_raw_records[]/feb_wifi_scan_raw_count from its
   own UART RX task, only ever under its own spinlock; every other access is on the NimBLE host
   task (the manual path hands off on scan_batch_done, the delegated-wardriving path snapshots
   under that same spinlock via feb_cluster_flush_snapshot()). */
extern wifi_ap_record_t feb_wifi_scan_raw_records[FEB_WIFI_SCAN_RAW_MAX];
extern uint16_t feb_wifi_scan_raw_count;
extern bool feb_wardriving_wifi_delegated;
void feb_wifi_scan_select_top32(uint16_t raw_count);
void feb_wifi_scan_start_local(uint16_t conn_handle);

/* Board-provided (cluster glue). */
bool feb_cluster_worker_is_present(void);
/* Manual wifi_scan: arms a worker-sourced scan if a worker is present; false = scan locally. */
bool feb_cluster_wifi_scan_delegate(uint64_t request_id);
/* Disconnect: cancels a worker-sourced manual scan still being collected; false = none was. */
bool feb_cluster_wifi_scan_cancel(void);
/* Delegated wardriving start/stop: installs/releases the flush buffer and sets the worker's
   scan mode. */
void feb_cluster_wardriving_begin(wifi_ap_record_t *flush_buf, uint8_t dwell_mode, uint16_t interval_ms);
void feb_cluster_wardriving_end(void);
/* Delegated wardriving flush (feb_wifi_scan_done_cb()): snapshot/release of the flush buffer.
   Returns true when the buffer is held and feb_cluster_flush_finish() must follow. */
bool feb_cluster_flush_snapshot(wifi_ap_record_t **raw, uint16_t *raw_count);
void feb_cluster_flush_finish(wifi_ap_record_t *raw);
#endif

#endif

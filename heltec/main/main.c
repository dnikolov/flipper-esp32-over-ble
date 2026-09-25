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
#include "factory_reset.h"
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

/* docs/PLAN.md Phase 4 step 3: base BLE transport/pairing/session-auth layer ported from
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
   combo radio, not a borrowed guarantee -- see docs/BASELINES.md's Heltec entry and this
   file's own wardriving_start_internal()/wardriving_wifi_interval_cb() comments). All five
   wardriving_*.c/h support files (record format, validation, dedup, raw-flash circular log,
   persisted settings) are copied unchanged from esp32/main/ -- they are board-agnostic engine
   code with no pin/radio-specific logic of their own; see heltec/partitions.csv for this
   board's own "wardrive" data partition (sized for its 8MB flash, not the C6's 4MB). The
   NVS-persisted per-run settings (WiFi swelling, country, sources, intervals) and the GPS
   fix-dependency record-level discard (docs/PLAN.md's "Real GPS driver..." section) are wired
   identically to the C6. feb_features[] and handle_command()'s dispatch now reflect
   wifi_scan/ble_scan/gps/wardriving. */

static const char *TAG = "flipper_heltec_over_ble";

#define MAX_RECONNECT_RETRIES 5
#define FEB_RX_FRAGMENT_BUFFER_SIZE 256u
/* Same derivation as esp32/main/main.c: ceil(FEB_MAX_RECORD_SIZE /
   feb_fragment_capacity(FEB_FLIPPER_WRITE_EFFECTIVE_MTU)) = ceil(768/60) = 13. */
#define FEB_TX_MAX_FRAGMENTS 13u
#define FEB_REASSEMBLY_CHECK_INTERVAL_MS (FEB_REASSEMBLY_TIMEOUT_MS / 2)
#define FEB_SCAN_SUMMARY_INTERVAL_MS 10000u
#define FEB_PAIRING_NVS_NAMESPACE "feb_pairing"
#define FEB_PAIRING_NVS_KEY "secret"

#define FEB_HELLO_ACK_TIMEOUT_MS 5000u
#define FEB_PAIR_REPLY_TIMEOUT_MS 5000u
#define FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP 6u
#define FEB_RUNTIME_AUTH_SLOW_CADENCE_MS (5u * 60u * 1000u)
#define FEB_RECONNECT_SLOW_CADENCE_MS (30u * 1000u)
#define FEB_IDLE_TIMEOUT_MS 30000u

/* Same fixed 64-byte Flipper write-characteristic cap as the C6 build (flipper/
   flipper_esp32_over_ble.c PAYLOAD_MAX) -- a shared-contract fact, not board-specific; see
   esp32/main/main.c's fuller comment on FEB_FLIPPER_WRITE_EFFECTIVE_MTU. */
#define FEB_FLIPPER_WRITE_CHAR_MAX_LEN 64u
#define FEB_FLIPPER_WRITE_EFFECTIVE_MTU (FEB_FLIPPER_WRITE_CHAR_MAX_LEN + FEB_ATT_WRITE_OVERHEAD)

/* docs/PLAN.md step 7 naming convention, this board's own hand-maintained constants. */
#define FEB_BOARD_MODEL "heltec-wifi-lora-32-v2"
#define FEB_FIRMWARE_VERSION "0.1.0"

/* `wifi_scan`/`ble_scan` capability constants, ported from esp32/main/main.c unchanged --
   see that file's fuller comments (same rationale applies verbatim; this board's
   esp_wifi_scan_get_ap_records()/NimBLE discovery APIs are identical to the C6's). */
#define FEB_WIFI_SCAN_RAW_MAX 64u
#define FEB_WIFI_SCAN_STATUS_ENCODE_HEADROOM 32u
#define FEB_BLE_SCAN_RAW_MAX 64u
#define FEB_BLE_SCAN_WINDOW_MS 10000u
#define FEB_BLE_SCAN_STATUS_ENCODE_HEADROOM 32u

/* `wardriving` capability constants, ported unchanged from esp32/main/main.c -- see that
   file's fuller comments (interval bounds/defaults live in wardriving_validate.h, shared
   unchanged via the wardriving_*.c/h files copied into this board's main/ directory). */
#define FEB_WARDRIVING_STATUS_ENCODE_HEADROOM 32u
#define FEB_WARDRIVING_PEEK_SCRATCH_LEN (FEB_WARDRIVING_MAX_RECORDS_PER_BATCH * WD_RECORD_MAX_PAYLOAD)
#define FEB_WARDRIVING_FLASH_FAILURE_LIMIT 3u

static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
    0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x11);
static const uint8_t advertised_service_uuid[] = {
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
    0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x11};
static const ble_uuid128_t write_uuid = BLE_UUID128_INIT(
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
    0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x12);
static const ble_uuid128_t notify_uuid = BLE_UUID128_INIT(
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
    0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x13);

static uint8_t own_addr_type;
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t service_start_handle;
static uint16_t service_end_handle;
static uint16_t write_value_handle;
static uint16_t notify_value_handle;
static uint16_t notify_cccd_handle;
static uint16_t negotiated_att_mtu = 23;
static uint8_t reconnect_retries;
static bool reconnect_task_active;
static uint32_t scan_report_window_count;
static uint32_t scan_report_lifetime_total;
static uint32_t scan_summary_elapsed_ms;

static feb_reassembly_t rx_reassembly;
static struct ble_npl_callout reassembly_timeout_co;
static struct ble_npl_callout reconnect_co;

static char board_id_buf[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
static size_t board_id_len;
static uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN];
static uint32_t pairing_window_start_ms;
static bool pairing_window_closed;

typedef enum {
    PAIRING_STATE_IDLE = 0,
    PAIRING_STATE_INIT_SENT,
    PAIRING_STATE_CONFIRM_SENT,
    PAIRING_STATE_COMPLETE_SENT,
} pairing_state_t;
static pairing_state_t pairing_state = PAIRING_STATE_IDLE;

typedef enum {
    TX_DONE_NONE = 0,
    TX_DONE_AWAIT_PAIR_REPLY,
    TX_DONE_SEND_PAIR_COMPLETE,
    TX_DONE_DISCONNECT,
    TX_DONE_AWAIT_HELLO_ACK,
    TX_DONE_RUNTIME_AUTHENTICATED,
    TX_DONE_CONTINUE_WIFI_SCAN, /* another wifi_scan status batch is queued behind this one */
    TX_DONE_CONTINUE_BLE_SCAN,  /* another ble_scan status batch is queued behind this one */
    TX_DONE_CONTINUE_WARDRIVING, /* wardriving_send_next_batch()'s own re-entry point, ported
                                     unchanged from esp32/main/main.c -- see that file's fuller
                                     comment. */
    TX_DONE_SEND_WARDRIVING_STOPPED, /* wardriving_self_stop()'s error record just went out;
                                         send the accompanying status(state="stopped") next */
} tx_done_action_t;
static tx_done_action_t tx_done_action = TX_DONE_NONE;

typedef enum {
    FEB_BOOT_MODE_PAIRING = 0,
    FEB_BOOT_MODE_RUNTIME_AUTH,
} feb_boot_mode_t;
static feb_boot_mode_t boot_mode = FEB_BOOT_MODE_PAIRING;
static uint8_t stored_pairing_secret[FEB_PAIRING_SECRET_LEN];

typedef enum {
    RUNTIME_AUTH_STATE_IDLE = 0,
    RUNTIME_AUTH_STATE_HELLO_SENT,
    RUNTIME_AUTH_STATE_AUTHENTICATED,
} runtime_auth_state_t;
static runtime_auth_state_t runtime_auth_state = RUNTIME_AUTH_STATE_IDLE;

typedef enum {
    DISCONNECT_REASON_NORMAL = 0,
    DISCONNECT_REASON_UNKNOWN_BOARD,
    DISCONNECT_REASON_AUTH_FAILED,
} disconnect_reason_t;
static disconnect_reason_t pending_disconnect_reason = DISCONNECT_REASON_NORMAL;

static uint8_t rt_session_id[FEB_SESSION_ID_LEN];
static uint8_t rt_client_nonce[FEB_SESSION_NONCE_FIELD_LEN];
static uint8_t rt_device_nonce[FEB_SESSION_NONCE_FIELD_LEN];
static uint8_t rt_session_key[FEB_SESSION_KEY_LEN];
static uint8_t rt_transcript_buf[FEB_SESSION_MAX_TRANSCRIPT_LEN];
static size_t rt_transcript_len;
static uint8_t runtime_auth_failure_count;
static uint32_t hello_ack_start_ms;
static uint32_t pair_reply_wait_start_ms;
static uint32_t last_record_activity_ms;

static uint64_t rt_tx_sequence;
static uint64_t rt_rx_sequence;
static uint8_t rt_plaintext_buf[FEB_CBOR_MAX_PAYLOAD];
static uint8_t rt_ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];

#define FEB_SESSION_SEQUENCE_MAX 0xFFFFFFu
#define FEB_TX_PENDING_PROTECTED_COUNT 4u
#define FEB_TX_PENDING_TYPE_MAX 32u

static const char *const feb_features[] = {"wifi_scan", "ble_scan", "gps", "wardriving"};
#define FEB_FEATURE_COUNT (sizeof(feb_features) / sizeof(feb_features[0]))

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": wardriving's Wi-Fi/BLE
   capture sources deliberately reuse the manual wifi_scan/ble_scan capabilities' own
   wifi_scan_in_progress/ble_scan_in_progress busy flags and done-callout plumbing below,
   rather than a second parallel set of state -- ported unchanged from esp32/main/main.c, see
   that file's fuller comment. */
typedef enum {
    WIFI_SCAN_SOURCE_MANUAL = 0,
    WIFI_SCAN_SOURCE_WARDRIVING,
} wifi_scan_source_t;
static wifi_scan_source_t wifi_scan_active_source;

typedef enum {
    BLE_SCAN_SOURCE_MANUAL = 0,
    BLE_SCAN_SOURCE_WARDRIVING,
} ble_scan_source_t;
static ble_scan_source_t ble_scan_active_source;

/* wardriving's own start/stop state and configured cadence, independent of BLE connection
   state (docs/PROTOCOL.md: "continues across BLE disconnects, buffering results to an
   on-device flash log"). Ported unchanged from esp32/main/main.c -- see that file's fuller
   comments on each of these. */
static bool wardriving_wifi_active;
static bool wardriving_ble_active;
static bool wardriving_ble_passive;
static uint32_t wardriving_wifi_interval_ms;
static uint32_t wardriving_ble_window_ms;
static uint32_t wardriving_ble_interval_ms;

typedef enum {
    WARDRIVING_SWELLING_NORMAL = 0,
    WARDRIVING_SWELLING_AGGRESSIVE = 1,
    WARDRIVING_SWELLING_SPEED_BASED = 2,
} wardriving_swelling_mode_t;

typedef enum {
    WARDRIVING_COUNTRY_ROW = 0,
    WARDRIVING_COUNTRY_BG = 1,
} wardriving_country_t;

static wardriving_swelling_mode_t wardriving_wifi_swelling;
static bool wardriving_swelling_aggressive_active;
static struct ble_npl_callout wardriving_wifi_interval_co;
static struct ble_npl_callout wardriving_ble_interval_co;
static feb_wardriving_persisted_state_t wardriving_persisted;
/* A plain ble_npl_event, not a ble_npl_callout -- see esp32/main/main.c's fuller comment on
   why (ESP-IDF's NimBLE FreeRTOS port caps the host-side callout pool at a hard-coded 8
   slots, already close to exhausted by this file's other callouts once wardriving_wifi/
   ble_interval_co are added). */
static struct ble_npl_event wardriving_button_toggle_ev;
static volatile bool wardriving_control_ready;
static volatile uint32_t wardriving_button_toggle_pending;
static bool wardriving_autostart_attempted;
static bool wardriving_tx_in_flight;
static size_t wardriving_pending_drain_count;
static uint8_t wardriving_flash_failure_count;
static bool wardriving_wifi_self_stop_pending;
static bool wardriving_ble_self_stop_pending;

/* `wifi_scan` capture state, ported unchanged from esp32/main/main.c. wifi_scan_done_handler()
   runs on the sys_evt task (esp_wifi's scan-done event); wifi_scan_done_cb() runs on the
   NimBLE host task via wifi_scan_done_co, the same cross-task handoff reassembly_timeout_co
   already uses, since sending touches connection_handle/tx_fragment_* state owned by that
   task. wifi_scan_raw_count carries the wardriving-source dedup/append loop's input count
   across that same handoff (see wifi_scan_done_handler()'s comment; G30 fix in
   docs/BACKLOG.md). */
static wifi_ap_record_t wifi_scan_raw_records[FEB_WIFI_SCAN_RAW_MAX];
static uint16_t wifi_scan_raw_count;
static feb_wifi_scan_ap_t wifi_scan_selected[FEB_WIFI_SCAN_MAX_APS_PER_RECORD];
static uint16_t wifi_scan_found_count;
static uint16_t wifi_scan_send_next_index;
static volatile bool wifi_scan_in_progress;
static uint64_t wifi_scan_request_id;
static struct ble_npl_callout wifi_scan_done_co;

/* `ble_scan` per-window BLE device catalog, ported unchanged from esp32/main/main.c.
   Written only from gap_event()'s BLE_GAP_EVENT_DISC case (NimBLE host task) while
   ble_scan_in_progress is set; read only from ble_scan_window_close_cb()/
   ble_scan_send_next_batch() -- also the NimBLE host task, via ble_scan_done_co. No
   cross-task handoff is needed here (unlike wifi_scan): BLE discovery events already
   arrive on the NimBLE host task. */
typedef struct {
    uint8_t addr[FEB_BLE_SCAN_ADDRESS_LEN];
    uint8_t addr_type;
    int8_t rssi;
    char name[FEB_BLE_SCAN_NAME_MAX_LEN];
    size_t name_len;
    bool has_name;
} ble_scan_raw_device_t;
static ble_scan_raw_device_t ble_scan_raw_devices[FEB_BLE_SCAN_RAW_MAX];
static uint16_t ble_scan_raw_count;
static feb_ble_scan_device_t ble_scan_selected[FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD];
static uint16_t ble_scan_found_count;
static uint16_t ble_scan_send_next_index;
static volatile bool ble_scan_in_progress;
static uint64_t ble_scan_request_id;
static struct ble_npl_callout ble_scan_done_co;

static uint8_t esp32_private_key[FEB_X25519_KEY_LEN];
static uint8_t esp32_public_key[FEB_X25519_KEY_LEN];
static uint8_t device_nonce[FEB_PAIRING_NONCE_LEN];
static uint8_t client_nonce[FEB_PAIRING_NONCE_LEN];
static uint8_t k_shared[FEB_PAIRING_KSHARED_LEN];
static uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN];
static uint8_t pairing_transcript_buf[FEB_PAIRING_MAX_TRANSCRIPT_LEN];
static size_t pairing_transcript_len;

static uint8_t tx_message_id;
static uint8_t pairing_payload_encode_buf[FEB_CBOR_MAX_PAYLOAD];
static uint8_t pairing_record_encode_buf[FEB_MAX_RECORD_SIZE];
static uint8_t tx_fragment_buf[FEB_MAX_RECORD_SIZE + FEB_TX_MAX_FRAGMENTS * FEB_FRAG_HEADER_SIZE];
static size_t tx_fragment_offsets[FEB_TX_MAX_FRAGMENTS];
static size_t tx_fragment_lens[FEB_TX_MAX_FRAGMENTS];
static size_t tx_fragment_write_pos;
static uint8_t tx_fragment_total;
static uint8_t tx_fragment_next;
typedef struct {
    uint16_t conn_handle;
    char type[FEB_TX_PENDING_TYPE_MAX];
    size_t type_len;
    uint8_t payload[FEB_CBOR_MAX_PAYLOAD];
    size_t payload_len;
    tx_done_action_t next_action;
} pending_protected_tx_t;
static pending_protected_tx_t pending_protected_tx[FEB_TX_PENDING_PROTECTED_COUNT];
static uint8_t pending_protected_tx_head;
static uint8_t pending_protected_tx_count;
static bool tx_dispatching_completion;

static int gap_event(struct ble_gap_event *event, void *arg);
static void nimble_host_task(void *arg);
static int service_discovered(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg);
static int characteristic_discovered(uint16_t conn_handle,
                                      const struct ble_gatt_error *error,
                                      const struct ble_gatt_chr *characteristic,
                                      void *arg);
static int descriptor_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 uint16_t characteristic_handle,
                                 const struct ble_gatt_dsc *descriptor,
                                 void *arg);
static int write_complete(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg);
static void begin_pairing(uint16_t conn_handle);
static void handle_pair_reply(uint16_t conn_handle, const feb_pairing_envelope_t *envelope);
static void finish_pairing_after_confirm(uint16_t conn_handle);
static void fail_pairing_ceremony(uint16_t conn_handle, feb_pairing_error_t err);
static void begin_runtime_auth(uint16_t conn_handle);
static void handle_hello_ack(uint16_t conn_handle, const feb_unencrypted_record_t *envelope);
static void fail_runtime_auth(uint16_t conn_handle);
static void handle_runtime_auth_unknown_board(uint16_t conn_handle);
static void schedule_runtime_auth_backoff(void);
static bool connecting_permitted(void);
static void handle_capability_query(uint16_t conn_handle);
static void handle_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
static void handle_wifi_scan_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
static void handle_ble_scan_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
static void handle_gps_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
static void wifi_scan_send_next_batch(uint16_t conn_handle);
static void wifi_scan_done_cb(struct ble_npl_event *ev);
static void wifi_scan_done_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static void ble_scan_catalog_advertisement(const struct ble_gap_disc_desc *disc);
static void ble_scan_send_next_batch(uint16_t conn_handle);
static void ble_scan_window_close_cb(struct ble_npl_event *ev);
static void start_wifi_subsystem(void);
static void handle_wardriving_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
static void wardriving_send_next_batch(uint16_t conn_handle);
static void wardriving_maybe_kick_send(uint16_t conn_handle);
static void wardriving_self_stop(const char *error_code);
static void wardriving_apply_wifi_swelling(wifi_scan_config_t *scan_cfg);
static void wardriving_wifi_interval_cb(struct ble_npl_event *ev);
static void wardriving_ble_interval_cb(struct ble_npl_event *ev);
static bool wardriving_start_internal(bool want_wifi, bool want_ble, bool want_ble_passive,
                                      uint32_t wifi_interval_ms, uint32_t ble_window_ms,
                                      uint32_t ble_interval_ms,
                                      wardriving_swelling_mode_t wifi_swelling,
                                      wardriving_country_t country);
static void wardriving_stop_internal(void);
static void wardriving_button_toggle_cb(struct ble_npl_event *ev);

static void compute_board_id(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    int written;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read factory MAC: %s", esp_err_to_name(err));
        memcpy(board_id_buf, "heltec-unknown", sizeof("heltec-unknown"));
        board_id_len = strlen(board_id_buf);
        return;
    }
    written = snprintf(board_id_buf, sizeof(board_id_buf), "heltec-%02x%02x%02x%02x%02x%02x",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (written < 0) {
        ESP_LOGE(TAG, "board_id snprintf failed");
        return;
    }
    board_id_len = (size_t)written < sizeof(board_id_buf) - 1 ? (size_t)written
                                                               : sizeof(board_id_buf) - 1;
}

static bool persist_pairing_secret(const uint8_t secret[FEB_PAIRING_SECRET_LEN])
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FEB_PAIRING_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, FEB_PAIRING_NVS_KEY, secret, FEB_PAIRING_SECRET_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist pairing_secret: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool load_pairing_secret(uint8_t out[FEB_PAIRING_SECRET_LEN])
{
    nvs_handle_t handle;
    esp_err_t err;
    size_t len = FEB_PAIRING_SECRET_LEN;

    err = nvs_open(FEB_PAIRING_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_get_blob(handle, FEB_PAIRING_NVS_KEY, out, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != FEB_PAIRING_SECRET_LEN) {
        return false;
    }
    return true;
}

static bool pairing_window_is_open(void)
{
    uint32_t now_ms;

    if (pairing_window_closed) {
        return false;
    }
    now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(now_ms - pairing_window_start_ms) >= FEB_PAIRING_WINDOW_MS) {
        pairing_window_closed = true;
        ESP_LOGW(TAG, "pairing window expired");
        return false;
    }
    return true;
}

static void pairing_attempt_zeroize(void)
{
    feb_secure_zero(esp32_private_key, sizeof(esp32_private_key));
    feb_secure_zero(k_shared, sizeof(k_shared));
    feb_secure_zero(k_confirm, sizeof(k_confirm));
}

static void runtime_auth_zeroize(void)
{
    feb_secure_zero(rt_session_key, sizeof(rt_session_key));
}

/* Called from factory_reset.c's perform_factory_reset() before esp_restart(). */
void feb_wipe_pairing_secrets(void)
{
    feb_secure_zero(stored_pairing_secret, sizeof(stored_pairing_secret));
    pairing_attempt_zeroize();
    runtime_auth_zeroize();
}

static bool connecting_permitted(void)
{
    if (boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH) {
        return true;
    }
    return pairing_window_is_open();
}

static uint32_t runtime_auth_backoff_delay_ms(void)
{
    if (runtime_auth_failure_count == 0) {
        return 0;
    }
    if (runtime_auth_failure_count > FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP) {
        return FEB_RUNTIME_AUTH_SLOW_CADENCE_MS;
    }
    return 1000u << (runtime_auth_failure_count - 1u);
}

static bool uuid128_matches(const ble_uuid_any_t *uuid,
                            const ble_uuid128_t *expected)
{
    return uuid->u.type == BLE_UUID_TYPE_128 &&
           memcmp(uuid->u128.value, expected->value, sizeof(expected->value)) == 0;
}

static bool scan_record_matches(const uint8_t *data, uint8_t length)
{
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, data, length) != 0) {
        return false;
    }

    for (uint8_t index = 0; index < fields.num_uuids128; index++) {
        if (memcmp(fields.uuids128[index].value, advertised_service_uuid,
                   sizeof(advertised_service_uuid)) == 0) {
            return true;
        }
    }
    return false;
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {0};
    int rc;

    if (connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    /* docs/PLAN.md "Revised long-run reconnect policy": never run a second, dedicated
       reconnect scan while wardriving's BLE source owns discovery -- ported unchanged from
       esp32/main/main.c, see that file's fuller comment (docs/LESSONS.md 2026-09-10). */
    if (wardriving_ble_active) {
        return;
    }
    if (!connecting_permitted()) {
        ESP_LOGI(TAG, "pairing window closed; staying idle");
        return;
    }

    params.passive = 0;
    /* Controller-side dup filtering keyed on address only never expires by default -- see
       esp32/main/main.c's fuller comment (docs/SESSION_MEMORY.md scan-stall root cause).
       Filtering happens in scan_record_matches() instead. */
    params.filter_duplicates = 0;
    params.itvl = 0;
    params.window = 0;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
    }
}

static void reconnect_timer_cb(struct ble_npl_event *ev)
{
    (void)ev;
    reconnect_task_active = false;
    start_scan();
}

static uint32_t reconnect_backoff_delay_ms(void)
{
    if (reconnect_retries > MAX_RECONNECT_RETRIES) {
        return FEB_RECONNECT_SLOW_CADENCE_MS;
    }
    return 1000u << (reconnect_retries - 1u);
}

static void schedule_reconnect(void)
{
    uint32_t delay_ms;

    if (!connecting_permitted()) {
        ESP_LOGW(TAG, "pairing window closed; not scheduling reconnect");
        return;
    }
    if (reconnect_task_active) {
        return;
    }

    if (reconnect_retries < 0xFFu) {
        reconnect_retries++;
    }
    delay_ms = reconnect_backoff_delay_ms();
    reconnect_task_active = true;
    if (reconnect_retries > MAX_RECONNECT_RETRIES) {
        ESP_LOGI(TAG, "reconnect retry in %lu ms (consecutive failures=%u)",
                 (unsigned long)delay_ms, reconnect_retries);
    } else {
        ESP_LOGI(TAG, "reconnect retry %u/%u in %lu ms", reconnect_retries,
                 MAX_RECONNECT_RETRIES, (unsigned long)delay_ms);
    }
    ble_npl_callout_reset(&reconnect_co, ble_npl_time_ms_to_ticks32(delay_ms));
}

static void schedule_runtime_auth_backoff(void)
{
    uint32_t delay_ms = runtime_auth_backoff_delay_ms();

    if (reconnect_task_active) {
        return;
    }
    reconnect_task_active = true;
    ESP_LOGI(TAG, "runtime auth backoff: retry in %lu ms (consecutive failures=%u)",
             (unsigned long)delay_ms, runtime_auth_failure_count);
    ble_npl_callout_reset(&reconnect_co, ble_npl_time_ms_to_ticks32(delay_ms));
}

static int mtu_exchanged(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         uint16_t mtu, void *arg)
{
    int rc;

    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "MTU exchange unavailable: %d", error->status);
    } else {
        negotiated_att_mtu = mtu;
        ESP_LOGI(TAG, "negotiated ATT MTU: %u", mtu);
    }

    rc = ble_gattc_disc_svc_by_uuid(conn_handle, &service_uuid.u,
                                    service_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "service discovery start failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int service_discovered(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    int rc;

    if (error->status == 0 && service != NULL &&
        uuid128_matches(&service->uuid, &service_uuid)) {
        service_start_handle = service->start_handle;
        service_end_handle = service->end_handle;
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (service_start_handle == 0) {
        ESP_LOGE(TAG, "v2 service not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    rc = ble_gattc_disc_all_chrs(conn_handle, service_start_handle,
                                 service_end_handle, characteristic_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "characteristic discovery start failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int characteristic_discovered(uint16_t conn_handle,
                                      const struct ble_gatt_error *error,
                                      const struct ble_gatt_chr *characteristic,
                                      void *arg)
{
    int rc;

    if (error->status == 0 && characteristic != NULL) {
        ESP_LOGI(TAG, "characteristic: def=%u val=%u props=0x%02x type=%u",
                 characteristic->def_handle, characteristic->val_handle,
                 characteristic->properties, characteristic->uuid.u.type);
        if (uuid128_matches(&characteristic->uuid, &write_uuid)) {
            write_value_handle = characteristic->val_handle;
        } else if (uuid128_matches(&characteristic->uuid, &notify_uuid)) {
            notify_value_handle = characteristic->val_handle;
        }
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (write_value_handle == 0 || notify_value_handle == 0) {
        ESP_LOGE(TAG, "required v2 characteristics not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    rc = ble_gattc_disc_all_dscs(conn_handle, service_start_handle,
                                 service_end_handle, descriptor_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD discovery start failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int descriptor_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 uint16_t characteristic_handle,
                                 const struct ble_gatt_dsc *descriptor,
                                 void *arg)
{
    int rc;
    static const uint8_t notify_enabled[] = {0x01, 0x00};

    if (error->status == 0 && descriptor != NULL) {
        ESP_LOGI(TAG, "descriptor: handle=%u chr_val=%u type=%u uuid16=0x%04x",
                 descriptor->handle, characteristic_handle, descriptor->uuid.u.type,
                 descriptor->uuid.u.type == BLE_UUID_TYPE_16 ? descriptor->uuid.u16.value : 0);
        if (descriptor->uuid.u.type == BLE_UUID_TYPE_16 &&
            descriptor->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16) {
            notify_cccd_handle = descriptor->handle;
        }
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (notify_cccd_handle == 0) {
        ESP_LOGE(TAG, "notification CCCD not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    rc = ble_gattc_write_flat(conn_handle, notify_cccd_handle, notify_enabled,
                              sizeof(notify_enabled), write_complete, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "notification subscribe failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static void tx_emit_fragment(const uint8_t *fragment, size_t fragment_len, void *ctx)
{
    uint8_t *count = (uint8_t *)ctx;

    if (*count >= FEB_TX_MAX_FRAGMENTS || tx_fragment_write_pos + fragment_len > sizeof(tx_fragment_buf)) {
        ESP_LOGE(TAG, "tx fragment buffer overflow at index %u", *count);
        return;
    }
    memcpy(tx_fragment_buf + tx_fragment_write_pos, fragment, fragment_len);
    tx_fragment_offsets[*count] = tx_fragment_write_pos;
    tx_fragment_lens[*count] = fragment_len;
    tx_fragment_write_pos += fragment_len;
    (*count)++;
}

static void send_next_tx_fragment(uint16_t conn_handle)
{
    int rc;

    if (tx_fragment_next >= tx_fragment_total) {
        return;
    }
    rc = ble_gattc_write_flat(conn_handle, write_value_handle,
                              tx_fragment_buf + tx_fragment_offsets[tx_fragment_next],
                              tx_fragment_lens[tx_fragment_next], write_complete, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "fragment %u write failed: %d", tx_fragment_next, rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    tx_fragment_next++;
}

static bool queue_encoded_record_for_tx(size_t record_len)
{
    size_t capacity;
    uint8_t built_count = 0;

    capacity = feb_fragment_capacity(negotiated_att_mtu < FEB_FLIPPER_WRITE_EFFECTIVE_MTU ?
                                     negotiated_att_mtu : FEB_FLIPPER_WRITE_EFFECTIVE_MTU);
    if (capacity == 0) {
        ESP_LOGE(TAG, "no usable fragment capacity at MTU %u", negotiated_att_mtu);
        return false;
    }

    tx_fragment_write_pos = 0;
    tx_fragment_total = feb_fragment_record(pairing_record_encode_buf, record_len, capacity,
                                            tx_message_id, tx_emit_fragment, &built_count);
    if (tx_fragment_total == 0 || tx_fragment_total != built_count) {
        ESP_LOGE(TAG, "record fragmentation failed");
        return false;
    }
    tx_message_id++;
    tx_fragment_next = 0;
    return true;
}

static bool encode_and_queue_pairing_record(const char *type, size_t type_len,
                                            const uint8_t *payload, size_t payload_len)
{
    feb_pairing_envelope_t envelope = {0};
    size_t record_len;

    envelope.version = 2;
    envelope.type = type;
    envelope.type_len = type_len;
    envelope.board_id = board_id_buf;
    envelope.board_id_len = board_id_len;
    envelope.payload_span = payload;
    envelope.payload_span_len = payload_len;

    record_len = feb_cbor_encode_pairing_envelope(pairing_record_encode_buf,
                                                  sizeof(pairing_record_encode_buf), &envelope);
    if (record_len == 0) {
        ESP_LOGE(TAG, "pairing envelope encode failed for type %.*s", (int)type_len, type);
        return false;
    }
    return queue_encoded_record_for_tx(record_len);
}

static bool encode_and_queue_session_record(const char *type, size_t type_len,
                                            const uint8_t *payload, size_t payload_len)
{
    feb_unencrypted_record_t envelope = {0};
    size_t record_len;

    envelope.version = 2;
    envelope.type = type;
    envelope.type_len = type_len;
    memcpy(envelope.session_id, rt_session_id, FEB_SESSION_ID_LEN);
    envelope.board_id = board_id_buf;
    envelope.board_id_len = board_id_len;
    envelope.payload_span = payload;
    envelope.payload_span_len = payload_len;

    record_len = feb_cbor_encode_unencrypted(pairing_record_encode_buf,
                                             sizeof(pairing_record_encode_buf), &envelope);
    if (record_len == 0) {
        ESP_LOGE(TAG, "session envelope encode failed for type %.*s", (int)type_len, type);
        return false;
    }
    return queue_encoded_record_for_tx(record_len);
}

static bool encode_and_queue_protected_record(const char *type, size_t type_len,
                                              const uint8_t *payload, size_t payload_len,
                                              uint64_t sequence)
{
    size_t record_len = feb_session_encrypt_record(rt_session_key, 2, type, type_len,
                                                    rt_session_id, board_id_buf, board_id_len,
                                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, sequence,
                                                    payload, payload_len,
                                                    rt_ciphertext_scratch, sizeof(rt_ciphertext_scratch),
                                                    pairing_record_encode_buf, sizeof(pairing_record_encode_buf));

    if (record_len == 0) {
        ESP_LOGE(TAG, "protected envelope encode failed for type %.*s", (int)type_len, type);
        return false;
    }
    return queue_encoded_record_for_tx(record_len);
}

static bool start_protected_send(uint16_t conn_handle, const char *type, size_t type_len,
                                 const uint8_t *payload, size_t payload_len,
                                 tx_done_action_t next_action)
{
    if (rt_tx_sequence >= FEB_SESSION_SEQUENCE_MAX) {
        ESP_LOGW(TAG, "protected tx sequence at cap; closing to force a new session");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return false;
    }
    if (!encode_and_queue_protected_record(type, type_len, payload, payload_len, rt_tx_sequence)) {
        return false;
    }
    rt_tx_sequence++;
    tx_done_action = next_action;
    send_next_tx_fragment(conn_handle);
    return true;
}

static bool enqueue_protected_send(uint16_t conn_handle, const char *type, size_t type_len,
                                   const uint8_t *payload, size_t payload_len,
                                   tx_done_action_t next_action)
{
    uint8_t index;
    pending_protected_tx_t *pending;

    if (type_len >= FEB_TX_PENDING_TYPE_MAX || payload_len > FEB_CBOR_MAX_PAYLOAD ||
        pending_protected_tx_count >= FEB_TX_PENDING_PROTECTED_COUNT) {
        ESP_LOGW(TAG, "protected tx queue full or request too large");
        return false;
    }
    index = (uint8_t)((pending_protected_tx_head + pending_protected_tx_count) %
                      FEB_TX_PENDING_PROTECTED_COUNT);
    pending = &pending_protected_tx[index];
    pending->conn_handle = conn_handle;
    memcpy(pending->type, type, type_len);
    pending->type[type_len] = '\0';
    pending->type_len = type_len;
    memcpy(pending->payload, payload, payload_len);
    pending->payload_len = payload_len;
    pending->next_action = next_action;
    pending_protected_tx_count++;
    return true;
}

static bool start_next_pending_protected_send(void)
{
    pending_protected_tx_t *pending;

    if (pending_protected_tx_count == 0) {
        return true;
    }
    pending = &pending_protected_tx[pending_protected_tx_head];
    if (!start_protected_send(pending->conn_handle, pending->type, pending->type_len,
                              pending->payload, pending->payload_len, pending->next_action)) {
        return false;
    }
    pending_protected_tx_head = (uint8_t)((pending_protected_tx_head + 1) %
                                          FEB_TX_PENDING_PROTECTED_COUNT);
    pending_protected_tx_count--;
    return true;
}

static bool queue_and_send_protected(uint16_t conn_handle, const char *type, size_t type_len,
                                     const uint8_t *payload, size_t payload_len,
                                     tx_done_action_t next_action)
{
    if (tx_fragment_next < tx_fragment_total || tx_dispatching_completion ||
        pending_protected_tx_count > 0) {
        return enqueue_protected_send(conn_handle, type, type_len, payload, payload_len,
                                      next_action);
    }
    return start_protected_send(conn_handle, type, type_len, payload, payload_len, next_action);
}

static bool send_protected(uint16_t conn_handle, const char *type, size_t type_len,
                           const uint8_t *payload, size_t payload_len)
{
    return queue_and_send_protected(conn_handle, type, type_len, payload, payload_len, TX_DONE_NONE);
}

static bool send_protected_error(uint16_t conn_handle, const char *code, size_t code_len,
                                 int has_request_id, uint64_t request_id)
{
    feb_error_payload_t err = {0};
    size_t payload_len;

    err.code = code;
    err.code_len = code_len;
    err.has_request_id = has_request_id;
    err.request_id = request_id;
    payload_len = feb_cbor_encode_error_payload(pairing_payload_encode_buf,
                                                sizeof(pairing_payload_encode_buf), &err);
    if (payload_len == 0) {
        return false;
    }
    return send_protected(conn_handle, "error", strlen("error"), pairing_payload_encode_buf, payload_len);
}

static void handle_capability_query(uint16_t conn_handle)
{
    feb_capability_response_payload_t response = {0};
    size_t payload_len;
    size_t i;

    response.board = FEB_BOARD_MODEL;
    response.board_len = strlen(FEB_BOARD_MODEL);
    response.firmware = FEB_FIRMWARE_VERSION;
    response.firmware_len = strlen(FEB_FIRMWARE_VERSION);
    response.feature_count = FEB_FEATURE_COUNT;
    for (i = 0; i < FEB_FEATURE_COUNT; i++) {
        response.features[i] = feb_features[i];
        response.feature_lens[i] = strlen(feb_features[i]);
    }

    payload_len = feb_cbor_encode_capability_response_payload(pairing_payload_encode_buf,
                                                              sizeof(pairing_payload_encode_buf), &response);
    if (payload_len == 0 ||
        !send_protected(conn_handle, "capability_response", strlen("capability_response"),
                        pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build capability_response");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "sending capability_response");
}

/* docs/PROTOCOL.md "`wifi_scan` command and status payloads": highest-generation PHY string
   and full-fidelity wifi_auth_mode_t string enum. Ported unchanged from esp32/main/main.c --
   this board has no 5GHz radio either, so phy_11a/phy_11ac never observably set on a real
   scan here, matching the C6's own comment. */
static const char *wifi_scan_phy_str(const wifi_ap_record_t *rec)
{
    if (rec->phy_11ax) return "11ax";
    if (rec->phy_11n) return "11n";
    if (rec->phy_11g) return "11g";
    return "11b";
}

static const char *wifi_scan_auth_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK: return "wpa2_psk";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa_wpa2_psk";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK: return "wpa3_psk";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2_wpa3_psk";
    case WIFI_AUTH_WAPI_PSK: return "wapi_psk";
    case WIFI_AUTH_OWE: return "owe";
    case WIFI_AUTH_WPA3_ENT_192: return "wpa3_ent_192";
    case WIFI_AUTH_WPA3_EXT_PSK: return "wpa3_ext_psk";
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "wpa3_ext_psk_mixed_mode";
    case WIFI_AUTH_DPP: return "dpp";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "wpa3_enterprise";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "wpa2_wpa3_enterprise";
    case WIFI_AUTH_WPA_ENTERPRISE: return "wpa_enterprise";
    default: return "unknown";
    }
}

/* docs/PROTOCOL.md "`ble_scan` command and status payloads": ported unchanged from
   esp32/main/main.c. */
static const char *ble_scan_addr_type_str(uint8_t addr_type)
{
    switch (addr_type) {
    case BLE_ADDR_PUBLIC:
    case BLE_ADDR_PUBLIC_ID:
        return "public";
    case BLE_ADDR_RANDOM:
    case BLE_ADDR_RANDOM_ID:
    default:
        return "random";
    }
}

/* Runs on the default event loop's own task (sys_evt), never the NimBLE host task -- a
   multi-second blocking scan must not run inside the protected-record dispatch handler on
   the NimBLE host task. Fetches results, selects the FEB_WIFI_SCAN_MAX_APS_PER_RECORD (32)
   strongest by RSSI, converts them into wire-ready feb_wifi_scan_ap_t entries, then hands
   off to the NimBLE host task via wifi_scan_done_co to actually build and send status
   records, since that touches connection_handle/rt_tx_sequence/tx_fragment_* state owned by
   the NimBLE host task. Ported unchanged from esp32/main/main.c's manual-scan (non-
   wardriving) path -- this board has no wardriving source to hand raw results off to. */
static void wifi_scan_done_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint16_t total_found = 0;
    uint16_t raw_count;
    uint16_t keep;
    uint16_t k;

    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    if (esp_wifi_scan_get_ap_num(&total_found) != ESP_OK) {
        total_found = 0;
    }
    raw_count = (total_found > FEB_WIFI_SCAN_RAW_MAX) ? FEB_WIFI_SCAN_RAW_MAX : total_found;
    if (raw_count > 0 && esp_wifi_scan_get_ap_records(&raw_count, wifi_scan_raw_records) != ESP_OK) {
        raw_count = 0;
    }
    if (total_found > FEB_WIFI_SCAN_RAW_MAX) {
        ESP_LOGW(TAG, "wifi_scan found %u APs, exceeding the %u-entry raw-fetch bound; only "
                      "the first %u (driver order, not RSSI order) are candidates for the "
                      "top-%u selection", (unsigned)total_found, (unsigned)FEB_WIFI_SCAN_RAW_MAX,
                 (unsigned)FEB_WIFI_SCAN_RAW_MAX, (unsigned)FEB_WIFI_SCAN_MAX_APS_PER_RECORD);
    }

    if (wifi_scan_active_source == WIFI_SCAN_SOURCE_WARDRIVING) {
        /* G30 fix, ported unchanged from esp32/main/main.c: the wardriving dedup/append loop
           runs in wifi_scan_done_cb() on the NimBLE host task instead -- the same task that
           reads this log via wardriving_send_next_batch() -- so the two can never interleave.
           This handler's only remaining job for the wardriving source is to hand the raw scan
           results across that task boundary, same shape as the non-wardriving branch below
           already uses wifi_scan_done_co for. */
        wifi_scan_raw_count = raw_count;
        ble_npl_callout_reset(&wifi_scan_done_co, 0);
        return;
    }

    keep = (raw_count < FEB_WIFI_SCAN_MAX_APS_PER_RECORD) ? raw_count : FEB_WIFI_SCAN_MAX_APS_PER_RECORD;
    for (k = 0; k < keep; k++) {
        uint16_t best = k;
        uint16_t j;
        wifi_ap_record_t *rec;
        feb_wifi_scan_ap_t *out;
        const char *phy;
        const char *auth;

        for (j = (uint16_t)(k + 1); j < raw_count; j++) {
            if (wifi_scan_raw_records[j].rssi > wifi_scan_raw_records[best].rssi) {
                best = j;
            }
        }
        if (best != k) {
            wifi_ap_record_t tmp = wifi_scan_raw_records[k];

            wifi_scan_raw_records[k] = wifi_scan_raw_records[best];
            wifi_scan_raw_records[best] = tmp;
        }

        rec = &wifi_scan_raw_records[k];
        out = &wifi_scan_selected[k];
        phy = wifi_scan_phy_str(rec);
        auth = wifi_scan_auth_str(rec->authmode);

        out->ssid = rec->ssid;
        out->ssid_len = strnlen((const char *)rec->ssid, sizeof(rec->ssid) - 1u);
        memcpy(out->bssid, rec->bssid, FEB_WIFI_SCAN_BSSID_LEN);
        out->rssi_offset = (uint64_t)((int)rec->rssi + 128);
        out->channel = rec->primary;
        out->phy = phy;
        out->phy_len = strlen(phy);
        out->auth = auth;
        out->auth_len = strlen(auth);
    }

    wifi_scan_found_count = keep;
    wifi_scan_send_next_index = 0;
    ble_npl_callout_reset(&wifi_scan_done_co, 0);
}

/* Runs on the NimBLE host task (wifi_scan_done_co's queue). Ported unchanged from
   esp32/main/main.c's manual-scan path. */
static void wifi_scan_done_cb(struct ble_npl_event *ev)
{
    (void)ev;

    if (wifi_scan_active_source == WIFI_SCAN_SOURCE_WARDRIVING) {
        /* G30 fix, ported unchanged from esp32/main/main.c -- see that file's fuller comment
           and docs/BACKLOG.md G30 for why this dedup/append loop runs here (NimBLE host task)
           rather than in wifi_scan_done_handler() (sys_evt task). wifi_scan_raw_count/
           wifi_scan_raw_records are populated by wifi_scan_done_handler() just before it hands
           off to this callout. */
        feb_location_t fix;
        feb_location_state_t loc_state = location_get_fix(&fix);
        uint16_t k;

        if (loc_state != FEB_LOCATION_FIX) {
            ESP_LOGW(TAG, "wardriving: discarding %u wifi result(s), no GPS fix yet",
                     (unsigned)wifi_scan_raw_count);
        } else {
            for (k = 0; k < wifi_scan_raw_count; k++) {
                wifi_ap_record_t *rec = &wifi_scan_raw_records[k];
                const char *auth = wifi_scan_auth_str(rec->authmode);
                feb_wardriving_record_t record;

                memset(&record, 0, sizeof(record));
                record.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000);
                record.utc_timestamp_s = fix.utc_timestamp_s;
                record.lat_e7_offset = (uint64_t)((int64_t)fix.lat_e7 + 900000000LL);
                record.lon_e7_offset = (uint64_t)((int64_t)fix.lon_e7 + 1800000000LL);
                record.payload_kind = FEB_WARDRIVING_PAYLOAD_WIFI;
                record.payload.wifi.ssid = rec->ssid;
                record.payload.wifi.ssid_len = strnlen((const char *)rec->ssid, sizeof(rec->ssid) - 1u);
                memcpy(record.payload.wifi.bssid, rec->bssid, FEB_WIFI_SCAN_BSSID_LEN);
                record.payload.wifi.rssi_offset = (uint64_t)((int)rec->rssi + 128);
                record.payload.wifi.channel = rec->primary;
                record.payload.wifi.auth = auth;
                record.payload.wifi.auth_len = strlen(auth);
                if (!wardriving_dedup_and_maybe_append(&record)) {
                    ESP_LOGW(TAG, "wardriving: failed to append wifi record to flash log");
                    if (wardriving_flash_failure_count < 0xFFu) {
                        wardriving_flash_failure_count++;
                    }
                    if (wardriving_flash_failure_count >= FEB_WARDRIVING_FLASH_FAILURE_LIMIT) {
                        wardriving_wifi_self_stop_pending = true;
                        break;
                    }
                } else {
                    wardriving_flash_failure_count = 0;
                }
            }
        }

        if (wardriving_wifi_self_stop_pending) {
            wardriving_wifi_self_stop_pending = false;
            wardriving_self_stop("internal_error");
            return;
        }
        if (!wardriving_wifi_active) {
            /* A stop() raced this scan's completion -- handle_wardriving_command()'s stop
               path already cleared wifi_scan_in_progress; nothing else to do. */
            return;
        }
        wardriving_maybe_kick_send(connection_handle);
        ble_npl_callout_reset(&wardriving_wifi_interval_co,
                              ble_npl_time_ms_to_ticks32(wardriving_wifi_interval_ms));
        return;
    }

    if (connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        ESP_LOGW(TAG, "wifi_scan completed with no authenticated connection; discarding %u result(s)",
                 (unsigned)wifi_scan_found_count);
        wifi_scan_in_progress = false;
        return;
    }
    wifi_scan_send_next_batch(connection_handle);
}

/* Builds and sends one wifi_scan `status` record starting at wifi_scan_send_next_index,
   packing as many remaining APs as fit under FEB_CBOR_MAX_PAYLOAD (minus headroom for the
   status payload's own wrapper fields), then chains the next batch (if any) via
   TX_DONE_CONTINUE_WIFI_SCAN once this record's fragments finish sending. Ported unchanged
   from esp32/main/main.c, including its static (not stack-local) result/trial/result_buf --
   see that file's comment on the 2026-09-07 nimble_host stack-protection-fault root cause;
   this board's NimBLE host task stack budget is the same shape. */
static void wifi_scan_send_next_batch(uint16_t conn_handle)
{
    static feb_wifi_scan_result_payload_t result;
    static feb_wifi_scan_result_payload_t trial;
    static feb_status_payload_t status_payload;
    static uint8_t result_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t result_len;
    size_t payload_len;
    bool is_complete;

    memset(&result, 0, sizeof(result));
    memset(&status_payload, 0, sizeof(status_payload));

    while (wifi_scan_send_next_index < wifi_scan_found_count &&
           result.ap_count < FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        size_t trial_len;

        trial = result;

        trial.aps[trial.ap_count] = wifi_scan_selected[wifi_scan_send_next_index];
        trial.ap_count++;
        trial_len = feb_cbor_encode_wifi_scan_result_payload(result_buf, sizeof(result_buf), &trial);
        if (trial_len == 0 || trial_len + FEB_WIFI_SCAN_STATUS_ENCODE_HEADROOM > FEB_CBOR_MAX_PAYLOAD) {
            if (result.ap_count == 0) {
                ESP_LOGE(TAG, "wifi_scan: single AP result too large to encode; dropping it");
                wifi_scan_send_next_index++;
                continue;
            }
            break;
        }
        result = trial;
        wifi_scan_send_next_index++;
    }

    result_len = feb_cbor_encode_wifi_scan_result_payload(result_buf, sizeof(result_buf), &result);
    is_complete = (wifi_scan_send_next_index >= wifi_scan_found_count);

    status_payload.request_id = wifi_scan_request_id;
    status_payload.state = is_complete ? "complete" : "partial";
    status_payload.state_len = strlen(status_payload.state);
    status_payload.result_span = result_buf;
    status_payload.result_span_len = result_len;
    status_payload.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(pairing_payload_encode_buf,
                                                 sizeof(pairing_payload_encode_buf), &status_payload);
    if (payload_len == 0 ||
        !queue_and_send_protected(conn_handle, "status", strlen("status"),
                                  pairing_payload_encode_buf, payload_len,
                                  is_complete ? TX_DONE_NONE : TX_DONE_CONTINUE_WIFI_SCAN)) {
        ESP_LOGE(TAG, "failed to build wifi_scan status record");
        wifi_scan_in_progress = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "sending wifi_scan status (%s, %u AP(s) this batch)",
             is_complete ? "complete" : "partial", (unsigned)result.ap_count);

    if (is_complete) {
        wifi_scan_in_progress = false;
    }
}

/* Ported unchanged from esp32/main/main.c. */
static void handle_wifi_scan_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    size_t arg_count;
    feb_cbor_status_t status;
    esp_err_t err;
    wifi_scan_config_t scan_cfg;

    if (feb_cbor_decode_map_header(cmd->arguments_span, cmd->arguments_span_len, &arg_count, &status) == 0 ||
        arg_count != 0) {
        if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (wifi_scan_in_progress) {
        if (!send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    /* docs/PROTOCOL.md's wifi_scan busy-handling rule ("also rejected busy if wardriving's
       Wi-Fi source is currently active") is already satisfied by the wifi_scan_in_progress
       check above -- wardriving's Wi-Fi source sets that same flag for its entire enabled
       lifetime (see wifi_scan_source_t's comment), so there is nothing extra to check here. */

    wifi_scan_in_progress = true;
    wifi_scan_active_source = WIFI_SCAN_SOURCE_MANUAL;
    wifi_scan_request_id = cmd->request_id;
    memset(&scan_cfg, 0, sizeof(scan_cfg));
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        wifi_scan_in_progress = false;
        if (!send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    ESP_LOGI(TAG, "wifi_scan started (request_id=%llu)", (unsigned long long)cmd->request_id);
}

/* docs/PROTOCOL.md "`ble_scan` command and status payloads": parses one BLE_GAP_EVENT_DISC
   advertisement's fields (reusing ble_hs_adv_parse_fields(), the same primitive
   scan_record_matches() already uses in this same call path) and folds it into
   ble_scan_raw_devices[], deduping by address within the current window and keeping the
   strongest RSSI seen. Ported unchanged from esp32/main/main.c. */
static void ble_scan_catalog_advertisement(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    uint16_t index;
    ble_scan_raw_device_t *dev;

    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return;
    }

    for (index = 0; index < ble_scan_raw_count; index++) {
        if (memcmp(ble_scan_raw_devices[index].addr, disc->addr.val, FEB_BLE_SCAN_ADDRESS_LEN) == 0) {
            break;
        }
    }

    if (index == ble_scan_raw_count) {
        if (ble_scan_raw_count >= FEB_BLE_SCAN_RAW_MAX) {
            return;
        }
        dev = &ble_scan_raw_devices[index];
        memcpy(dev->addr, disc->addr.val, FEB_BLE_SCAN_ADDRESS_LEN);
        dev->addr_type = disc->addr.type;
        dev->rssi = disc->rssi;
        dev->has_name = false;
        dev->name_len = 0;
        ble_scan_raw_count++;
    } else {
        dev = &ble_scan_raw_devices[index];
        if (disc->rssi > dev->rssi) {
            dev->rssi = disc->rssi;
        }
    }

    if (fields.name_len > 0 && !dev->has_name) {
        uint8_t copy_len = fields.name_len;

        if (copy_len > FEB_BLE_SCAN_NAME_MAX_LEN) {
            copy_len = FEB_BLE_SCAN_NAME_MAX_LEN;
        }
        memcpy(dev->name, fields.name, copy_len);
        dev->name_len = copy_len;
        dev->has_name = true;
    }
}

/* Builds and sends one ble_scan `status` record, mirroring wifi_scan_send_next_batch()'s
   packing/chaining exactly. Ported unchanged from esp32/main/main.c. */
static void ble_scan_send_next_batch(uint16_t conn_handle)
{
    static feb_ble_scan_result_payload_t result;
    static feb_ble_scan_result_payload_t trial;
    static feb_status_payload_t status_payload;
    static uint8_t result_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t result_len;
    size_t payload_len;
    bool is_complete;

    memset(&result, 0, sizeof(result));
    memset(&status_payload, 0, sizeof(status_payload));

    while (ble_scan_send_next_index < ble_scan_found_count &&
           result.device_count < FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        size_t trial_len;

        trial = result;
        trial.devices[trial.device_count] = ble_scan_selected[ble_scan_send_next_index];
        trial.device_count++;
        trial_len = feb_cbor_encode_ble_scan_result_payload(result_buf, sizeof(result_buf), &trial);
        if (trial_len == 0 || trial_len + FEB_BLE_SCAN_STATUS_ENCODE_HEADROOM > FEB_CBOR_MAX_PAYLOAD) {
            if (result.device_count == 0) {
                ESP_LOGE(TAG, "ble_scan: single device result too large to encode; dropping it");
                ble_scan_send_next_index++;
                continue;
            }
            break;
        }
        result = trial;
        ble_scan_send_next_index++;
    }

    result_len = feb_cbor_encode_ble_scan_result_payload(result_buf, sizeof(result_buf), &result);
    is_complete = (ble_scan_send_next_index >= ble_scan_found_count);

    status_payload.request_id = ble_scan_request_id;
    status_payload.state = is_complete ? "complete" : "partial";
    status_payload.state_len = strlen(status_payload.state);
    status_payload.result_span = result_buf;
    status_payload.result_span_len = result_len;
    status_payload.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(pairing_payload_encode_buf,
                                                 sizeof(pairing_payload_encode_buf), &status_payload);
    if (payload_len == 0 ||
        !queue_and_send_protected(conn_handle, "status", strlen("status"),
                                  pairing_payload_encode_buf, payload_len,
                                  is_complete ? TX_DONE_NONE : TX_DONE_CONTINUE_BLE_SCAN)) {
        ESP_LOGE(TAG, "failed to build ble_scan status record");
        ble_scan_in_progress = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "sending ble_scan status (%s, %u device(s) this batch)",
             is_complete ? "complete" : "partial", (unsigned)result.device_count);

    if (is_complete) {
        ble_scan_in_progress = false;
    }
}

/* Fires once FEB_BLE_SCAN_WINDOW_MS after handle_ble_scan_command() armed this callout.
   Cancels the still-running discovery, selection-sorts the top
   FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD by RSSI, then sends -- or, if the connection that
   requested this scan is gone, discards. Ported unchanged from esp32/main/main.c's
   manual-scan path. */
static void ble_scan_window_close_cb(struct ble_npl_event *ev)
{
    uint16_t keep;
    uint16_t k;
    int rc;

    (void)ev;

    rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_scan: ble_gap_disc_cancel at window close failed: %d", rc);
    }

    if (ble_scan_active_source == BLE_SCAN_SOURCE_WARDRIVING) {
        /* Ported unchanged from esp32/main/main.c -- see that file's fuller comment.
           wardriving logs every cataloged device this window, not just the top
           FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD by RSSI (that cap is specific to ble_scan's
           one-shot *reporting* contract). */
        feb_location_t fix;
        feb_location_state_t loc_state = location_get_fix(&fix);
        uint16_t k;

        if (loc_state != FEB_LOCATION_FIX) {
            ESP_LOGW(TAG, "wardriving: discarding %u ble result(s), no GPS fix yet",
                     (unsigned)ble_scan_raw_count);
        } else {
            for (k = 0; k < ble_scan_raw_count; k++) {
                ble_scan_raw_device_t *rec = &ble_scan_raw_devices[k];
                feb_wardriving_record_t record;

                memset(&record, 0, sizeof(record));
                record.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000);
                record.utc_timestamp_s = fix.utc_timestamp_s;
                record.lat_e7_offset = (uint64_t)((int64_t)fix.lat_e7 + 900000000LL);
                record.lon_e7_offset = (uint64_t)((int64_t)fix.lon_e7 + 1800000000LL);
                record.payload_kind = FEB_WARDRIVING_PAYLOAD_BLE;
                memcpy(record.payload.ble.address, rec->addr, FEB_BLE_SCAN_ADDRESS_LEN);
                if (rec->has_name) {
                    record.payload.ble.name = rec->name;
                    record.payload.ble.name_len = rec->name_len;
                    record.payload.ble.has_name = 1;
                }
                record.payload.ble.rssi_offset = (uint64_t)((int)rec->rssi + 128);
                if (!wardriving_dedup_and_maybe_append(&record)) {
                    ESP_LOGW(TAG, "wardriving: failed to append ble record to flash log");
                    if (wardriving_flash_failure_count < 0xFFu) {
                        wardriving_flash_failure_count++;
                    }
                    if (wardriving_flash_failure_count >= FEB_WARDRIVING_FLASH_FAILURE_LIMIT) {
                        wardriving_ble_self_stop_pending = true;
                        break;
                    }
                } else {
                    wardriving_flash_failure_count = 0;
                }
            }
        }

        if (wardriving_ble_self_stop_pending) {
            wardriving_ble_self_stop_pending = false;
            wardriving_self_stop("internal_error");
            return;
        }
        if (!wardriving_ble_active) {
            /* A stop() raced this window's completion -- handle_wardriving_command()'s stop
               path already cleared ble_scan_in_progress; nothing else to do. */
            return;
        }
        wardriving_maybe_kick_send(connection_handle);
        {
            uint32_t gap_ms = (wardriving_ble_interval_ms > wardriving_ble_window_ms) ?
                              (wardriving_ble_interval_ms - wardriving_ble_window_ms) : 0u;

            ble_npl_callout_reset(&wardriving_ble_interval_co, ble_npl_time_ms_to_ticks32(gap_ms));
        }
        return;
    }

    keep = (ble_scan_raw_count < FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) ?
           ble_scan_raw_count : FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD;
    for (k = 0; k < keep; k++) {
        uint16_t best = k;
        uint16_t j;
        ble_scan_raw_device_t *rec;
        feb_ble_scan_device_t *out;

        for (j = (uint16_t)(k + 1); j < ble_scan_raw_count; j++) {
            if (ble_scan_raw_devices[j].rssi > ble_scan_raw_devices[best].rssi) {
                best = j;
            }
        }
        if (best != k) {
            ble_scan_raw_device_t tmp = ble_scan_raw_devices[k];

            ble_scan_raw_devices[k] = ble_scan_raw_devices[best];
            ble_scan_raw_devices[best] = tmp;
        }

        rec = &ble_scan_raw_devices[k];
        out = &ble_scan_selected[k];
        memcpy(out->address, rec->addr, FEB_BLE_SCAN_ADDRESS_LEN);
        if (rec->has_name) {
            out->name = rec->name;
            out->name_len = rec->name_len;
            out->has_name = 1;
        } else {
            out->name = NULL;
            out->name_len = 0;
            out->has_name = 0;
        }
        out->rssi_offset = (uint64_t)((int)rec->rssi + 128);
        out->addr_type = ble_scan_addr_type_str(rec->addr_type);
        out->addr_type_len = strlen(out->addr_type);
    }

    ble_scan_found_count = keep;
    ble_scan_send_next_index = 0;

    if (connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        ESP_LOGW(TAG, "ble_scan window closed with no authenticated connection; discarding %u result(s)",
                 (unsigned)ble_scan_found_count);
        ble_scan_in_progress = false;
        return;
    }
    ble_scan_send_next_batch(connection_handle);
}

/* Mirrors handle_wifi_scan_command()'s validation/busy-check/start shape exactly, substituting
   a fixed-duration discovery window for a Wi-Fi scan. The window itself is closed by
   ble_scan_done_co (armed here), not by any GAP "discovery complete" callback -- NimBLE
   discovery with BLE_HS_FOREVER runs until explicitly cancelled. Ported unchanged from
   esp32/main/main.c. */
static void handle_ble_scan_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    size_t arg_count;
    feb_cbor_status_t status;
    struct ble_gap_disc_params params = {0};
    int rc;

    if (feb_cbor_decode_map_header(cmd->arguments_span, cmd->arguments_span_len, &arg_count, &status) == 0 ||
        arg_count != 0) {
        if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (ble_scan_in_progress) {
        if (!send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    /* docs/PROTOCOL.md's ble_scan busy-handling rule ("also rejected busy if wardriving's
       BLE source is currently active") is already satisfied by the ble_scan_in_progress
       check above -- wardriving's BLE source sets that same flag for its entire enabled
       lifetime (see ble_scan_source_t's comment), so there is nothing extra to check here. */

    ble_scan_in_progress = true;
    ble_scan_active_source = BLE_SCAN_SOURCE_MANUAL;
    ble_scan_request_id = cmd->request_id;
    ble_scan_raw_count = 0;

    params.passive = 0;
    /* Active scanning: send scan requests and collect scan response data, which often
       includes device names that passive advertisements omit -- same tradeoff as the C6. */
    params.filter_duplicates = 0;
    params.itvl = 0;
    params.window = 0;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_scan: ble_gap_disc start failed: %d", rc);
        ble_scan_in_progress = false;
        if (!send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    ble_npl_callout_reset(&ble_scan_done_co, ble_npl_time_ms_to_ticks32(FEB_BLE_SCAN_WINDOW_MS));
    ESP_LOGI(TAG, "ble_scan started (request_id=%llu)", (unsigned long long)cmd->request_id);
}

/* docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and real wardriving-record
   timestamps": `gps` is a poll-only, single-shot status query -- no scan-duration lifecycle,
   no busy/exclusivity concept (the UART read is a passive background task independent of the
   Wi-Fi/BLE radio), no request_id dedup cache (matches wifi_scan/ble_scan). `result` is
   present only when state == "fix", per docs/PROTOCOL.md's `gps` status table. Ported
   unchanged from esp32/main/main.c's handle_gps_command() -- this capability's wire shape is
   board-agnostic; only location.c's pin/UART-port choice differs by board. */
static void handle_gps_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    size_t arg_count;
    feb_cbor_status_t status;
    feb_location_t fix;
    feb_location_state_t loc_state;
    feb_status_payload_t status_payload = {0};
    /* Sized with real margin, matching esp32/main/main.c's own 2026-09-21 hardware-found fix
       (a 128-byte buffer silently failed to encode a real fix's large lat/lon/timestamp/
       altitude values) -- see that file's handle_gps_command() for the full byte accounting. */
    uint8_t result_buf[192];
    size_t payload_len;

    if (feb_cbor_decode_map_header(cmd->arguments_span, cmd->arguments_span_len, &arg_count, &status) == 0 ||
        arg_count != 0) {
        if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    loc_state = location_get_fix(&fix);
    status_payload.request_id = cmd->request_id;
    switch (loc_state) {
    case FEB_LOCATION_FIX: status_payload.state = "fix"; break;
    case FEB_LOCATION_ACQUIRING: status_payload.state = "acquiring"; break;
    case FEB_LOCATION_NO_SIGNAL:
    default: status_payload.state = "no_signal"; break;
    }
    status_payload.state_len = strlen(status_payload.state);

    if (loc_state == FEB_LOCATION_FIX) {
        feb_gps_result_payload_t result = {0};
        size_t result_len;

        result.lat_e7_offset = (uint64_t)((int64_t)fix.lat_e7 + 900000000LL);
        result.lon_e7_offset = (uint64_t)((int64_t)fix.lon_e7 + 1800000000LL);
        result.fix_quality = fix.fix_quality;
        result.satellites = fix.satellites;
        result.hdop_e1 = fix.hdop_e1;
        result.utc_timestamp_s = fix.utc_timestamp_s;
        result.altitude_dm_offset = (uint64_t)((int64_t)fix.altitude_dm + FEB_GPS_ALTITUDE_DM_OFFSET);
        result.speed_e1_kmh = fix.speed_e1_kmh;

        result_len = feb_cbor_encode_gps_result_payload(result_buf, sizeof(result_buf), &result);
        if (result_len == 0) {
            if (!send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                      1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
        status_payload.result_span = result_buf;
        status_payload.result_span_len = result_len;
        status_payload.has_result = 1;
    }

    payload_len = feb_cbor_encode_status_payload(pairing_payload_encode_buf,
                                                 sizeof(pairing_payload_encode_buf), &status_payload);
    if (payload_len == 0 ||
        !send_protected(conn_handle, "status", strlen("status"), pairing_payload_encode_buf, payload_len)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "gps status query answered (request_id=%llu, state=%s)",
             (unsigned long long)cmd->request_id, status_payload.state);
}

/* Re-arms the next Wi-Fi capture pass after wardriving_wifi_interval_ms (0 =
   immediate/continuous) -- runs on the NimBLE host task (wardriving_wifi_interval_co's
   queue). Ported unchanged from esp32/main/main.c, including its speed-based WiFi swelling
   hysteresis logic (docs/WARDRIVING_REDESIGN.md) -- see that file's fuller comment. */
static void wardriving_wifi_interval_cb(struct ble_npl_event *ev)
{
    wifi_scan_config_t scan_cfg;
    esp_err_t err;

    (void)ev;
    if (!wardriving_wifi_active) {
        return;
    }
    if (wardriving_wifi_swelling == WARDRIVING_SWELLING_SPEED_BASED) {
        feb_location_t fix;
        feb_location_state_t loc_state = location_get_fix(&fix);

        if (loc_state == FEB_LOCATION_FIX) {
            if (fix.speed_e1_kmh >= 100u) {
                wardriving_swelling_aggressive_active = true;
            } else if (fix.speed_e1_kmh < 80u) {
                wardriving_swelling_aggressive_active = false;
            }
        }
    }
    memset(&scan_cfg, 0, sizeof(scan_cfg));
    wardriving_apply_wifi_swelling(&scan_cfg);
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wardriving: re-trigger esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        wardriving_self_stop("internal_error");
    }
}

/* Same re-arming role as wardriving_wifi_interval_cb() above, for the BLE source. Ported
   unchanged from esp32/main/main.c -- see that file's fuller comment, including why the
   merged reconnect-scan pass must use active (not passive) discovery. */
static void wardriving_ble_interval_cb(struct ble_npl_event *ev)
{
    struct ble_gap_disc_params params = {0};
    int rc;

    (void)ev;
    if (!wardriving_ble_active) {
        return;
    }
    ble_scan_raw_count = 0;
    params.passive = wardriving_ble_passive &&
                     runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED;
    params.filter_duplicates = 0;
    params.itvl = 0;
    params.window = 0;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == BLE_HS_EBUSY) {
        ble_npl_callout_reset(&wardriving_ble_interval_co,
                              ble_npl_time_ms_to_ticks32(wardriving_ble_window_ms));
        return;
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "wardriving: re-trigger ble_gap_disc failed: %d", rc);
        wardriving_self_stop("internal_error");
        return;
    }
    ble_npl_callout_reset(&ble_scan_done_co, ble_npl_time_ms_to_ticks32(wardriving_ble_window_ms));
}

static void wardriving_sync_status_led(void)
{
    feb_status_led_set_wardriving_active(wardriving_wifi_active || wardriving_ble_active);
}

/* Shared teardown for every wardriving stop path. Ported unchanged from esp32/main/main.c --
   see that file's fuller comment. Runs on the NimBLE host task only. */
static void wardriving_stop_internal(void)
{
    if (wardriving_wifi_active) {
        wardriving_wifi_active = false;
        wifi_scan_in_progress = false;
        ble_npl_callout_stop(&wardriving_wifi_interval_co);
        {
            esp_err_t serr = esp_wifi_scan_stop();

            if (serr != ESP_OK && serr != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_scan_stop failed while stopping wardriving: %s",
                         esp_err_to_name(serr));
            }
        }
    }
    if (wardriving_ble_active) {
        wardriving_ble_active = false;
        ble_scan_in_progress = false;
        ble_npl_callout_stop(&wardriving_ble_interval_co);
        {
            int derr = ble_gap_disc_cancel();

            if (derr != 0 && derr != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "ble_gap_disc_cancel failed while stopping wardriving: %d", derr);
            }
        }
    }
    wardriving_sync_status_led();
}

/* Ported unchanged from esp32/main/main.c -- see that file's fuller comment. */
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
   autostart, boot-button toggle-on). Ported unchanged from esp32/main/main.c -- see that
   file's fuller comment. Runs on the NimBLE host task only. */
static bool wardriving_start_internal(bool want_wifi, bool want_ble, bool want_ble_passive,
                                      uint32_t wifi_interval_ms, uint32_t ble_window_ms,
                                      uint32_t ble_interval_ms,
                                      wardriving_swelling_mode_t wifi_swelling,
                                      wardriving_country_t country)
{
    if (wardriving_wifi_active || wardriving_ble_active) {
        return false;
    }
    if ((want_wifi && wifi_scan_in_progress) || (want_ble && ble_scan_in_progress)) {
        return false;
    }

    if (want_wifi) {
        wifi_scan_config_t scan_cfg;
        esp_err_t err;
        esp_err_t country_err;

        memset(&scan_cfg, 0, sizeof(scan_cfg));
        wifi_scan_in_progress = true;
        wifi_scan_active_source = WIFI_SCAN_SOURCE_WARDRIVING;
        wardriving_wifi_interval_ms = wifi_interval_ms;
        wardriving_wifi_swelling = wifi_swelling;
        wardriving_swelling_aggressive_active = false;

        /* Applied once, here, at start -- a radio-global setting per
           docs/WARDRIVING_REDESIGN.md, so any later manual wifi_scan observes whatever
           country wardriving last set. Best-effort: a failure here doesn't abort wardriving
           start. */
        country_err = esp_wifi_set_country_code(
            country == WARDRIVING_COUNTRY_BG ? "BG" : "01", false);
        if (country_err != ESP_OK) {
            ESP_LOGW(TAG, "wardriving: esp_wifi_set_country_code failed: %s",
                     esp_err_to_name(country_err));
        }

        wardriving_apply_wifi_swelling(&scan_cfg);
        err = esp_wifi_scan_start(&scan_cfg, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wardriving: esp_wifi_scan_start failed: %s", esp_err_to_name(err));
            wifi_scan_in_progress = false;
            return false;
        }
        wardriving_wifi_active = true;
        wardriving_sync_status_led();
    }

    if (want_ble) {
        struct ble_gap_disc_params params = {0};
        int rc;

        ble_scan_in_progress = true;
        ble_scan_active_source = BLE_SCAN_SOURCE_WARDRIVING;
        ble_scan_raw_count = 0;
        wardriving_ble_window_ms = ble_window_ms;
        wardriving_ble_interval_ms = ble_interval_ms;
        wardriving_ble_passive = want_ble_passive;

        params.passive = wardriving_ble_passive &&
                 runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED;
        params.filter_duplicates = 0;
        params.itvl = 0;
        params.window = 0;
        rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "wardriving: ble_gap_disc start failed: %d", rc);
            ble_scan_in_progress = false;
            if (want_wifi) {
                wardriving_wifi_active = false;
                wifi_scan_in_progress = false;
                esp_wifi_scan_stop();
                wardriving_sync_status_led();
            }
            return false;
        }
        ble_npl_callout_reset(&ble_scan_done_co, ble_npl_time_ms_to_ticks32(wardriving_ble_window_ms));
        wardriving_ble_active = true;
        wardriving_sync_status_led();
    }

    return true;
}

/* Stops whichever wardriving source(s) are active and, if connected+authenticated, sends the
   docs/PROTOCOL.md-specified unsolicited error + status(state="stopped") pair. Ported
   unchanged from esp32/main/main.c -- see that file's fuller comment. Runs on the NimBLE host
   task only. */
static void wardriving_self_stop(const char *error_code)
{
    bool was_active = wardriving_wifi_active || wardriving_ble_active;

    wardriving_stop_internal();
    if (!was_active) {
        return;
    }
    ESP_LOGE(TAG, "wardriving self-stopped (%s)", error_code);

    if (connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        return;
    }
    {
        feb_error_payload_t err = {0};
        size_t payload_len;

        err.code = error_code;
        err.code_len = strlen(error_code);
        err.has_request_id = 0;
        payload_len = feb_cbor_encode_error_payload(pairing_payload_encode_buf,
                                                    sizeof(pairing_payload_encode_buf), &err);
        if (payload_len == 0 ||
            !queue_and_send_protected(connection_handle, "error", strlen("error"),
                                      pairing_payload_encode_buf, payload_len,
                                      TX_DONE_SEND_WARDRIVING_STOPPED)) {
            ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

/* Runs on the NimBLE host task (posted via wardriving_button_toggle_ev). Ported unchanged
   from esp32/main/main.c -- see that file's fuller comment (and factory_reset.c's
   FEB_WARDRIVING_TOGGLE_MIN_MS/MAX_MS, ported alongside this for the same GPIO0 button). */
static void wardriving_button_toggle_cb(struct ble_npl_event *ev)
{
    uint32_t pending = __atomic_exchange_n(&wardriving_button_toggle_pending, 0, __ATOMIC_SEQ_CST);

    (void)ev;
    for (; pending > 0; pending--) {
        if (wardriving_wifi_active || wardriving_ble_active) {
            wardriving_stop_internal();
            wardriving_persisted.enabled = false;
            wardriving_persist_save(&wardriving_persisted);
            ESP_LOGI(TAG, "wardriving stopped via boot-button toggle");
            continue;
        }
        if (wardriving_start_internal(wardriving_persisted.want_wifi, wardriving_persisted.want_ble,
                                      wardriving_persisted.want_ble_passive,
                                      wardriving_persisted.wifi_interval_ms,
                                      wardriving_persisted.ble_window_ms,
                                      wardriving_persisted.ble_interval_ms,
                                      WARDRIVING_SWELLING_NORMAL, WARDRIVING_COUNTRY_ROW)) {
            wardriving_persisted.enabled = true;
            wardriving_persist_save(&wardriving_persisted);
            ESP_LOGI(TAG, "wardriving started via boot-button toggle (wifi=%d ble=%d ble_passive=%d)",
                     (int)wardriving_persisted.want_wifi, (int)wardriving_persisted.want_ble,
                     (int)wardriving_persisted.want_ble_passive);
        } else {
            ESP_LOGW(TAG, "wardriving boot-button toggle-on rejected (radio busy or start failed)");
        }
    }
}

/* Declared in factory_reset.h. Ported unchanged from esp32/main/main.c -- see that file's
   fuller comment. */
void feb_wardriving_request_button_toggle(void)
{
    if (!wardriving_control_ready) {
        ESP_LOGW(TAG, "boot-button press ignored: wardriving control not ready yet");
        return;
    }
    __atomic_fetch_add(&wardriving_button_toggle_pending, 1, __ATOMIC_SEQ_CST);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &wardriving_button_toggle_ev);
}

/* Ported unchanged from esp32/main/main.c -- see that file's fuller comment. */
static void wardriving_maybe_kick_send(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        return;
    }
    if (wardriving_tx_in_flight) {
        return;
    }
    if (wardriving_log_pending_count() == 0) {
        return;
    }
    wardriving_tx_in_flight = true;
    feb_status_led_set(FEB_STATUS_LED_FLUSHING);
    wardriving_send_next_batch(conn_handle);
}

/* Builds and sends one wardriving status(state="data") record. Ported unchanged from
   esp32/main/main.c -- see that file's fuller comment on why peeked/peek_scratch/result/
   trial/status_payload/result_buf are static, not stack-local (the same nimble_host task
   stack-budget argument applies unchanged on this board's classic-ESP32 NimBLE host task). */
static void wardriving_send_next_batch(uint16_t conn_handle)
{
    static feb_wardriving_record_t peeked[FEB_WARDRIVING_MAX_RECORDS_PER_BATCH];
    static uint8_t peek_scratch[FEB_WARDRIVING_PEEK_SCRATCH_LEN];
    static feb_wardriving_status_result_payload_t result;
    static feb_wardriving_status_result_payload_t trial;
    static feb_status_payload_t status_payload;
    static uint8_t result_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t peeked_count;
    size_t include_count;
    size_t remaining_after;
    size_t result_len;
    size_t payload_len;
    size_t pending_now;

    if (wardriving_pending_drain_count > 0) {
        wardriving_log_mark_drained(wardriving_pending_drain_count);
        wardriving_pending_drain_count = 0;
    }

    peeked_count = wardriving_log_peek_pending(peeked, FEB_WARDRIVING_MAX_RECORDS_PER_BATCH,
                                               peek_scratch, sizeof(peek_scratch));
    if (peeked_count == 0) {
        wardriving_tx_in_flight = false;
        feb_status_led_set(FEB_STATUS_LED_CONNECTED);
        return;
    }

    memset(&result, 0, sizeof(result));
    include_count = 0;
    pending_now = wardriving_log_pending_count();
    while (include_count < peeked_count) {
        size_t trial_len;

        trial = result;
        trial.records[trial.record_count] = peeked[include_count];
        trial.record_count++;
        trial.backlog_remaining = (pending_now >= trial.record_count) ?
                                  (pending_now - trial.record_count) : 0;
        trial_len = feb_cbor_encode_wardriving_status_result_payload(result_buf, sizeof(result_buf), &trial);
        if (trial_len == 0 || trial_len + FEB_WARDRIVING_STATUS_ENCODE_HEADROOM > FEB_CBOR_MAX_PAYLOAD) {
            if (result.record_count == 0) {
                ESP_LOGE(TAG, "wardriving: single record too large to encode; dropping it");
                include_count++;
                continue;
            }
            break;
        }
        result = trial;
        include_count++;
    }

    remaining_after = (pending_now >= include_count) ? (pending_now - include_count) : 0;
    result.backlog_remaining = remaining_after;
    result_len = feb_cbor_encode_wardriving_status_result_payload(result_buf, sizeof(result_buf), &result);

    memset(&status_payload, 0, sizeof(status_payload));
    status_payload.request_id = 0;
    status_payload.state = "data";
    status_payload.state_len = strlen("data");
    status_payload.result_span = result_buf;
    status_payload.result_span_len = result_len;
    status_payload.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(pairing_payload_encode_buf,
                                                 sizeof(pairing_payload_encode_buf), &status_payload);
    if (result_len == 0) {
        ESP_LOGE(TAG, "wardriving status(data) result encode failed (records=%u, peeked=%u)",
                 (unsigned)result.record_count, (unsigned)peeked_count);
    } else if (payload_len == 0) {
        ESP_LOGE(TAG, "wardriving status(data) wrapper encode failed (result_len=%u)",
                 (unsigned)result_len);
    }
    if (payload_len == 0) {
        ESP_LOGE(TAG, "failed to build wardriving status(data) record");
        wardriving_tx_in_flight = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    if (!queue_and_send_protected(conn_handle, "status", strlen("status"),
                                  pairing_payload_encode_buf, payload_len,
                                  TX_DONE_CONTINUE_WARDRIVING)) {
        ESP_LOGE(TAG, "failed to queue wardriving status(data) record (payload_len=%u)",
                 (unsigned)payload_len);
        wardriving_tx_in_flight = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    wardriving_pending_drain_count = include_count;
    ESP_LOGI(TAG, "sending wardriving status(data) (%u record(s), backlog_remaining=%u)",
             (unsigned)result.record_count, (unsigned)remaining_after);
}

static bool wardriving_source_requested(const feb_wardriving_command_payload_t *payload, const char *name)
{
    size_t len = strlen(name);
    size_t i;

    for (i = 0; i < payload->source_count; i++) {
        if (payload->source_lens[i] == len && memcmp(payload->sources[i], name, len) == 0) {
            return true;
        }
    }
    return false;
}

/* docs/PROTOCOL.md "`wardriving` command and status payloads" / docs/CAPABILITIES.md's
   `wardriving` bullet. Ported unchanged from esp32/main/main.c -- see that file's fuller
   comment on the "required vs. default when omitted" interval resolution
   (wardriving_resolve_start_intervals(), wardriving_validate.h/.c, copied unchanged). */
static void handle_wardriving_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    feb_wardriving_command_payload_t payload;
    feb_cbor_status_t status;
    bool is_start;
    bool is_stop;
    bool is_status_query;
    bool want_wifi = false;
    bool want_ble = false;
    bool want_ble_passive = false;
    wardriving_swelling_mode_t wifi_swelling = WARDRIVING_SWELLING_NORMAL;
    wardriving_country_t country = WARDRIVING_COUNTRY_ROW;
    size_t i;

    status = feb_cbor_decode_wardriving_command_payload(cmd->arguments_span, cmd->arguments_span_len, &payload);
    if (status != FEB_CBOR_OK) {
        if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    is_start = (payload.action_len == strlen("start") && memcmp(payload.action, "start", payload.action_len) == 0);
    is_stop = (payload.action_len == strlen("stop") && memcmp(payload.action, "stop", payload.action_len) == 0);
    is_status_query =
        (payload.action_len == strlen("status") && memcmp(payload.action, "status", payload.action_len) == 0);
    if (!is_start && !is_stop && !is_status_query) {
        if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (is_status_query) {
        if (payload.has_sources || payload.has_wifi_interval_ms || payload.has_ble_params ||
            payload.has_wifi_swelling || payload.has_country) {
            if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        feb_status_payload_t status_payload = {0};
        size_t payload_len;

        status_payload.request_id = cmd->request_id;
        status_payload.state = (wardriving_wifi_active || wardriving_ble_active) ? "started" : "stopped";
        status_payload.state_len = strlen(status_payload.state);
        payload_len = feb_cbor_encode_status_payload(
            pairing_payload_encode_buf, sizeof(pairing_payload_encode_buf), &status_payload);
        if (payload_len == 0 ||
            !send_protected(conn_handle, "status", strlen("status"), pairing_payload_encode_buf, payload_len)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        ESP_LOGI(TAG, "wardriving status query answered (request_id=%llu, running=%d)",
                 (unsigned long long)cmd->request_id,
                 (int)(wardriving_wifi_active || wardriving_ble_active));
        return;
    }

    if (is_stop) {
        if (payload.has_sources || payload.has_wifi_interval_ms || payload.has_ble_params ||
            payload.has_wifi_swelling || payload.has_country) {
            if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
        if (!wardriving_wifi_active && !wardriving_ble_active) {
            if (!send_protected_error(conn_handle, "not_running", strlen("not_running"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        wardriving_stop_internal();
        wardriving_persisted.enabled = false;
        wardriving_persist_save(&wardriving_persisted);

        {
            feb_status_payload_t status_payload = {0};
            size_t payload_len;

            status_payload.request_id = cmd->request_id;
            status_payload.state = "stopped";
            status_payload.state_len = strlen("stopped");
            payload_len = feb_cbor_encode_status_payload(pairing_payload_encode_buf,
                                                         sizeof(pairing_payload_encode_buf), &status_payload);
            if (payload_len == 0 ||
                !send_protected(conn_handle, "status", strlen("status"), pairing_payload_encode_buf, payload_len)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return;
            }
        }
        ESP_LOGI(TAG, "wardriving stopped (request_id=%llu)", (unsigned long long)cmd->request_id);
        return;
    }

    /* is_start */
    if (wardriving_wifi_active || wardriving_ble_active) {
        if (!send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    if (!payload.has_sources || payload.source_count == 0) {
        if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    want_wifi = wardriving_source_requested(&payload, "wifi");
    want_ble = wardriving_source_requested(&payload, "ble");
    want_ble_passive = wardriving_source_requested(&payload, "ble_passive");
    {
        size_t recognized_count = (want_wifi ? 1u : 0u) + (want_ble ? 1u : 0u) +
                                  (want_ble_passive ? 1u : 0u);

        if (payload.source_count != recognized_count || (want_ble && want_ble_passive)) {
            if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
    }

    {
        bool have_wifi_scan = false;
        bool have_ble_scan = false;

        for (i = 0; i < FEB_FEATURE_COUNT; i++) {
            if (strcmp(feb_features[i], "wifi_scan") == 0) have_wifi_scan = true;
            if (strcmp(feb_features[i], "ble_scan") == 0) have_ble_scan = true;
        }
        if ((want_wifi && !have_wifi_scan) || ((want_ble || want_ble_passive) && !have_ble_scan)) {
            if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
    }

    if (want_wifi) {
        bool wifi_swelling_ok = false;
        bool country_ok = false;

        if (!payload.has_wifi_swelling || !payload.has_country) {
            if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        if (payload.wifi_swelling_len == strlen("normal") &&
            memcmp(payload.wifi_swelling, "normal", payload.wifi_swelling_len) == 0) {
            wifi_swelling = WARDRIVING_SWELLING_NORMAL;
            wifi_swelling_ok = true;
        } else if (payload.wifi_swelling_len == strlen("aggressive") &&
                  memcmp(payload.wifi_swelling, "aggressive", payload.wifi_swelling_len) == 0) {
            wifi_swelling = WARDRIVING_SWELLING_AGGRESSIVE;
            wifi_swelling_ok = true;
        } else if (payload.wifi_swelling_len == strlen("speed_based") &&
                  memcmp(payload.wifi_swelling, "speed_based", payload.wifi_swelling_len) == 0) {
            wifi_swelling = WARDRIVING_SWELLING_SPEED_BASED;
            wifi_swelling_ok = true;
        }

        if (payload.country_len == strlen("BG") &&
            memcmp(payload.country, "BG", payload.country_len) == 0) {
            country = WARDRIVING_COUNTRY_BG;
            country_ok = true;
        } else if (payload.country_len == strlen("RoW") &&
                  memcmp(payload.country, "RoW", payload.country_len) == 0) {
            country = WARDRIVING_COUNTRY_ROW;
            country_ok = true;
        }

        if (!wifi_swelling_ok || !country_ok) {
            if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
    } else if (payload.has_wifi_swelling || payload.has_country) {
        if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    {
        wardriving_start_request_t req = {0};
        wardriving_resolved_intervals_t resolved;

        req.want_wifi = want_wifi;
        req.want_ble = want_ble || want_ble_passive;
        req.has_wifi_interval_ms = payload.has_wifi_interval_ms;
        req.wifi_interval_ms = payload.wifi_interval_ms;
        req.has_ble_params = payload.has_ble_params;
        req.ble_window_ms = payload.ble_window_ms;
        req.ble_interval_ms = payload.ble_interval_ms;

        if (!wardriving_resolve_start_intervals(&req, &resolved)) {
            if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        payload.wifi_interval_ms = resolved.wifi_interval_ms;
        payload.ble_window_ms = resolved.ble_window_ms;
        payload.ble_interval_ms = resolved.ble_interval_ms;
    }

    if (want_wifi && wifi_scan_in_progress) {
        if (!send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    if (want_ble && ble_scan_in_progress) {
        if (!send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (!wardriving_start_internal(want_wifi, want_ble, want_ble_passive,
                                   (uint32_t)payload.wifi_interval_ms,
                                   (uint32_t)payload.ble_window_ms,
                                   (uint32_t)payload.ble_interval_ms,
                                   wifi_swelling, country)) {
        if (!send_protected_error(conn_handle, "internal_error", strlen("internal_error"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    wardriving_persisted.enabled = true;
    wardriving_persisted.want_wifi = want_wifi;
    wardriving_persisted.want_ble = want_ble;
    wardriving_persisted.want_ble_passive = want_ble_passive;
    wardriving_persisted.wifi_interval_ms = (uint32_t)payload.wifi_interval_ms;
    wardriving_persisted.ble_window_ms = (uint32_t)payload.ble_window_ms;
    wardriving_persisted.ble_interval_ms = (uint32_t)payload.ble_interval_ms;
    wardriving_persist_save(&wardriving_persisted);

    {
        feb_status_payload_t status_payload = {0};
        size_t payload_len;

        status_payload.request_id = cmd->request_id;
        status_payload.state = "started";
        status_payload.state_len = strlen("started");
        payload_len = feb_cbor_encode_status_payload(pairing_payload_encode_buf,
                                                     sizeof(pairing_payload_encode_buf), &status_payload);
        if (payload_len == 0 ||
            !send_protected(conn_handle, "status", strlen("status"), pairing_payload_encode_buf, payload_len)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
    }
    ESP_LOGI(TAG, "wardriving started (request_id=%llu, wifi=%d ble=%d)",
             (unsigned long long)cmd->request_id, (int)want_wifi, (int)want_ble);
}

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": capability-name
   dispatch. */
static void handle_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    if (cmd->capability_len == strlen("wifi_scan") &&
        memcmp(cmd->capability, "wifi_scan", cmd->capability_len) == 0) {
        handle_wifi_scan_command(conn_handle, cmd);
    } else if (cmd->capability_len == strlen("ble_scan") &&
              memcmp(cmd->capability, "ble_scan", cmd->capability_len) == 0) {
        handle_ble_scan_command(conn_handle, cmd);
    } else if (cmd->capability_len == strlen("wardriving") &&
              memcmp(cmd->capability, "wardriving", cmd->capability_len) == 0) {
        handle_wardriving_command(conn_handle, cmd);
    } else if (cmd->capability_len == strlen("gps") &&
              memcmp(cmd->capability, "gps", cmd->capability_len) == 0) {
        handle_gps_command(conn_handle, cmd);
    } else if (!send_protected_error(conn_handle, "unsupported_capability", strlen("unsupported_capability"),
                              1, cmd->request_id)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void fail_pairing_ceremony(uint16_t conn_handle, feb_pairing_error_t err)
{
    feb_error_payload_t error_payload = {0};
    size_t payload_len;

    error_payload.code = feb_pairing_error_code_str(err);
    error_payload.code_len = strlen(error_payload.code);

    payload_len = feb_cbor_encode_error_payload(pairing_payload_encode_buf,
                                                sizeof(pairing_payload_encode_buf), &error_payload);
    pairing_attempt_zeroize();
    if (payload_len == 0 ||
        !encode_and_queue_pairing_record("error", strlen("error"), pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build pairing error record; disconnecting directly");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    tx_done_action = TX_DONE_DISCONNECT;
    send_next_tx_fragment(conn_handle);
}

static void begin_pairing(uint16_t conn_handle)
{
    uint8_t random_buf[FEB_X25519_KEY_LEN];
    feb_pair_init_payload_t init_payload;
    size_t payload_len;

    if (!pairing_window_is_open()) {
        ESP_LOGW(TAG, "pairing window closed before pair_init could be sent");
        fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_EXPIRED);
        return;
    }

    esp_fill_random(random_buf, sizeof(random_buf));
    feb_x25519_keypair(esp32_private_key, esp32_public_key, random_buf);
    feb_secure_zero(random_buf, sizeof(random_buf));
    esp_fill_random(device_nonce, sizeof(device_nonce));

    memcpy(init_payload.pairing_epoch, pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    memcpy(init_payload.device_nonce, device_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(init_payload.esp32_public_key, esp32_public_key, FEB_PAIRING_PUBKEY_LEN);

    payload_len = feb_cbor_encode_pair_init_payload(pairing_payload_encode_buf,
                                                    sizeof(pairing_payload_encode_buf), &init_payload);
    if (payload_len == 0 ||
        !encode_and_queue_pairing_record(FEB_PAIR_INIT_TYPE, strlen(FEB_PAIR_INIT_TYPE),
                                         pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build pair_init");
        pairing_attempt_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    pairing_state = PAIRING_STATE_INIT_SENT;
    pair_reply_wait_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    tx_done_action = TX_DONE_AWAIT_PAIR_REPLY;
    ESP_LOGI(TAG, "sending pair_init");
    send_next_tx_fragment(conn_handle);
}

static void handle_pair_reply(uint16_t conn_handle, const feb_pairing_envelope_t *envelope)
{
    feb_pair_reply_payload_t reply;
    feb_cbor_status_t status;
    feb_pairing_transcript_t transcript = {0};
    uint8_t expected_confirm[FEB_PAIRING_REPLY_CONFIRM_LEN];
    feb_pair_confirm_payload_t confirm_payload;
    size_t payload_len;

    pair_reply_wait_start_ms = 0;
    status = feb_cbor_decode_pair_reply_payload(envelope->payload_span, envelope->payload_span_len, &reply);
    if (status != FEB_CBOR_OK) {
        ESP_LOGW(TAG, "pair_reply payload decode failed: %d", (int)status);
        fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_x25519(k_shared, esp32_private_key, reply.flipper_public_key);
    if (feb_is_all_zero(k_shared, sizeof(k_shared))) {
        ESP_LOGW(TAG, "rejected all-zero X25519 shared secret");
        fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    memcpy(client_nonce, reply.client_nonce, FEB_PAIRING_NONCE_LEN);

    transcript.version = 2;
    memcpy(transcript.service_uuid, FEB_PAIRING_SERVICE_UUID, FEB_PAIRING_SERVICE_UUID_LEN);
    transcript.board_id = board_id_buf;
    transcript.board_id_len = board_id_len;
    memcpy(transcript.pairing_epoch, pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    memcpy(transcript.client_nonce, client_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(transcript.device_nonce, device_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(transcript.esp32_public_key, esp32_public_key, FEB_PAIRING_PUBKEY_LEN);
    memcpy(transcript.flipper_public_key, reply.flipper_public_key, FEB_PAIRING_PUBKEY_LEN);

    pairing_transcript_len = feb_pairing_encode_transcript(pairing_transcript_buf,
                                                           sizeof(pairing_transcript_buf), &transcript);
    if (pairing_transcript_len == 0) {
        ESP_LOGE(TAG, "transcript T encode failed");
        fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_pairing_derive_kconfirm(k_shared, pairing_epoch, k_confirm);
    feb_pairing_flipper_confirm(k_confirm, pairing_transcript_buf, pairing_transcript_len, expected_confirm);
    if (!feb_consttime_equal(expected_confirm, reply.confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN)) {
        ESP_LOGW(TAG, "flipper confirmation mismatch");
        fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_pairing_esp32_confirm(k_confirm, pairing_transcript_buf, pairing_transcript_len,
                              confirm_payload.confirmation);
    payload_len = feb_cbor_encode_pair_confirm_payload(pairing_payload_encode_buf,
                                                       sizeof(pairing_payload_encode_buf), &confirm_payload);
    if (payload_len == 0 ||
        !encode_and_queue_pairing_record(FEB_PAIR_CONFIRM_TYPE, strlen(FEB_PAIR_CONFIRM_TYPE),
                                         pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build pair_confirm");
        fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    pairing_state = PAIRING_STATE_CONFIRM_SENT;
    tx_done_action = TX_DONE_SEND_PAIR_COMPLETE;
    ESP_LOGI(TAG, "sending pair_confirm");
    send_next_tx_fragment(conn_handle);
}

static void finish_pairing_after_confirm(uint16_t conn_handle)
{
    uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN];
    feb_pair_complete_payload_t complete_payload;
    size_t payload_len;

    feb_pairing_derive_secret(k_shared, pairing_epoch, client_nonce, device_nonce,
                             board_id_buf, board_id_len, pairing_secret);

    if (!persist_pairing_secret(pairing_secret)) {
        ESP_LOGE(TAG, "failed to persist pairing_secret; aborting ceremony");
        feb_secure_zero(pairing_secret, sizeof(pairing_secret));
        fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_pairing_complete_tag(k_confirm, pairing_transcript_buf, pairing_transcript_len,
                             complete_payload.confirmation);
    feb_secure_zero(pairing_secret, sizeof(pairing_secret));

    payload_len = feb_cbor_encode_pair_complete_payload(pairing_payload_encode_buf,
                                                        sizeof(pairing_payload_encode_buf), &complete_payload);
    if (payload_len == 0 ||
        !encode_and_queue_pairing_record(FEB_PAIR_COMPLETE_TYPE, strlen(FEB_PAIR_COMPLETE_TYPE),
                                         pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build pair_complete after persisting secret");
        pairing_attempt_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    pairing_attempt_zeroize();
    pairing_state = PAIRING_STATE_COMPLETE_SENT;
    tx_done_action = TX_DONE_DISCONNECT;
    ESP_LOGI(TAG, "pairing_secret persisted; sending pair_complete");
    send_next_tx_fragment(conn_handle);
}

static void fail_runtime_auth(uint16_t conn_handle)
{
    if (runtime_auth_failure_count < 0xFFu) {
        runtime_auth_failure_count++;
    }
    hello_ack_start_ms = 0;
    pending_disconnect_reason = DISCONNECT_REASON_AUTH_FAILED;
    runtime_auth_zeroize();
    ESP_LOGW(TAG, "runtime auth failed (%u consecutive failure(s)); closing without reply",
             runtime_auth_failure_count);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

static void handle_runtime_auth_unknown_board(uint16_t conn_handle)
{
    hello_ack_start_ms = 0;
    pending_disconnect_reason = DISCONNECT_REASON_UNKNOWN_BOARD;
    runtime_auth_zeroize();
    ESP_LOGW(TAG, "flipper has no pairing record for board_id=%s; will open a pairing window "
                  "on the next connection attempt", board_id_buf);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

static void begin_runtime_auth(uint16_t conn_handle)
{
    feb_hello_payload_t hello_payload;
    size_t payload_len;

    esp_fill_random(rt_session_id, sizeof(rt_session_id));
    esp_fill_random(rt_client_nonce, sizeof(rt_client_nonce));
    memcpy(hello_payload.client_nonce, rt_client_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    payload_len = feb_cbor_encode_hello_payload(pairing_payload_encode_buf,
                                                sizeof(pairing_payload_encode_buf), &hello_payload);
    if (payload_len == 0 ||
        !encode_and_queue_session_record(FEB_HELLO_TYPE, strlen(FEB_HELLO_TYPE),
                                         pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build hello");
        runtime_auth_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    runtime_auth_state = RUNTIME_AUTH_STATE_HELLO_SENT;
    tx_done_action = TX_DONE_AWAIT_HELLO_ACK;
    hello_ack_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "sending hello");
    send_next_tx_fragment(conn_handle);
}

static void handle_hello_ack(uint16_t conn_handle, const feb_unencrypted_record_t *envelope)
{
    feb_hello_ack_payload_t ack;
    feb_cbor_status_t status;
    feb_session_transcript_t transcript = {0};
    uint8_t expected_proof[FEB_SESSION_PROOF_LEN];
    feb_client_auth_payload_t auth_payload;
    size_t payload_len;

    hello_ack_start_ms = 0;

    status = feb_cbor_decode_hello_ack_payload(envelope->payload_span, envelope->payload_span_len, &ack);
    if (status != FEB_CBOR_OK) {
        ESP_LOGW(TAG, "hello_ack payload decode failed: %d", (int)status);
        fail_runtime_auth(conn_handle);
        return;
    }

    memcpy(rt_device_nonce, ack.device_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    transcript.version = 2;
    transcript.board_id = board_id_buf;
    transcript.board_id_len = board_id_len;
    memcpy(transcript.session_id, rt_session_id, FEB_SESSION_ID_LEN);
    memcpy(transcript.client_nonce, rt_client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(transcript.device_nonce, rt_device_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    rt_transcript_len = feb_session_encode_transcript(rt_transcript_buf, sizeof(rt_transcript_buf), &transcript);
    if (rt_transcript_len == 0) {
        ESP_LOGE(TAG, "runtime transcript S encode failed");
        fail_runtime_auth(conn_handle);
        return;
    }

    feb_session_flipper_proof(stored_pairing_secret, rt_transcript_buf, rt_transcript_len, expected_proof);
    if (!feb_consttime_equal(expected_proof, ack.proof, FEB_SESSION_PROOF_LEN)) {
        ESP_LOGW(TAG, "flipper runtime proof mismatch");
        fail_runtime_auth(conn_handle);
        return;
    }

    runtime_auth_failure_count = 0;

    feb_session_derive_key(stored_pairing_secret, rt_client_nonce, rt_device_nonce,
                           board_id_buf, board_id_len, rt_session_id, rt_session_key);

    feb_session_esp32_proof(stored_pairing_secret, rt_transcript_buf, rt_transcript_len, auth_payload.proof);
    payload_len = feb_cbor_encode_client_auth_payload(pairing_payload_encode_buf,
                                                      sizeof(pairing_payload_encode_buf), &auth_payload);
    if (payload_len == 0 ||
        !encode_and_queue_session_record(FEB_CLIENT_AUTH_TYPE, strlen(FEB_CLIENT_AUTH_TYPE),
                                         pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build client_auth");
        runtime_auth_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    tx_done_action = TX_DONE_RUNTIME_AUTHENTICATED;
    ESP_LOGI(TAG, "sending client_auth");
    send_next_tx_fragment(conn_handle);
}

static int write_complete(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    tx_done_action_t action;
    bool pending_started;

    if (error->status != 0) {
        ESP_LOGE(TAG, "GATT write failed: %d", error->status);
        pairing_attempt_zeroize();
        runtime_auth_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    if (notify_cccd_handle != 0) {
        notify_cccd_handle = 0;
        if (boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH) {
            ESP_LOGI(TAG, "notifications subscribed; beginning runtime auth");
            begin_runtime_auth(conn_handle);
        } else {
            ESP_LOGI(TAG, "notifications subscribed; beginning pairing ceremony");
            begin_pairing(conn_handle);
        }
        return 0;
    }
    if (tx_fragment_next < tx_fragment_total) {
        send_next_tx_fragment(conn_handle);
        return 0;
    }

    last_record_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);

    action = tx_done_action;
    tx_done_action = TX_DONE_NONE;
    tx_dispatching_completion = true;
    switch (action) {
    case TX_DONE_AWAIT_PAIR_REPLY:
        ESP_LOGI(TAG, "pair_init sent; awaiting pair_reply");
        break;
    case TX_DONE_SEND_PAIR_COMPLETE:
        finish_pairing_after_confirm(conn_handle);
        break;
    case TX_DONE_DISCONNECT:
        ESP_LOGI(TAG, "pairing record sent; closing connection");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        break;
    case TX_DONE_AWAIT_HELLO_ACK:
        ESP_LOGI(TAG, "hello sent; awaiting hello_ack");
        break;
    case TX_DONE_RUNTIME_AUTHENTICATED:
        runtime_auth_state = RUNTIME_AUTH_STATE_AUTHENTICATED;
        rt_tx_sequence = 1;
        rt_rx_sequence = 1;
        ESP_LOGI(TAG, "client_auth sent; runtime session authenticated");
        feb_status_led_set(FEB_STATUS_LED_CONNECTED);
        /* docs/PROTOCOL.md "Unsolicited backlog drain": on every authenticated session
           establishment, if the flash log holds buffered records, start draining them now
           without waiting for a `command`. */
        wardriving_maybe_kick_send(conn_handle);
        break;
    case TX_DONE_CONTINUE_WIFI_SCAN:
        wifi_scan_send_next_batch(conn_handle);
        break;
    case TX_DONE_CONTINUE_BLE_SCAN:
        ble_scan_send_next_batch(conn_handle);
        break;
    case TX_DONE_CONTINUE_WARDRIVING:
        wardriving_send_next_batch(conn_handle);
        break;
    case TX_DONE_SEND_WARDRIVING_STOPPED: {
        feb_status_payload_t status_payload = {0};
        size_t payload_len;

        status_payload.request_id = 0;
        status_payload.state = "stopped";
        status_payload.state_len = strlen("stopped");
        payload_len = feb_cbor_encode_status_payload(pairing_payload_encode_buf,
                                                     sizeof(pairing_payload_encode_buf), &status_payload);
        if (payload_len == 0 ||
            !send_protected(conn_handle, "status", strlen("status"), pairing_payload_encode_buf, payload_len)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;
    }
    default:
        break;
    }
    tx_dispatching_completion = false;
    pending_started = start_next_pending_protected_send();
    if (!pending_started) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        scan_report_window_count++;
        scan_report_lifetime_total++;

        if (ble_scan_in_progress) {
            ble_scan_catalog_advertisement(&event->disc);
        }

        if (connection_handle == BLE_HS_CONN_HANDLE_NONE &&
            scan_record_matches(event->disc.data, event->disc.length_data)) {
            if (!connecting_permitted()) {
                ESP_LOGI(TAG, "pairing window closed; not connecting to discovered peer");
                ble_gap_disc_cancel();
                return 0;
            }
            ESP_LOGI(TAG, "found v2 peer, connecting");
            ble_gap_disc_cancel();
            rc = ble_gap_connect(own_addr_type, &event->disc.addr, 30000, NULL,
                                 gap_event, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "connect start failed: %d", rc);
                schedule_reconnect();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (connection_handle == BLE_HS_CONN_HANDLE_NONE && !reconnect_task_active) {
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed: %d", event->connect.status);
            schedule_reconnect();
            return 0;
        }
        connection_handle = event->connect.conn_handle;
        reconnect_retries = 0;
        service_start_handle = 0;
        service_end_handle = 0;
        write_value_handle = 0;
        notify_value_handle = 0;
        notify_cccd_handle = 0;
        negotiated_att_mtu = 23;
        pairing_state = PAIRING_STATE_IDLE;
        runtime_auth_state = RUNTIME_AUTH_STATE_IDLE;
        feb_status_led_set(FEB_STATUS_LED_CONNECTING);
        pending_disconnect_reason = DISCONNECT_REASON_NORMAL;
        hello_ack_start_ms = 0;
        pair_reply_wait_start_ms = 0;
        last_record_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
        rt_tx_sequence = 0;
        rt_rx_sequence = 0;
        tx_done_action = TX_DONE_NONE;
        tx_fragment_total = 0;
        tx_fragment_next = 0;
        pending_protected_tx_head = 0;
        pending_protected_tx_count = 0;
        tx_dispatching_completion = false;
        feb_reassembly_reset(&rx_reassembly);
        wardriving_tx_in_flight = false; /* per-connection only -- wardriving_{wifi,ble}_active
                                             deliberately persist across connect/disconnect */
        wardriving_pending_drain_count = 0;
        ESP_LOGI(TAG, "connected; exchanging MTU");
        rc = ble_gattc_exchange_mtu(connection_handle, mtu_exchanged, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "MTU exchange start failed: %d", rc);
            mtu_exchanged(connection_handle, &(struct ble_gatt_error){0}, 23, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        disconnect_reason_t reason = pending_disconnect_reason;

        ESP_LOGW(TAG, "disconnected: reason=%d", event->disconnect.reason);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        service_start_handle = 0;
        service_end_handle = 0;
        write_value_handle = 0;
        notify_value_handle = 0;
        notify_cccd_handle = 0;
        pairing_state = PAIRING_STATE_IDLE;
        runtime_auth_state = RUNTIME_AUTH_STATE_IDLE;
        feb_status_led_set(FEB_STATUS_LED_CONNECTING);
        hello_ack_start_ms = 0;
        pair_reply_wait_start_ms = 0;
        pending_disconnect_reason = DISCONNECT_REASON_NORMAL;
        tx_done_action = TX_DONE_NONE;
        tx_fragment_total = 0;
        tx_fragment_next = 0;
        pending_protected_tx_head = 0;
        pending_protected_tx_count = 0;
        tx_dispatching_completion = false;
        feb_reassembly_reset(&rx_reassembly);
        pairing_attempt_zeroize();
        runtime_auth_zeroize();
        wardriving_tx_in_flight = false;

        if (wifi_scan_in_progress && wifi_scan_active_source == WIFI_SCAN_SOURCE_MANUAL) {
            /* Don't clear wifi_scan_in_progress directly here -- the radio scan this
               connection started may still be running, and a new connection's `command`
               could otherwise race a still-in-flight wifi_scan_done_handler() write to
               wifi_scan_raw_records/wifi_scan_selected from a stale scan.
               esp_wifi_scan_stop() still fires WIFI_EVENT_SCAN_DONE for the aborted scan;
               wifi_scan_done_cb() then finds no authenticated connection and clears the
               flag there, uniformly. Gated to the manual source only -- wardriving's Wi-Fi
               capture (if active) must keep running across this disconnect
               (docs/PROTOCOL.md: "continues across BLE disconnects"), so wifi_scan_done_cb()'s
               WARDRIVING branch does not check connection state at all. */
            esp_err_t serr = esp_wifi_scan_stop();

            if (serr != ESP_OK && serr != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_scan_stop failed during disconnect cleanup: %s",
                         esp_err_to_name(serr));
            }
            ESP_LOGI(TAG, "wifi_scan was in progress at disconnect; stopping it "
                          "(pending results will be discarded)");
        }

        if (ble_scan_in_progress && ble_scan_active_source == BLE_SCAN_SOURCE_MANUAL) {
            /* Same disconnect-safety shape as wifi_scan_in_progress above, and gated to the
               manual source for the same reason (wardriving's BLE capture must survive this
               disconnect): stop the radio scan immediately, but don't clear
               ble_scan_in_progress here -- the already-armed ble_scan_done_co window-close
               callout will observe "no authenticated connection" when it fires and clear the
               flag there, uniformly. */
            int derr = ble_gap_disc_cancel();

            if (derr != 0 && derr != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "ble_gap_disc_cancel failed during disconnect cleanup: %d", derr);
            }
            ESP_LOGI(TAG, "ble_scan was in progress at disconnect; stopping discovery "
                          "(pending results will be discarded)");
        }

        if (boot_mode == FEB_BOOT_MODE_PAIRING) {
            pairing_window_closed = true;
            ESP_LOGI(TAG, "pairing attempt consumed; staying idle until next reset");
            return 0;
        }

        switch (reason) {
        case DISCONNECT_REASON_UNKNOWN_BOARD:
            boot_mode = FEB_BOOT_MODE_PAIRING;
            esp_fill_random(pairing_epoch, sizeof(pairing_epoch));
            pairing_window_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            pairing_window_closed = false;
            reconnect_retries = 0;
            ESP_LOGI(TAG, "falling back to pairing window on next connection attempt "
                          "(board_id=%s unknown to flipper)", board_id_buf);
            start_scan();
            break;
        case DISCONNECT_REASON_AUTH_FAILED:
            schedule_runtime_auth_backoff();
            break;
        case DISCONNECT_REASON_NORMAL:
        default:
            start_scan();
            break;
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t buffer[FEB_RX_FRAGMENT_BUFFER_SIZE];
        const uint8_t *record;
        size_t record_len;
        feb_frame_status_t status;

        if (event->notify_rx.attr_handle != notify_value_handle) {
            return 0;
        }
        if (length > sizeof(buffer)) {
            ESP_LOGW(TAG, "notification fragment too large: %u bytes", length);
            return 0;
        }
        if (os_mbuf_copydata(event->notify_rx.om, 0, length, buffer) != 0) {
            ESP_LOGW(TAG, "notification copy failed");
            return 0;
        }

        status = feb_reassembly_feed(&rx_reassembly, buffer, length,
                                     (uint32_t)(esp_timer_get_time() / 1000), &record, &record_len);
        switch (status) {
        case FEB_FRAME_OK:
            ESP_LOGI(TAG, "reassembly fragment accepted: %u bytes", length);
            break;
        case FEB_FRAME_MESSAGE_COMPLETE: {
            last_record_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (boot_mode == FEB_BOOT_MODE_PAIRING) {
                feb_pairing_envelope_t envelope;
                feb_cbor_status_t decode_status;

                if (pairing_state != PAIRING_STATE_INIT_SENT) {
                    ESP_LOGW(TAG, "unexpected pairing record received in state %d", (int)pairing_state);
                    fail_pairing_ceremony(connection_handle, FEB_PAIRING_ERR_FAILED);
                    break;
                }
                decode_status = feb_cbor_decode_pairing_envelope(record, record_len, &envelope);
                if (decode_status != FEB_CBOR_OK) {
                    ESP_LOGW(TAG, "pairing envelope decode failed: %d", (int)decode_status);
                    fail_pairing_ceremony(connection_handle, FEB_PAIRING_ERR_FAILED);
                    break;
                }
                if (envelope.version != 2 || envelope.board_id_len != board_id_len ||
                    memcmp(envelope.board_id, board_id_buf, board_id_len) != 0) {
                    ESP_LOGW(TAG, "pairing envelope version/board_id mismatch");
                    fail_pairing_ceremony(connection_handle, FEB_PAIRING_ERR_FAILED);
                    break;
                }
                if (envelope.type_len == strlen(FEB_PAIR_REPLY_TYPE) &&
                    memcmp(envelope.type, FEB_PAIR_REPLY_TYPE, envelope.type_len) == 0) {
                    handle_pair_reply(connection_handle, &envelope);
                } else if (envelope.type_len == strlen("error") &&
                          memcmp(envelope.type, "error", envelope.type_len) == 0) {
                    feb_error_payload_t peer_error;

                    if (feb_cbor_decode_error_payload(envelope.payload_span, envelope.payload_span_len,
                                                      &peer_error) == FEB_CBOR_OK) {
                        ESP_LOGW(TAG, "peer reported pairing error: %.*s",
                                 (int)peer_error.code_len, peer_error.code);
                    } else {
                        ESP_LOGW(TAG, "peer reported a pairing error (undecodable payload)");
                    }
                    pairing_attempt_zeroize();
                    ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                } else {
                    ESP_LOGW(TAG, "expected pair_reply, got type=%.*s",
                             (int)envelope.type_len, envelope.type);
                    fail_pairing_ceremony(connection_handle, FEB_PAIRING_ERR_FAILED);
                }
                break;
            }

            /* boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH */
            if (runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED) {
                feb_session_decrypted_record_t decrypted;
                feb_cbor_status_t decode_status;

                decode_status = feb_session_decrypt_record(rt_session_key, record, record_len,
                                                           FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
                                                           rt_plaintext_buf, sizeof(rt_plaintext_buf),
                                                           &decrypted);
                if (decode_status != FEB_CBOR_OK) {
                    ESP_LOGW(TAG, "protected record decode/decrypt failed: %d; closing without reply",
                             (int)decode_status);
                    ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                if (decrypted.version != 2 ||
                    memcmp(decrypted.session_id, rt_session_id, FEB_SESSION_ID_LEN) != 0 ||
                    decrypted.board_id_len != board_id_len ||
                    memcmp(decrypted.board_id, board_id_buf, board_id_len) != 0 ||
                    decrypted.sequence != rt_rx_sequence ||
                    decrypted.sequence >= FEB_SESSION_SEQUENCE_MAX) {
                    ESP_LOGW(TAG, "protected record session/sequence mismatch; closing without reply");
                    ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                rt_rx_sequence++;

                if (decrypted.type_len == strlen("capability_query") &&
                    memcmp(decrypted.type, "capability_query", decrypted.type_len) == 0) {
                    feb_capability_query_payload_t query;

                    if (feb_cbor_decode_capability_query_payload(decrypted.plaintext, decrypted.plaintext_len,
                                                                  &query) != FEB_CBOR_OK) {
                        ESP_LOGW(TAG, "capability_query payload decode failed; closing without reply");
                        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                        break;
                    }
                    handle_capability_query(connection_handle);
                } else if (decrypted.type_len == strlen("command") &&
                          memcmp(decrypted.type, "command", decrypted.type_len) == 0) {
                    feb_command_payload_t cmd;

                    if (feb_cbor_decode_command_payload(decrypted.plaintext, decrypted.plaintext_len,
                                                        &cmd) != FEB_CBOR_OK) {
                        ESP_LOGW(TAG, "command payload decode failed; closing without reply");
                        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                        break;
                    }
                    handle_command(connection_handle, &cmd);
                } else {
                    ESP_LOGW(TAG, "unrecognized protected record type=%.*s while authenticated",
                             (int)decrypted.type_len, decrypted.type);
                }
                break;
            }
            if (runtime_auth_state != RUNTIME_AUTH_STATE_HELLO_SENT) {
                ESP_LOGW(TAG, "unexpected runtime record received in state %d", (int)runtime_auth_state);
                fail_runtime_auth(connection_handle);
                break;
            }
            {
                feb_unencrypted_record_t envelope;
                feb_cbor_status_t decode_status;

                decode_status = feb_cbor_decode_unencrypted(record, record_len, &envelope);
                if (decode_status == FEB_CBOR_OK && envelope.version == 2 &&
                    memcmp(envelope.session_id, rt_session_id, FEB_SESSION_ID_LEN) == 0 &&
                    envelope.board_id_len == board_id_len &&
                    memcmp(envelope.board_id, board_id_buf, board_id_len) == 0) {
                    if (envelope.type_len == strlen(FEB_HELLO_ACK_TYPE) &&
                        memcmp(envelope.type, FEB_HELLO_ACK_TYPE, envelope.type_len) == 0) {
                        handle_hello_ack(connection_handle, &envelope);
                    } else {
                        ESP_LOGW(TAG, "expected hello_ack, got type=%.*s",
                                 (int)envelope.type_len, envelope.type);
                        fail_runtime_auth(connection_handle);
                    }
                    break;
                }

                {
                    feb_pairing_envelope_t perr_env;
                    feb_cbor_status_t perr_status =
                        feb_cbor_decode_pairing_envelope(record, record_len, &perr_env);

                    if (perr_status == FEB_CBOR_OK && perr_env.version == 2 &&
                        perr_env.board_id_len == board_id_len &&
                        memcmp(perr_env.board_id, board_id_buf, board_id_len) == 0 &&
                        perr_env.type_len == strlen("error") &&
                        memcmp(perr_env.type, "error", perr_env.type_len) == 0) {
                        feb_error_payload_t perr_payload;

                        if (feb_cbor_decode_error_payload(perr_env.payload_span, perr_env.payload_span_len,
                                                          &perr_payload) == FEB_CBOR_OK &&
                            perr_payload.code_len == strlen("unknown_board") &&
                            memcmp(perr_payload.code, "unknown_board", perr_payload.code_len) == 0) {
                            handle_runtime_auth_unknown_board(connection_handle);
                            break;
                        }
                    }
                }

                ESP_LOGW(TAG, "unrecognized record while awaiting hello_ack");
                fail_runtime_auth(connection_handle);
            }
            break;
        }
        case FEB_FRAME_DUPLICATE_FRAGMENT:
            ESP_LOGW(TAG, "reassembly rejected: duplicate fragment");
            break;
        case FEB_FRAME_INCONSISTENT_COUNT:
            ESP_LOGW(TAG, "reassembly rejected: inconsistent fragment count");
            break;
        case FEB_FRAME_OVERSIZED:
            ESP_LOGW(TAG, "reassembly rejected: oversized message");
            break;
        case FEB_FRAME_OUT_OF_ORDER:
            ESP_LOGW(TAG, "reassembly rejected: out-of-order fragment");
            break;
        case FEB_FRAME_INVALID_HEADER:
            ESP_LOGW(TAG, "reassembly rejected: invalid fragment header");
            break;
        default:
            ESP_LOGW(TAG, "reassembly rejected: unexpected status %d", (int)status);
            break;
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void reassembly_timeout_cb(struct ble_npl_event *ev)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    (void)ev;
    feb_status_led_tick();
    if (feb_reassembly_check_timeout(&rx_reassembly, now_ms) == FEB_FRAME_TIMEOUT) {
        ESP_LOGW(TAG, "rx reassembly timed out; discarding partial message");
    }
    if (boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH &&
        runtime_auth_state == RUNTIME_AUTH_STATE_HELLO_SENT &&
        hello_ack_start_ms != 0 &&
        (uint32_t)(now_ms - hello_ack_start_ms) >= FEB_HELLO_ACK_TIMEOUT_MS &&
        connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "timed out waiting for hello_ack");
        fail_runtime_auth(connection_handle);
    }
    if (pairing_state == PAIRING_STATE_INIT_SENT &&
        pair_reply_wait_start_ms != 0 &&
        (uint32_t)(now_ms - pair_reply_wait_start_ms) >= FEB_PAIR_REPLY_TIMEOUT_MS &&
        connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "timed out waiting for pair_reply");
        pair_reply_wait_start_ms = 0;
        fail_pairing_ceremony(connection_handle, FEB_PAIRING_ERR_EXPIRED);
    }
    if (connection_handle != BLE_HS_CONN_HANDLE_NONE &&
        runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED &&
        (uint32_t)(now_ms - last_record_activity_ms) >= FEB_IDLE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "idle authenticated connection (%lu ms without a record); terminating",
                 (unsigned long)(now_ms - last_record_activity_ms));
        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        last_record_activity_ms = now_ms;
    }
    scan_summary_elapsed_ms += FEB_REASSEMBLY_CHECK_INTERVAL_MS;
    if (scan_summary_elapsed_ms >= FEB_SCAN_SUMMARY_INTERVAL_MS) {
        scan_summary_elapsed_ms = 0;
        if (scan_report_window_count > 0) {
            ESP_LOGI(TAG, "scan reports: %lu in last %lus (lifetime %lu)",
                     (unsigned long)scan_report_window_count,
                     (unsigned long)(FEB_SCAN_SUMMARY_INTERVAL_MS / 1000u),
                     (unsigned long)scan_report_lifetime_total);
            scan_report_window_count = 0;
        }
    }
    ble_npl_callout_reset(&reassembly_timeout_co,
                          ble_npl_time_ms_to_ticks32(FEB_REASSEMBLY_CHECK_INTERVAL_MS));
}

/* docs/PROTOCOL.md "`wifi_scan` command and status payloads": esp_netif/default event
   loop/esp_wifi initialize once at boot, STA mode, never connecting to anything, and stay
   resident for the device's whole lifetime. Not lazy-initialized on first wifi_scan command.
   Ported unchanged from esp32/main/main.c -- see this file's top-of-file comment and
   docs/BASELINES.md/.claude/agents/heltec-developer.md for this board's untested-versus-
   validated coexistence status: unlike the C6 (whose Wi-Fi 6 + BLE 5 + 802.15.4 radio had
   its concurrent-scan behavior validated in an earlier step), this board's Wi-Fi 4 + BT
   Classic/BLE 4.2 combo radio has never been exercised running a Wi-Fi scan concurrently
   with an active BLE connection to the Flipper -- that is a real, currently-untested gap for
   this port, not a borrowed-and-verified number. */
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
                                              &wifi_scan_done_handler, NULL, NULL);
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
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);

    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        return;
    }
    ble_npl_callout_init(&reassembly_timeout_co, nimble_port_get_dflt_eventq(),
                        reassembly_timeout_cb, NULL);
    ble_npl_callout_reset(&reassembly_timeout_co,
                          ble_npl_time_ms_to_ticks32(FEB_REASSEMBLY_CHECK_INTERVAL_MS));
    ble_npl_callout_init(&wifi_scan_done_co, nimble_port_get_dflt_eventq(), wifi_scan_done_cb, NULL);
    ble_npl_callout_init(&ble_scan_done_co, nimble_port_get_dflt_eventq(), ble_scan_window_close_cb, NULL);
    ble_npl_callout_init(&wardriving_wifi_interval_co, nimble_port_get_dflt_eventq(),
                        wardriving_wifi_interval_cb, NULL);
    ble_npl_callout_init(&wardriving_ble_interval_co, nimble_port_get_dflt_eventq(),
                        wardriving_ble_interval_cb, NULL);
    ble_npl_callout_init(&reconnect_co, nimble_port_get_dflt_eventq(), reconnect_timer_cb, NULL);
    /* A raw event, not a 7th callout -- see wardriving_button_toggle_ev's comment and
       esp32/main/main.c's fuller one on the same mechanism. */
    ble_npl_event_init(&wardriving_button_toggle_ev, wardriving_button_toggle_cb, NULL);

    /* docs/BACKLOG.md "Per-board wardriving autostart setting", ported unchanged from
       esp32/main/main.c -- see that file's fuller comment (resume before start_scan() below,
       guarded against a later NimBLE resync re-attempting it). */
    if (!wardriving_autostart_attempted) {
        wardriving_autostart_attempted = true;
        if (wardriving_persisted.enabled) {
            if (!wardriving_start_internal(wardriving_persisted.want_wifi, wardriving_persisted.want_ble,
                                           wardriving_persisted.want_ble_passive,
                                           wardriving_persisted.wifi_interval_ms,
                                           wardriving_persisted.ble_window_ms,
                                           wardriving_persisted.ble_interval_ms,
                                           WARDRIVING_SWELLING_NORMAL, WARDRIVING_COUNTRY_ROW)) {
                ESP_LOGW(TAG, "wardriving autostart failed; leaving stopped");
            } else {
                ESP_LOGI(TAG, "wardriving autostarted from persisted state (wifi=%d ble=%d ble_passive=%d)",
                         (int)wardriving_persisted.want_wifi, (int)wardriving_persisted.want_ble,
                         (int)wardriving_persisted.want_ble_passive);
            }
        }
    }
    wardriving_control_ready = true;

    ESP_LOGI(TAG, "starting v2 service-filtered scan");
    start_scan();
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
    wardriving_persist_load(&wardriving_persisted);

    compute_board_id();
    if (load_pairing_secret(stored_pairing_secret)) {
        boot_mode = FEB_BOOT_MODE_RUNTIME_AUTH;
        ESP_LOGI(TAG, "board_id=%s: stored pairing_secret found; attempting runtime auth "
                      "(no pairing window opened)", board_id_buf);
    } else {
        boot_mode = FEB_BOOT_MODE_PAIRING;
        esp_fill_random(pairing_epoch, sizeof(pairing_epoch));
        pairing_window_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "board_id=%s: no stored pairing_secret; pairing window open for %u ms",
                 board_id_buf, (unsigned)FEB_PAIRING_WINDOW_MS);
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

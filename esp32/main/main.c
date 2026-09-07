#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
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
#include "pairing.h"
#include "pairing_crypto.h"
#include "session.h"
#include "session_crypto.h"

static const char *TAG = "flipper_esp32_over_ble";

#define MAX_RECONNECT_RETRIES 5
#define FEB_RX_FRAGMENT_BUFFER_SIZE 256u
/* ceil(FEB_MAX_RECORD_SIZE / feb_fragment_capacity(FEB_FLIPPER_WRITE_EFFECTIVE_MTU)) =
   ceil(768/60) = 13. Assumes ATT MTU negotiation succeeds to >= FEB_FLIPPER_WRITE_EFFECTIVE_MTU
   (67), per encode_and_queue_pairing_record()'s MTU clamp below. If MTU exchange never
   completes and negotiated_att_mtu stays at the pre-negotiation default of 23, capacity
   drops to feb_fragment_capacity(23) = 16 and a near-max-size record could need up to 48
   fragments -- an accepted, currently-unhandled gap on that fallback path. */
#define FEB_TX_MAX_FRAGMENTS 13u
#define FEB_REASSEMBLY_CHECK_INTERVAL_MS (FEB_REASSEMBLY_TIMEOUT_MS / 2)
/* Cadence for the aggregate BLE_GAP_EVENT_DISC summary log emitted from
   reassembly_timeout_cb() -- piggybacks on that existing callout rather than a second timer. */
#define FEB_SCAN_SUMMARY_INTERVAL_MS 10000u
#define FEB_PAIRING_NVS_NAMESPACE "feb_pairing"
#define FEB_PAIRING_NVS_KEY "secret"

/* docs/PLAN.md step 6: how long the ESP32 waits for hello_ack after sending hello before
   treating it as a proof-verification-class failure (docs/PROTOCOL.md doesn't pin a number;
   a couple of small BLE round trips over an already-established connection are realistically
   sub-second, so 5s is generous margin, not a tight bound). */
#define FEB_HELLO_ACK_TIMEOUT_MS 5000u
/* Judgment call (docs/PLAN.md step 6 leaves the exact shape open): repeated runtime-auth
   proof failures back off exponentially (1,2,4,...32s) for the first
   FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP attempts, then fall back to a fixed slow cadence
   indefinitely -- mirroring step 2's "revised long-run reconnect policy" bounded-then-slow
   shape, kept as an independent counter/backoff from the GAP-level reconnect_retries below
   since a proof failure can recur even when the physical link connects cleanly every time. */
#define FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP 6u
#define FEB_RUNTIME_AUTH_SLOW_CADENCE_MS (5u * 60u * 1000u)
/* docs/PLAN.md backlog fix (2026-09-06): GAP-level connect() failures get the same
   exponential-then-flatten shape as runtime auth above, but with an independent, much
   shorter flat cadence -- the auth path's 5-minute cadence is doing double duty as an
   anti-hammering throttle against repeated bad credentials, which doesn't apply to a plain
   link-layer connect failure (the peer could be back and connectable within seconds), and
   reusing 5 minutes here would reintroduce a "goes quiet for no reason" symptom on this
   path similar to the one the scan-stall fix just eliminated on a different one. */
#define FEB_RECONNECT_SLOW_CADENCE_MS (30u * 1000u)

/* docs/PROTOCOL.md "Reliability and reconnect behavior": "A peer closes an idle
   authenticated connection after 30 seconds without a record." The Flipper FAP's exit
   path doesn't always send a disconnect PDU (bt_profile_restore_default() restarts its
   BLE coprocessor instead), so without this the ESP32 central can be stuck "connected"
   until NimBLE's own long connection-supervision timeout elapses -- this closes that
   gap proactively. Only enforced once runtime_auth_state is AUTHENTICATED, matching the
   spec text ("idle authenticated connection"). */
#define FEB_IDLE_TIMEOUT_MS 30000u

/* The Flipper's Write characteristic (flipper_esp32_over_ble.c PAYLOAD_MAX) has a fixed
   max GATT attribute value length of 64 bytes, set once at registration and independent of
   the negotiated ATT MTU. Writes to it must never exceed this length, however large the
   negotiated MTU is. Modeling it as an "MTU" of length+ATT_WRITE_OVERHEAD lets us reuse
   feb_fragment_capacity() unchanged: feb_fragment_capacity(64 + 3) = 64 - 4 = 60 payload
   bytes/fragment, and clamping the real negotiated MTU down to this before calling
   feb_fragment_capacity() keeps the smaller value when the real MTU is even tinier (e.g.
   the pre-negotiation default of 23). */
#define FEB_FLIPPER_WRITE_CHAR_MAX_LEN 64u
#define FEB_FLIPPER_WRITE_EFFECTIVE_MTU (FEB_FLIPPER_WRITE_CHAR_MAX_LEN + FEB_ATT_WRITE_OVERHEAD)

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

static char board_id_buf[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
static size_t board_id_len;
static uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN];
static uint32_t pairing_window_deadline_ms;
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
} tx_done_action_t;
static tx_done_action_t tx_done_action = TX_DONE_NONE;

/* docs/PLAN.md step 6: reset no longer unconditionally opens a pairing window. A stored
   pairing_secret found at boot means runtime auth is attempted first; the board only falls
   back to FEB_BOOT_MODE_PAIRING if the Flipper rejects it with unknown_board (see
   BLE_GAP_EVENT_DISCONNECT below), not on this same connection. */
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

/* Set by fail_runtime_auth()/handle_runtime_auth_unknown_board() just before
   ble_gap_terminate(), read (and reset to NORMAL) by BLE_GAP_EVENT_DISCONNECT to decide
   which of the three docs/PLAN.md step 6 outcomes applies. */
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
static uint32_t hello_ack_deadline_ms; /* 0 = no deadline currently active */
static uint32_t last_record_activity_ms; /* reset on connect and on each record received */

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

static void compute_board_id(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    int written;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read factory MAC: %s", esp_err_to_name(err));
        memcpy(board_id_buf, "esp32c6-unknown", sizeof("esp32c6-unknown"));
        board_id_len = strlen(board_id_buf);
        return;
    }
    written = snprintf(board_id_buf, sizeof(board_id_buf), "esp32c6-%02x%02x%02x%02x%02x%02x",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    board_id_len = (written > 0) ? (size_t)written : 0;
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
    if (now_ms >= pairing_window_deadline_ms) {
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

/* session_key is the only value session.h marks ephemeral for this state machine
   (client_nonce/device_nonce/session_id are not secret -- docs/PROTOCOL.md). */
static void runtime_auth_zeroize(void)
{
    feb_secure_zero(rt_session_key, sizeof(rt_session_key));
}

static bool connecting_permitted(void)
{
    if (boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH) {
        /* No window in this mode -- retries indefinitely per docs/PLAN.md step 6. */
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
    if (!connecting_permitted()) {
        ESP_LOGI(TAG, "pairing window closed; staying idle");
        return;
    }

    params.passive = 0;
    /* Controller-side dup filtering is keyed on address only (CONFIG_BT_LE_SCAN_DUPL_TYPE_DEVICE)
       and never expires (CONFIG_BT_LE_SCAN_DUPL_CACHE_REFRESH_PERIOD=0), so once the peer's MAC
       has been cached against a non-matching advert (e.g. the Flipper FAP exiting), a later
       matching advert from the same MAC gets silently dropped -- root cause of the scan-stall
       in docs/SESSION_MEMORY.md. Filtering happens in scan_record_matches() instead. */
    params.filter_duplicates = 0;
    params.itvl = 0;
    params.window = 0;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
    }
}

static void reconnect_task(void *arg)
{
    TickType_t delay = (TickType_t)(uintptr_t)arg;

    vTaskDelay(delay);
    reconnect_task_active = false;
    start_scan();
    vTaskDelete(NULL);
}

/* Mirrors runtime_auth_backoff_delay_ms()'s exponential-then-flatten shape, but with its
   own independent flat cadence (FEB_RECONNECT_SLOW_CADENCE_MS's comment explains why it's
   much shorter). reconnect_retries is only ever nonzero when this runs, since
   schedule_reconnect() increments it before calling this -- no need for a "== 0" branch. */
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
    if (xTaskCreate(reconnect_task, "ble_reconnect", 3072,
                    (void *)(uintptr_t)pdMS_TO_TICKS(delay_ms), 4, NULL) != pdPASS) {
        reconnect_task_active = false;
        ESP_LOGE(TAG, "could not schedule reconnect");
    }
}

/* Independent of reconnect_retries/MAX_RECONNECT_RETRIES above (which now means "length of
   the exponential ramp before flattening," not a hard cap -- reconnect_backoff_delay_ms()
   governs GAP-level connect() failures): a runtime-auth proof failure can recur even when
   the physical link connects cleanly every time, and per docs/PLAN.md step 6 must never
   stop retrying outright, only slow down -- see FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP's comment. */
static void schedule_runtime_auth_backoff(void)
{
    uint32_t delay_ms = runtime_auth_backoff_delay_ms();

    if (reconnect_task_active) {
        return;
    }
    reconnect_task_active = true;
    ESP_LOGI(TAG, "runtime auth backoff: retry in %lu ms (consecutive failures=%u)",
             (unsigned long)delay_ms, runtime_auth_failure_count);
    if (xTaskCreate(reconnect_task, "ble_reconnect", 3072,
                    (void *)(uintptr_t)pdMS_TO_TICKS(delay_ms), 4, NULL) != pdPASS) {
        reconnect_task_active = false;
        ESP_LOGE(TAG, "could not schedule runtime-auth backoff reconnect");
    }
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
        ESP_LOGE(TAG, "pairing fragment %u write failed: %d", tx_fragment_next, rc);
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

/* docs/PLAN.md step 6: unencrypted hello/hello_ack/client_auth envelope, distinct from the
   pairing-phase envelope above -- this one carries the runtime session_id. */
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
    hello_ack_deadline_ms = 0;
    pending_disconnect_reason = DISCONNECT_REASON_AUTH_FAILED;
    runtime_auth_zeroize();
    ESP_LOGW(TAG, "runtime auth failed (%u consecutive failure(s)); closing without reply",
             runtime_auth_failure_count);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

static void handle_runtime_auth_unknown_board(uint16_t conn_handle)
{
    hello_ack_deadline_ms = 0;
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
    hello_ack_deadline_ms = (uint32_t)(esp_timer_get_time() / 1000) + FEB_HELLO_ACK_TIMEOUT_MS;
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

    hello_ack_deadline_ms = 0;

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

    /* Proof verified: this attempt succeeded, so the failure-rate-limit resets. */
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

    action = tx_done_action;
    tx_done_action = TX_DONE_NONE;
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
        ESP_LOGI(TAG, "client_auth sent; runtime session authenticated");
        break;
    default:
        break;
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
        pending_disconnect_reason = DISCONNECT_REASON_NORMAL;
        hello_ack_deadline_ms = 0;
        last_record_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
        tx_done_action = TX_DONE_NONE;
        tx_fragment_total = 0;
        tx_fragment_next = 0;
        feb_reassembly_reset(&rx_reassembly);
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
        hello_ack_deadline_ms = 0;
        pending_disconnect_reason = DISCONNECT_REASON_NORMAL;
        tx_done_action = TX_DONE_NONE;
        tx_fragment_total = 0;
        tx_fragment_next = 0;
        feb_reassembly_reset(&rx_reassembly);
        pairing_attempt_zeroize();
        runtime_auth_zeroize();

        if (boot_mode == FEB_BOOT_MODE_PAIRING) {
            /* Any established connection's outcome consumes the one-shot pairing
               attempt (docs/PAIRING.md); go idle until the next physical reset. */
            pairing_window_closed = true;
            ESP_LOGI(TAG, "pairing attempt consumed; staying idle until next reset");
            return 0;
        }

        /* boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH: docs/PLAN.md step 6's three outcomes. */
        switch (reason) {
        case DISCONNECT_REASON_UNKNOWN_BOARD:
            boot_mode = FEB_BOOT_MODE_PAIRING;
            esp_fill_random(pairing_epoch, sizeof(pairing_epoch));
            pairing_window_deadline_ms = (uint32_t)(esp_timer_get_time() / 1000) + FEB_PAIRING_WINDOW_MS;
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
            /* Plain link loss / idle timeout / graceful close -- reconnect promptly,
               no rate-limit penalty (docs/PLAN.md step 6). */
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

                /* Not a valid session-envelope hello_ack -- the only other legitimate
                   reply at this point is an unknown_board error using the pairing-phase
                   envelope (no session_id exists from the Flipper's perspective yet), per
                   docs/PROTOCOL.md's "Runtime auth failure handling". */
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
    if (feb_reassembly_check_timeout(&rx_reassembly, now_ms) == FEB_FRAME_TIMEOUT) {
        ESP_LOGW(TAG, "rx reassembly timed out; discarding partial message");
    }
    if (boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH &&
        runtime_auth_state == RUNTIME_AUTH_STATE_HELLO_SENT &&
        hello_ack_deadline_ms != 0 && now_ms >= hello_ack_deadline_ms &&
        connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "timed out waiting for hello_ack");
        fail_runtime_auth(connection_handle);
    }
    if (connection_handle != BLE_HS_CONN_HANDLE_NONE &&
        runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED &&
        (uint32_t)(now_ms - last_record_activity_ms) >= FEB_IDLE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "idle authenticated connection (%lu ms without a record); terminating",
                 (unsigned long)(now_ms - last_record_activity_ms));
        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        /* Avoid re-firing every check interval while BLE_GAP_EVENT_DISCONNECT is still
           pending -- gives a full FEB_IDLE_TIMEOUT_MS of grace before a repeat call. */
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

    feb_factory_reset_start();

    compute_board_id();
    if (load_pairing_secret(stored_pairing_secret)) {
        boot_mode = FEB_BOOT_MODE_RUNTIME_AUTH;
        ESP_LOGI(TAG, "board_id=%s: stored pairing_secret found; attempting runtime auth "
                      "(no pairing window opened)", board_id_buf);
    } else {
        boot_mode = FEB_BOOT_MODE_PAIRING;
        esp_fill_random(pairing_epoch, sizeof(pairing_epoch));
        pairing_window_deadline_ms = (uint32_t)(esp_timer_get_time() / 1000) + FEB_PAIRING_WINDOW_MS;
        ESP_LOGI(TAG, "board_id=%s: no stored pairing_secret; pairing window open for %u ms",
                 board_id_buf, (unsigned)FEB_PAIRING_WINDOW_MS);
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = host_synced;
    nimble_port_freertos_init(nimble_host_task);
}

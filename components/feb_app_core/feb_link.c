#include "feb_app_internal.h"

static const feb_app_hooks_t feb_no_hooks = {0};
const feb_app_hooks_t *feb_app_hooks = &feb_no_hooks;

void feb_app_core_init(const feb_app_hooks_t *hooks)
{
    feb_app_hooks = (hooks != NULL) ? hooks : &feb_no_hooks;
}

/* ceil(FEB_MAX_RECORD_SIZE / feb_fragment_capacity(FEB_FLIPPER_WRITE_EFFECTIVE_MTU)) =
   ceil(768/240) = 4 at the current 244-byte FEB_WRITE_CHAR_MAX_LEN (was ceil(768/60) = 13
   at the old 64-byte cap). Left at 13, its original worst-case value: over-provisioned
   headroom, never a shortfall, so no resize is needed when the per-fragment capacity only
   grows. Assumes ATT MTU negotiation succeeds to >= FEB_FLIPPER_WRITE_EFFECTIVE_MTU (247),
   per encode_and_queue_pairing_record()'s MTU clamp below. If MTU exchange never completes
   and feb_negotiated_att_mtu stays at the pre-negotiation default of 23, capacity drops to
   feb_fragment_capacity(23) = 16 and a near-max-size record could need up to 48 fragments --
   an accepted, currently-unhandled gap on that fallback path. */
#define FEB_TX_MAX_FRAGMENTS 13u
#define FEB_PAIRING_NVS_NAMESPACE "feb_pairing"
#define FEB_PAIRING_NVS_KEY "secret"
/* Judgment call (docs/PLAN.md step 6 leaves the exact shape open): repeated runtime-auth
   proof failures back off exponentially (1,2,4,...32s) for the first
   FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP attempts, then fall back to a fixed slow cadence
   indefinitely -- mirroring step 2's "revised long-run reconnect policy" bounded-then-slow
   shape, kept as an independent counter/backoff from the GAP-level reconnect_retries below
   since a proof failure can recur even when the physical link connects cleanly every time. */
#define FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP 6u
#define FEB_RUNTIME_AUTH_SLOW_CADENCE_MS (5u * 60u * 1000u)

/* The Flipper's Write characteristic (flipper_esp32_over_ble.c PAYLOAD_MAX) has a fixed
   max GATT attribute value length, set once at registration and independent of the
   negotiated ATT MTU. Writes to it must never exceed this length, however large the
   negotiated MTU is. Modeling it as an "MTU" of length+ATT_WRITE_OVERHEAD lets us reuse
   feb_fragment_capacity() unchanged: feb_fragment_capacity(244 + 3) = 244 - 4 = 240 payload
   bytes/fragment, and clamping the real negotiated MTU down to this before calling
   feb_fragment_capacity() keeps the smaller value when the real MTU is even tinier (e.g.
   the pre-negotiation default of 23). Sourced from components/feb_protocol/framing.h's
   FEB_WRITE_CHAR_MAX_LEN so both firmwares stay in lockstep rather than hardcoding their
   own copy. */
#define FEB_FLIPPER_WRITE_CHAR_MAX_LEN FEB_WRITE_CHAR_MAX_LEN
#define FEB_FLIPPER_WRITE_EFFECTIVE_MTU (FEB_FLIPPER_WRITE_CHAR_MAX_LEN + FEB_ATT_WRITE_OVERHEAD)

char feb_board_id_buf[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
size_t feb_board_id_len;
uint8_t feb_pairing_epoch[FEB_PAIRING_EPOCH_LEN];
uint32_t feb_pairing_window_start_ms;
bool feb_pairing_window_closed;
pairing_state_t feb_pairing_state = PAIRING_STATE_IDLE;
tx_done_action_t feb_tx_done_action = TX_DONE_NONE;
feb_boot_mode_t feb_boot_mode = FEB_BOOT_MODE_PAIRING;
uint8_t feb_stored_pairing_secret[FEB_PAIRING_SECRET_LEN];
runtime_auth_state_t feb_runtime_auth_state = RUNTIME_AUTH_STATE_IDLE;
disconnect_reason_t feb_pending_disconnect_reason = DISCONNECT_REASON_NORMAL;

uint8_t feb_rt_session_id[FEB_SESSION_ID_LEN];
static uint8_t rt_client_nonce[FEB_SESSION_NONCE_FIELD_LEN];
static uint8_t rt_device_nonce[FEB_SESSION_NONCE_FIELD_LEN];
uint8_t feb_rt_session_key[FEB_SESSION_KEY_LEN];
static uint8_t rt_transcript_buf[FEB_SESSION_MAX_TRANSCRIPT_LEN];
static size_t rt_transcript_len;
uint8_t feb_runtime_auth_failure_count;
uint32_t feb_hello_ack_start_ms; /* 0 = no hello_ack wait currently active; else the
                                        wrap-safe elapsed-time base for FEB_HELLO_ACK_TIMEOUT_MS */
uint32_t feb_pair_reply_wait_start_ms; /* 0 = no wait active; else wrap-safe elapsed-time
                                              base for FEB_PAIR_REPLY_TIMEOUT_MS, mirrors
                                              feb_hello_ack_start_ms */

/* docs/PLAN.md step 7: first protected records exchanged post-auth. Per
   docs/PROTOCOL.md, a sequence counter begins at 1 for each authenticated session and
   increases by exactly one per protected record in a direction; set to 1 when
   feb_runtime_auth_state becomes AUTHENTICATED (feb_write_complete()'s TX_DONE_RUNTIME_AUTHENTICATED
   case), reset alongside the rest of the per-connection state on BLE_GAP_EVENT_CONNECT. */
uint64_t feb_rt_tx_sequence;
static uint8_t rt_ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];
#define FEB_TX_PENDING_PROTECTED_COUNT 4u
#define FEB_TX_PENDING_TYPE_MAX 32u

static uint8_t esp32_private_key[FEB_X25519_KEY_LEN];
static uint8_t esp32_public_key[FEB_X25519_KEY_LEN];
static uint8_t device_nonce[FEB_PAIRING_NONCE_LEN];
static uint8_t client_nonce[FEB_PAIRING_NONCE_LEN];
static uint8_t k_shared[FEB_PAIRING_KSHARED_LEN];
static uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN];
static uint8_t pairing_transcript_buf[FEB_PAIRING_MAX_TRANSCRIPT_LEN];
static size_t pairing_transcript_len;

static uint8_t tx_message_id;
uint8_t feb_pairing_payload_encode_buf[FEB_CBOR_MAX_PAYLOAD];
static uint8_t pairing_record_encode_buf[FEB_MAX_RECORD_SIZE];
static uint8_t tx_fragment_buf[FEB_MAX_RECORD_SIZE + FEB_TX_MAX_FRAGMENTS * FEB_FRAG_HEADER_SIZE];
static size_t tx_fragment_offsets[FEB_TX_MAX_FRAGMENTS];
static size_t tx_fragment_lens[FEB_TX_MAX_FRAGMENTS];
static size_t tx_fragment_write_pos;
uint8_t feb_tx_fragment_total;
uint8_t feb_tx_fragment_next;
typedef struct {
    uint16_t conn_handle;
    char type[FEB_TX_PENDING_TYPE_MAX];
    size_t type_len;
    uint8_t payload[FEB_CBOR_MAX_PAYLOAD];
    size_t payload_len;
    tx_done_action_t next_action;
} pending_protected_tx_t;
static pending_protected_tx_t pending_protected_tx[FEB_TX_PENDING_PROTECTED_COUNT];
uint8_t feb_pending_protected_tx_head;
uint8_t feb_pending_protected_tx_count;
bool feb_tx_dispatching_completion;

void feb_compute_board_id(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    int written;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read factory MAC: %s", esp_err_to_name(err));
        memcpy(feb_board_id_buf, FEB_BOARD_ID_PREFIX "-unknown", sizeof(FEB_BOARD_ID_PREFIX "-unknown"));
        feb_board_id_len = strlen(feb_board_id_buf);
        return;
    }
    written = snprintf(feb_board_id_buf, sizeof(feb_board_id_buf), FEB_BOARD_ID_PREFIX "-%02x%02x%02x%02x%02x%02x",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (written < 0) {
        ESP_LOGE(TAG, "board_id snprintf failed");
        return;
    }
    feb_board_id_len = (size_t)written < sizeof(feb_board_id_buf) - 1 ? (size_t)written
                                                               : sizeof(feb_board_id_buf) - 1;
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

bool feb_load_pairing_secret(uint8_t out[FEB_PAIRING_SECRET_LEN])
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

    if (feb_pairing_window_closed) {
        return false;
    }
    now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(now_ms - feb_pairing_window_start_ms) >= FEB_PAIRING_WINDOW_MS) {
        feb_pairing_window_closed = true;
        ESP_LOGW(TAG, "pairing window expired");
        return false;
    }
    return true;
}

#if FEB_HAS_LINK_HOOKS
/* feb_central.c's link-state reports (feb_hook_ble_state()) need the same check. */
bool feb_pairing_window_is_open(void)
{
    return pairing_window_is_open();
}
#endif

void feb_pairing_attempt_zeroize(void)
{
    feb_secure_zero(esp32_private_key, sizeof(esp32_private_key));
    feb_secure_zero(k_shared, sizeof(k_shared));
    feb_secure_zero(k_confirm, sizeof(k_confirm));
}

/* session_key is the only value session.h marks ephemeral for this state machine
   (client_nonce/device_nonce/session_id are not secret -- docs/PROTOCOL.md). */
void feb_runtime_auth_zeroize(void)
{
    feb_secure_zero(feb_rt_session_key, sizeof(feb_rt_session_key));
}

/* Called from factory_reset.c's perform_factory_reset() before esp_restart(), so the
   persisted pairing_secret's in-RAM copy (and any pairing/session scratch still live from
   an interrupted ceremony) doesn't survive in SRAM past the erase that's supposed to
   invalidate it. */
void feb_wipe_pairing_secrets(void)
{
    feb_secure_zero(feb_stored_pairing_secret, sizeof(feb_stored_pairing_secret));
    feb_pairing_attempt_zeroize();
    feb_runtime_auth_zeroize();
}

bool feb_connecting_permitted(void)
{
    if (feb_boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH) {
        /* No window in this mode -- retries indefinitely per docs/PLAN.md step 6. */
        return true;
    }
    return pairing_window_is_open();
}

uint32_t feb_runtime_auth_backoff_delay_ms(void)
{
    if (feb_runtime_auth_failure_count == 0) {
        return 0;
    }
    if (feb_runtime_auth_failure_count > FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP) {
        return FEB_RUNTIME_AUTH_SLOW_CADENCE_MS;
    }
    return 1000u << (feb_runtime_auth_failure_count - 1u);
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

void feb_send_next_tx_fragment(uint16_t conn_handle)
{
    int rc;

    if (feb_tx_fragment_next >= feb_tx_fragment_total) {
        return;
    }
    rc = ble_gattc_write_flat(conn_handle, feb_write_value_handle,
                              tx_fragment_buf + tx_fragment_offsets[feb_tx_fragment_next],
                              tx_fragment_lens[feb_tx_fragment_next], feb_write_complete, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "pairing fragment %u write failed: %d", feb_tx_fragment_next, rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    feb_tx_fragment_next++;
}

static bool queue_encoded_record_for_tx(size_t record_len)
{
    size_t capacity;
    uint8_t built_count = 0;

    capacity = feb_fragment_capacity(feb_negotiated_att_mtu < FEB_FLIPPER_WRITE_EFFECTIVE_MTU ?
                                     feb_negotiated_att_mtu : FEB_FLIPPER_WRITE_EFFECTIVE_MTU);
    if (capacity == 0) {
        ESP_LOGE(TAG, "no usable fragment capacity at MTU %u", feb_negotiated_att_mtu);
        return false;
    }

    tx_fragment_write_pos = 0;
    feb_tx_fragment_total = feb_fragment_record(pairing_record_encode_buf, record_len, capacity,
                                            tx_message_id, tx_emit_fragment, &built_count);
    if (feb_tx_fragment_total == 0 || feb_tx_fragment_total != built_count) {
        ESP_LOGE(TAG, "record fragmentation failed");
        return false;
    }
    tx_message_id++;
    feb_tx_fragment_next = 0;
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
    envelope.board_id = feb_board_id_buf;
    envelope.board_id_len = feb_board_id_len;
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
    memcpy(envelope.session_id, feb_rt_session_id, FEB_SESSION_ID_LEN);
    envelope.board_id = feb_board_id_buf;
    envelope.board_id_len = feb_board_id_len;
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

/* docs/PLAN.md step 7: protected (AES-256-GCM) record, ESP32-to-Flipper direction only --
   the ESP32 never originates a protected record in the other direction today. */
static bool encode_and_queue_protected_record(const char *type, size_t type_len,
                                              const uint8_t *payload, size_t payload_len,
                                              uint64_t sequence)
{
    size_t record_len = feb_session_encrypt_record(feb_rt_session_key, 2, type, type_len,
                                                    feb_rt_session_id, feb_board_id_buf, feb_board_id_len,
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
    if (feb_rt_tx_sequence >= FEB_SESSION_SEQUENCE_MAX) {
        ESP_LOGW(TAG, "protected tx sequence at cap; closing to force a new session");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return false;
    }
    if (!encode_and_queue_protected_record(type, type_len, payload, payload_len, feb_rt_tx_sequence)) {
        return false;
    }
    feb_rt_tx_sequence++;
    feb_tx_done_action = next_action;
    feb_send_next_tx_fragment(conn_handle);
    return true;
}

static bool enqueue_protected_send(uint16_t conn_handle, const char *type, size_t type_len,
                                   const uint8_t *payload, size_t payload_len,
                                   tx_done_action_t next_action)
{
    uint8_t index;
    pending_protected_tx_t *pending;

    if (type_len >= FEB_TX_PENDING_TYPE_MAX || payload_len > FEB_CBOR_MAX_PAYLOAD ||
        feb_pending_protected_tx_count >= FEB_TX_PENDING_PROTECTED_COUNT) {
        ESP_LOGW(TAG, "protected tx queue full or request too large");
        return false;
    }
    index = (uint8_t)((feb_pending_protected_tx_head + feb_pending_protected_tx_count) %
                      FEB_TX_PENDING_PROTECTED_COUNT);
    pending = &pending_protected_tx[index];
    pending->conn_handle = conn_handle;
    memcpy(pending->type, type, type_len);
    pending->type[type_len] = '\0';
    pending->type_len = type_len;
    memcpy(pending->payload, payload, payload_len);
    pending->payload_len = payload_len;
    pending->next_action = next_action;
    feb_pending_protected_tx_count++;
    return true;
}

bool feb_start_next_pending_protected_send(void)
{
    pending_protected_tx_t *pending;

    if (feb_pending_protected_tx_count == 0) {
        return true;
    }
    pending = &pending_protected_tx[feb_pending_protected_tx_head];
    if (!start_protected_send(pending->conn_handle, pending->type, pending->type_len,
                              pending->payload, pending->payload_len, pending->next_action)) {
        return false;
    }
    feb_pending_protected_tx_head = (uint8_t)((feb_pending_protected_tx_head + 1) %
                                          FEB_TX_PENDING_PROTECTED_COUNT);
    feb_pending_protected_tx_count--;
    return true;
}

/* Shared tail of every protected-record send: encode+queue, advance feb_rt_tx_sequence, arm
   `next_action` for feb_write_complete() to run once every fragment of this record has gone out,
   and kick off the first fragment. `next_action` is TX_DONE_NONE for a one-shot record
   (capability_response, error) or TX_DONE_CONTINUE_WIFI_SCAN when another status batch is
   already queued behind this one (docs/PLAN.md "Wi-Fi scan capability" step). */
bool feb_queue_and_send_protected(uint16_t conn_handle, const char *type, size_t type_len,
                                     const uint8_t *payload, size_t payload_len,
                                     tx_done_action_t next_action)
{
    if (feb_tx_fragment_next < feb_tx_fragment_total || feb_tx_dispatching_completion ||
        feb_pending_protected_tx_count > 0) {
        return enqueue_protected_send(conn_handle, type, type_len, payload, payload_len,
                                      next_action);
    }
    return start_protected_send(conn_handle, type, type_len, payload, payload_len, next_action);
}

bool feb_send_protected(uint16_t conn_handle, const char *type, size_t type_len,
                           const uint8_t *payload, size_t payload_len)
{
    return feb_queue_and_send_protected(conn_handle, type, type_len, payload, payload_len, TX_DONE_NONE);
}

bool feb_send_protected_error(uint16_t conn_handle, const char *code, size_t code_len,
                                 int has_request_id, uint64_t request_id)
{
    feb_error_payload_t err = {0};
    size_t payload_len;

    err.code = code;
    err.code_len = code_len;
    err.has_request_id = has_request_id;
    err.request_id = request_id;
    payload_len = feb_cbor_encode_error_payload(feb_pairing_payload_encode_buf,
                                                sizeof(feb_pairing_payload_encode_buf), &err);
    if (payload_len == 0) {
        return false;
    }
    return feb_send_protected(conn_handle, "error", strlen("error"), feb_pairing_payload_encode_buf, payload_len);
}

void feb_handle_capability_query(uint16_t conn_handle)
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

    payload_len = feb_cbor_encode_capability_response_payload(feb_pairing_payload_encode_buf,
                                                              sizeof(feb_pairing_payload_encode_buf), &response);
    if (payload_len == 0 ||
        !feb_send_protected(conn_handle, "capability_response", strlen("capability_response"),
                        feb_pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build capability_response");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "sending capability_response");
}

void feb_fail_pairing_ceremony(uint16_t conn_handle, feb_pairing_error_t err)
{
    feb_error_payload_t error_payload = {0};
    size_t payload_len;

    error_payload.code = feb_pairing_error_code_str(err);
    error_payload.code_len = strlen(error_payload.code);

    payload_len = feb_cbor_encode_error_payload(feb_pairing_payload_encode_buf,
                                                sizeof(feb_pairing_payload_encode_buf), &error_payload);
    feb_pairing_attempt_zeroize();
    if (payload_len == 0 ||
        !encode_and_queue_pairing_record("error", strlen("error"), feb_pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build pairing error record; disconnecting directly");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    feb_tx_done_action = TX_DONE_DISCONNECT;
    feb_send_next_tx_fragment(conn_handle);
}

void feb_begin_pairing(uint16_t conn_handle)
{
    uint8_t random_buf[FEB_X25519_KEY_LEN];
    feb_pair_init_payload_t init_payload;
    size_t payload_len;

    if (!pairing_window_is_open()) {
        ESP_LOGW(TAG, "pairing window closed before pair_init could be sent");
        feb_fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_EXPIRED);
        return;
    }

    esp_fill_random(random_buf, sizeof(random_buf));
    feb_x25519_keypair(esp32_private_key, esp32_public_key, random_buf);
    feb_secure_zero(random_buf, sizeof(random_buf));
    esp_fill_random(device_nonce, sizeof(device_nonce));

    memcpy(init_payload.pairing_epoch, feb_pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    memcpy(init_payload.device_nonce, device_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(init_payload.esp32_public_key, esp32_public_key, FEB_PAIRING_PUBKEY_LEN);

    payload_len = feb_cbor_encode_pair_init_payload(feb_pairing_payload_encode_buf,
                                                    sizeof(feb_pairing_payload_encode_buf), &init_payload);
    if (payload_len == 0 ||
        !encode_and_queue_pairing_record(FEB_PAIR_INIT_TYPE, strlen(FEB_PAIR_INIT_TYPE),
                                         feb_pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build pair_init");
        feb_pairing_attempt_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    feb_pairing_state = PAIRING_STATE_INIT_SENT;
    feb_pair_reply_wait_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    feb_tx_done_action = TX_DONE_AWAIT_PAIR_REPLY;
    ESP_LOGI(TAG, "sending pair_init");
    feb_send_next_tx_fragment(conn_handle);
}

void feb_handle_pair_reply(uint16_t conn_handle, const feb_pairing_envelope_t *envelope)
{
    feb_pair_reply_payload_t reply;
    feb_cbor_status_t status;
    feb_pairing_transcript_t transcript = {0};
    uint8_t expected_confirm[FEB_PAIRING_REPLY_CONFIRM_LEN];
    feb_pair_confirm_payload_t confirm_payload;
    size_t payload_len;

    feb_pair_reply_wait_start_ms = 0;
    status = feb_cbor_decode_pair_reply_payload(envelope->payload_span, envelope->payload_span_len, &reply);
    if (status != FEB_CBOR_OK) {
        ESP_LOGW(TAG, "pair_reply payload decode failed: %d", (int)status);
        feb_fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_x25519(k_shared, esp32_private_key, reply.flipper_public_key);
    if (feb_is_all_zero(k_shared, sizeof(k_shared))) {
        ESP_LOGW(TAG, "rejected all-zero X25519 shared secret");
        feb_fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    memcpy(client_nonce, reply.client_nonce, FEB_PAIRING_NONCE_LEN);

    transcript.version = 2;
    memcpy(transcript.service_uuid, FEB_PAIRING_SERVICE_UUID, FEB_PAIRING_SERVICE_UUID_LEN);
    transcript.board_id = feb_board_id_buf;
    transcript.board_id_len = feb_board_id_len;
    memcpy(transcript.pairing_epoch, feb_pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    memcpy(transcript.client_nonce, client_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(transcript.device_nonce, device_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(transcript.esp32_public_key, esp32_public_key, FEB_PAIRING_PUBKEY_LEN);
    memcpy(transcript.flipper_public_key, reply.flipper_public_key, FEB_PAIRING_PUBKEY_LEN);

    pairing_transcript_len = feb_pairing_encode_transcript(pairing_transcript_buf,
                                                           sizeof(pairing_transcript_buf), &transcript);
    if (pairing_transcript_len == 0) {
        ESP_LOGE(TAG, "transcript T encode failed");
        feb_fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_pairing_derive_kconfirm(k_shared, feb_pairing_epoch, k_confirm);
    feb_pairing_flipper_confirm(k_confirm, pairing_transcript_buf, pairing_transcript_len, expected_confirm);
    if (!feb_consttime_equal(expected_confirm, reply.confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN)) {
        ESP_LOGW(TAG, "flipper confirmation mismatch");
        feb_fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_pairing_esp32_confirm(k_confirm, pairing_transcript_buf, pairing_transcript_len,
                              confirm_payload.confirmation);
    payload_len = feb_cbor_encode_pair_confirm_payload(feb_pairing_payload_encode_buf,
                                                       sizeof(feb_pairing_payload_encode_buf), &confirm_payload);
    if (payload_len == 0 ||
        !encode_and_queue_pairing_record(FEB_PAIR_CONFIRM_TYPE, strlen(FEB_PAIR_CONFIRM_TYPE),
                                         feb_pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build pair_confirm");
        feb_fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_pairing_state = PAIRING_STATE_CONFIRM_SENT;
    feb_tx_done_action = TX_DONE_SEND_PAIR_COMPLETE;
    ESP_LOGI(TAG, "sending pair_confirm");
    feb_send_next_tx_fragment(conn_handle);
}

void feb_finish_pairing_after_confirm(uint16_t conn_handle)
{
    uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN];
    feb_pair_complete_payload_t complete_payload;
    size_t payload_len;

    feb_pairing_derive_secret(k_shared, feb_pairing_epoch, client_nonce, device_nonce,
                             feb_board_id_buf, feb_board_id_len, pairing_secret);

    if (!persist_pairing_secret(pairing_secret)) {
        ESP_LOGE(TAG, "failed to persist pairing_secret; aborting ceremony");
        feb_secure_zero(pairing_secret, sizeof(pairing_secret));
        feb_fail_pairing_ceremony(conn_handle, FEB_PAIRING_ERR_FAILED);
        return;
    }

    feb_pairing_complete_tag(k_confirm, pairing_transcript_buf, pairing_transcript_len,
                             complete_payload.confirmation);
    feb_secure_zero(pairing_secret, sizeof(pairing_secret));

    payload_len = feb_cbor_encode_pair_complete_payload(feb_pairing_payload_encode_buf,
                                                        sizeof(feb_pairing_payload_encode_buf), &complete_payload);
    if (payload_len == 0 ||
        !encode_and_queue_pairing_record(FEB_PAIR_COMPLETE_TYPE, strlen(FEB_PAIR_COMPLETE_TYPE),
                                         feb_pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build pair_complete after persisting secret");
        feb_pairing_attempt_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    feb_pairing_attempt_zeroize();
    feb_pairing_state = PAIRING_STATE_COMPLETE_SENT;
    feb_tx_done_action = TX_DONE_DISCONNECT;
    ESP_LOGI(TAG, "pairing_secret persisted; sending pair_complete");
    feb_send_next_tx_fragment(conn_handle);
}

void feb_fail_runtime_auth(uint16_t conn_handle)
{
    if (feb_runtime_auth_failure_count < 0xFFu) {
        feb_runtime_auth_failure_count++;
    }
    feb_hello_ack_start_ms = 0;
    feb_pending_disconnect_reason = DISCONNECT_REASON_AUTH_FAILED;
    feb_runtime_auth_zeroize();
    ESP_LOGW(TAG, "runtime auth failed (%u consecutive failure(s)); closing without reply",
             feb_runtime_auth_failure_count);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

void feb_handle_runtime_auth_unknown_board(uint16_t conn_handle)
{
    feb_hello_ack_start_ms = 0;
    feb_pending_disconnect_reason = DISCONNECT_REASON_UNKNOWN_BOARD;
    feb_runtime_auth_zeroize();
    ESP_LOGW(TAG, "flipper has no pairing record for board_id=%s; will open a pairing window "
                  "on the next connection attempt", feb_board_id_buf);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

void feb_begin_runtime_auth(uint16_t conn_handle)
{
    feb_hello_payload_t hello_payload;
    size_t payload_len;

    esp_fill_random(feb_rt_session_id, sizeof(feb_rt_session_id));
    esp_fill_random(rt_client_nonce, sizeof(rt_client_nonce));
    memcpy(hello_payload.client_nonce, rt_client_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    payload_len = feb_cbor_encode_hello_payload(feb_pairing_payload_encode_buf,
                                                sizeof(feb_pairing_payload_encode_buf), &hello_payload);
    if (payload_len == 0 ||
        !encode_and_queue_session_record(FEB_HELLO_TYPE, strlen(FEB_HELLO_TYPE),
                                         feb_pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build hello");
        feb_runtime_auth_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    feb_runtime_auth_state = RUNTIME_AUTH_STATE_HELLO_SENT;
    feb_tx_done_action = TX_DONE_AWAIT_HELLO_ACK;
    feb_hello_ack_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "sending hello");
    feb_send_next_tx_fragment(conn_handle);
}

void feb_handle_hello_ack(uint16_t conn_handle, const feb_unencrypted_record_t *envelope)
{
    feb_hello_ack_payload_t ack;
    feb_cbor_status_t status;
    feb_session_transcript_t transcript = {0};
    uint8_t expected_proof[FEB_SESSION_PROOF_LEN];
    feb_client_auth_payload_t auth_payload;
    size_t payload_len;

    feb_hello_ack_start_ms = 0;

    status = feb_cbor_decode_hello_ack_payload(envelope->payload_span, envelope->payload_span_len, &ack);
    if (status != FEB_CBOR_OK) {
        ESP_LOGW(TAG, "hello_ack payload decode failed: %d", (int)status);
        feb_fail_runtime_auth(conn_handle);
        return;
    }

    memcpy(rt_device_nonce, ack.device_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    transcript.version = 2;
    transcript.board_id = feb_board_id_buf;
    transcript.board_id_len = feb_board_id_len;
    memcpy(transcript.session_id, feb_rt_session_id, FEB_SESSION_ID_LEN);
    memcpy(transcript.client_nonce, rt_client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(transcript.device_nonce, rt_device_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    rt_transcript_len = feb_session_encode_transcript(rt_transcript_buf, sizeof(rt_transcript_buf), &transcript);
    if (rt_transcript_len == 0) {
        ESP_LOGE(TAG, "runtime transcript S encode failed");
        feb_fail_runtime_auth(conn_handle);
        return;
    }

    feb_session_flipper_proof(feb_stored_pairing_secret, rt_transcript_buf, rt_transcript_len, expected_proof);
    if (!feb_consttime_equal(expected_proof, ack.proof, FEB_SESSION_PROOF_LEN)) {
        ESP_LOGW(TAG, "flipper runtime proof mismatch");
        feb_fail_runtime_auth(conn_handle);
        return;
    }

    /* Proof verified: this attempt succeeded, so the failure-rate-limit resets. */
    feb_runtime_auth_failure_count = 0;

    feb_session_derive_key(feb_stored_pairing_secret, rt_client_nonce, rt_device_nonce,
                           feb_board_id_buf, feb_board_id_len, feb_rt_session_id, feb_rt_session_key);

    feb_session_esp32_proof(feb_stored_pairing_secret, rt_transcript_buf, rt_transcript_len, auth_payload.proof);
    payload_len = feb_cbor_encode_client_auth_payload(feb_pairing_payload_encode_buf,
                                                      sizeof(feb_pairing_payload_encode_buf), &auth_payload);
    if (payload_len == 0 ||
        !encode_and_queue_session_record(FEB_CLIENT_AUTH_TYPE, strlen(FEB_CLIENT_AUTH_TYPE),
                                         feb_pairing_payload_encode_buf, payload_len)) {
        ESP_LOGE(TAG, "failed to build client_auth");
        feb_runtime_auth_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    feb_tx_done_action = TX_DONE_RUNTIME_AUTHENTICATED;
    ESP_LOGI(TAG, "sending client_auth");
    feb_send_next_tx_fragment(conn_handle);
}

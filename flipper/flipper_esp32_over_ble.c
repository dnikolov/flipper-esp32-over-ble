#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_random.h>
#include <bt/bt_service/bt.h>
#include <ble/ble.h>
#include <ble_glue.h>
#include <app_common.h>
#include <ble/core/ble_defs.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>
#include <furi_ble/profile_interface.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <storage/storage.h>
#include <furi/core/string.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#include <stdio.h>
#include <string.h>

#include "framing.h"
#include "cbor_codec.h"
#include "pairing.h"
#include "pairing_crypto.h"
#include "session.h"

#define TAG "Esp32OverBle"
#define PAYLOAD_MAX 64
/* Default (pre-MTU-negotiation) BLE ATT MTU. Pairing records are exchanged before/around
   MTU negotiation, so outgoing pairing-phase records are always fragmented against this
   conservative worst-case value rather than an MTU that may not have taken effect yet. */
#define FEB_DEFAULT_ATT_MTU 23
#define PAIRING_REASON_MAX_LEN 32
#define PAIRING_DIR_NAME "pairings"
#define FEB_PAIRINGS_PATH_MAX_LEN 96
/* Polling period for the reassembly-stall check, well under FEB_REASSEMBLY_TIMEOUT_MS
   (2000ms) so a stalled fragment sequence is reclaimed promptly rather than right at the
   deadline. */
#define REASSEMBLY_TIMEOUT_CHECK_PERIOD_MS 500

typedef enum {
    CharacteristicWrite,
    CharacteristicNotify,
    CharacteristicCount,
} CharacteristicId;

typedef enum {
    PairingPhaseNone,
    PairingPhaseWaiting,
    PairingPhaseExchanging,
    PairingPhaseConfirming,
    PairingPhaseSaving,
    /* Runtime session establishment (docs/PLAN.md step 6) reuses this same connection-
       phase enum rather than a parallel one -- a connecting peer is either running the
       fresh-pairing ceremony (phases above) or the runtime hello/hello_ack/client_auth
       flow (phases below); the two are mutually exclusive per connection. */
    PairingPhaseAuthenticating,
    PairingPhaseSessionActive,
    PairingPhaseDone,
    PairingPhaseFailed,
} PairingPhase;

typedef enum {
    AppEventInput,
    AppEventBtStatus,
    AppEventPairingPhase,
} AppEventType;

typedef struct {
    AppEventType type;
    InputEvent input;
    BtStatus bt_status;
    PairingPhase pairing_phase;
    char pairing_reason[PAIRING_REASON_MAX_LEN];
} AppEvent;

typedef struct {
    Bt* bt;
    Storage* storage;
    NotificationApp* notifications;
    FuriMessageQueue* queue;
    FuriHalBleProfileBase* profile;
    PairingPhase pairing_phase;
    char pairing_reason[PAIRING_REASON_MAX_LEN];
    bool has_saved_pairing;
} Esp32App;

typedef struct {
    FuriHalBleProfileBase base;
    uint16_t service_handle;
    BleGattCharacteristicInstance characteristics[CharacteristicCount];
    GapSvcEventHandler* event_handler;
    Esp32App* app;
} Esp32BleProfile;

static const Service_UUID_t service_uuid = {.Service_UUID_128 =
                                                {0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
                                                 0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x11}};

typedef struct {
    const uint8_t* data;
    uint16_t len;
} NotifyFragment;

/* ble_gatt_characteristic_update()'s Fixed-data path always sends data.fixed.length bytes
   regardless of the source buffer's real size; a Callback characteristic is required here
   so each notification carries exactly the fragment's own length over the air. */
static bool notify_data_callback(const void* context, const uint8_t** data, uint16_t* data_len) {
    if(context == NULL) {
        if(data) *data = NULL;
        if(data_len) *data_len = PAYLOAD_MAX;
        return false;
    }
    const NotifyFragment* fragment = context;
    if(data) *data = fragment->data;
    if(data_len) *data_len = fragment->len;
    return false;
}

static const BleGattCharacteristicParams characteristics[CharacteristicCount] = {
    [CharacteristicWrite] =
        {.name = "Write",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = PAYLOAD_MAX,
         .uuid.Char_UUID_128 = {0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31, 0x9e, 0xa2,
                                0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x12},
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [CharacteristicNotify] =
        {.name = "Notify",
         .data_prop_type = FlipperGattCharacteristicDataCallback,
         .data.callback.fn = notify_data_callback,
         .data.callback.context = NULL,
         .uuid.Char_UUID_128 = {0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31, 0x9e, 0xa2,
                                0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x13},
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
         .is_variable = CHAR_VALUE_LEN_VARIABLE}};

static feb_reassembly_t reassembly;
/* feb_reassembly_feed() runs synchronously on the BleEventWorker thread (see
   profile_event_handler); the periodic timeout check below runs on the separate Furi
   timer-service thread, so `reassembly` needs real cross-thread protection here — unlike
   every other BleEventWorker-only static in this file, which stays safe under the
   single-threaded-BLE-dispatch assumption alone. Both critical sections are tiny (no
   crypto, no nested calls), so a plain blocking mutex acquire is fine on the 1280-byte
   BleEventWorker stack. */
static FuriMutex* reassembly_mutex;
static FuriTimer* reassembly_timeout_timer;
static uint8_t outgoing_message_id;

static const FuriHalBleProfileTemplate profile_callbacks;

/* ---- pairing ceremony state: single BLE connection, single-threaded BLE event dispatch
   (see profile_event_handler), so static storage for the one in-flight ceremony is safe
   and keeps these off the ~1280-byte BleEventWorker stack (docs/SESSION_MEMORY.md's
   stack-overflow root cause). ---- */
typedef enum {
    PairStageNone,
    PairStageInitReceived,
    PairStageConfirmReceived,
} PairStage;

static PairStage pair_stage = PairStageNone;
static char pair_board_id[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
static size_t pair_board_id_len;
static uint8_t pair_transcript[FEB_PAIRING_MAX_TRANSCRIPT_LEN];
static size_t pair_transcript_len;
static uint8_t pair_k_confirm[FEB_PAIRING_KCONFIRM_LEN];
static uint8_t pair_secret[FEB_PAIRING_SECRET_LEN];

static uint8_t pairing_payload_buf[256];
static uint8_t pairing_record_buf[FEB_MAX_RECORD_SIZE];

static void emit_fragment(const uint8_t* fragment, size_t fragment_len, void* ctx) {
    Esp32BleProfile* profile = ctx;
    NotifyFragment notify_fragment = {.data = fragment, .len = (uint16_t)fragment_len};
    ble_gatt_characteristic_update(
        profile->service_handle, &profile->characteristics[CharacteristicNotify], &notify_fragment);
}

static bool send_pairing_record(Esp32BleProfile* profile, const uint8_t* record, size_t record_len) {
    size_t capacity = feb_fragment_capacity(FEB_DEFAULT_ATT_MTU);
    if(capacity == 0) {
        return false;
    }
    uint8_t message_id = outgoing_message_id++;
    uint8_t fragment_count =
        feb_fragment_record(record, record_len, capacity, message_id, emit_fragment, profile);
    return fragment_count != 0;
}

static int text_matches(const char* data, size_t len, const char* literal) {
    size_t literal_len = strlen(literal);
    return len == literal_len && memcmp(data, literal, literal_len) == 0;
}

static bool board_id_is_valid(const char* board_id, size_t len) {
    if(board_id == NULL || len == 0 || len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        return false;
    }
    for(size_t i = 0; i < len; i++) {
        char c = board_id[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if(!ok) {
            return false;
        }
    }
    return true;
}

/* Resolved once, from this app's own thread, at app init (see flipper_esp32_over_ble_app())
   -- never call APP_DATA_PATH() again after this. `APP_DATA_PATH(...)` is a pure compile-
   time string concatenation ("/data/" + path); the "/data" prefix is only resolved to a
   real `/ext/apps_data/<app id>/...` path when a storage_* call carrying it is issued, and
   that resolution keys off the *calling* thread's registered app id (confirmed in
   applications/services/storage/storage_processing.c's storage_process_alias(), via
   furi_thread_get_appid(thread_id)). handle_pair_complete() (and therefore
   pairing_storage_save()) run synchronously inside profile_event_handler() on the BLE
   stack's own "BleEventWorker" thread, owned by the firmware's built-in `bt` service --
   any `APP_DATA_PATH(...)` call made from that thread resolves against `bt`'s app id, not
   this app's, silently writing pairing files under the wrong directory (found during the
   step 5 hardware-verification pass, see docs/SESSION_MEMORY.md). Fixed by resolving the
   alias exactly once, synchronously, from this app's own thread via the exported
   storage_common_resolve_path_and_ensure_app_directory() (captures
   furi_thread_get_current_id() of its *caller*, not of whichever thread later reuses the
   resulting string), then caching the resulting absolute path (already starting with
   "/ext/apps_data/...", not "/data") for every later pairing-file path build -- a path
   that doesn't start with "/data" is used as a literal absolute path by every storage_*
   call regardless of which thread issues it, so this is safe to reuse from
   BleEventWorker afterward. */
static char pairings_dir_path[FEB_PAIRINGS_PATH_MAX_LEN];
static bool pairings_dir_ready;

static bool resolve_pairings_dir_path(Storage* storage) {
    FuriString* resolved = furi_string_alloc_set_str(APP_DATA_PATH(PAIRING_DIR_NAME));
    storage_common_resolve_path_and_ensure_app_directory(storage, resolved);
    bool ok = furi_string_size(resolved) < sizeof(pairings_dir_path);
    if(ok) {
        strncpy(pairings_dir_path, furi_string_get_cstr(resolved), sizeof(pairings_dir_path) - 1);
        pairings_dir_path[sizeof(pairings_dir_path) - 1] = '\0';
    } else {
        FURI_LOG_E(TAG, "Resolved pairings path too long to cache");
    }
    furi_string_free(resolved);
    if(!ok) {
        return false;
    }

    /* storage_common_resolve_path_and_ensure_app_directory() only guarantees
       /ext/apps_data/<app id> exists, not our own "pairings" subdirectory beneath it. */
    FS_Error mkdir_err = storage_common_mkdir(storage, pairings_dir_path);
    if(mkdir_err != FSE_OK && mkdir_err != FSE_EXIST) {
        FURI_LOG_E(TAG, "mkdir pairings dir failed: %d", mkdir_err);
        return false;
    }
    return true;
}

static bool build_pairing_path(
    char* out,
    size_t out_cap,
    const char* board_id,
    size_t board_id_len,
    bool tmp) {
    if(!pairings_dir_ready) {
        return false;
    }
    int written = snprintf(
        out,
        out_cap,
        "%s/%.*s%s",
        pairings_dir_path,
        (int)board_id_len,
        board_id,
        tmp ? ".dat.tmp" : ".dat");
    return written > 0 && (size_t)written < out_cap;
}

/* Atomic per-board persistence: temp-file write, exact-length verification,
   storage_file_sync(), close, remove-old, rename -- the archive_favorites.c precedent
   (docs/PLAN.md step 5) with the missing sync call added. One file per board_id so
   replacing one board's pairing can never touch another's. */
static bool
    pairing_storage_save(Storage* storage, const char* board_id, size_t board_id_len, const uint8_t* secret) {
    static char final_path[96];
    static char tmp_path[96];
    if(!build_pairing_path(final_path, sizeof(final_path), board_id, board_id_len, false) ||
       !build_pairing_path(tmp_path, sizeof(tmp_path), board_id, board_id_len, true)) {
        return false;
    }

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, tmp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        size_t written = storage_file_write(file, secret, FEB_PAIRING_SECRET_LEN);
        ok = (written == FEB_PAIRING_SECRET_LEN) && storage_file_sync(file);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(!ok) {
        storage_common_remove(storage, tmp_path);
        return false;
    }

    storage_common_remove(storage, final_path);
    FS_Error rename_err = storage_common_rename(storage, tmp_path, final_path);
    if(rename_err != FSE_OK) {
        FURI_LOG_E(TAG, "rename pairing file failed: %d", rename_err);
        storage_common_remove(storage, tmp_path);
        return false;
    }
    return true;
}

/* Loads a previously-saved pairing_secret for `board_id`. Returns false uniformly for "no
   file" and "file present but unreadable/wrong length" -- docs/PROTOCOL.md's `unknown_board`
   response doesn't distinguish these causes to the peer either (do not expose the cause,
   matching this file's existing pairing_failed convention); the real reason is still
   logged locally for diagnostics. */
static bool
    pairing_storage_load(Storage* storage, const char* board_id, size_t board_id_len, uint8_t* secret_out) {
    static char path[96];
    if(!build_pairing_path(path, sizeof(path), board_id, board_id_len, false)) {
        return false;
    }
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        size_t read = storage_file_read(file, secret_out, FEB_PAIRING_SECRET_LEN);
        ok = (read == FEB_PAIRING_SECRET_LEN);
        if(!ok) {
            FURI_LOG_W(TAG, "Pairing file for '%.*s' unreadable or wrong length", (int)board_id_len, board_id);
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

static bool any_saved_pairing_exists(Storage* storage) {
    if(!pairings_dir_ready) {
        return false;
    }
    File* dir = storage_file_alloc(storage);
    bool found = false;
    if(storage_dir_open(dir, pairings_dir_path)) {
        FileInfo info;
        char name[64];
        found = storage_dir_read(dir, &info, name, sizeof(name));
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    return found;
}

static void post_pairing_phase(Esp32App* app, PairingPhase phase, const char* reason) {
    AppEvent event = {.type = AppEventPairingPhase, .pairing_phase = phase};
    if(reason) {
        strncpy(event.pairing_reason, reason, sizeof(event.pairing_reason) - 1);
        event.pairing_reason[sizeof(event.pairing_reason) - 1] = '\0';
    } else {
        event.pairing_reason[0] = '\0';
    }
    furi_message_queue_put(app->queue, &event, 0);
}

static void pairing_reset_state(void) {
    pair_stage = PairStageNone;
    feb_secure_zero(pair_board_id, sizeof(pair_board_id));
    pair_board_id_len = 0;
    feb_secure_zero(pair_transcript, sizeof(pair_transcript));
    pair_transcript_len = 0;
    feb_secure_zero(pair_k_confirm, sizeof(pair_k_confirm));
    feb_secure_zero(pair_secret, sizeof(pair_secret));
}

static void send_pairing_wire_error(Esp32BleProfile* profile, feb_pairing_error_t err) {
    const char* code = feb_pairing_error_code_str(err);
    static feb_error_payload_t error_payload;
    error_payload = (feb_error_payload_t){
        .code = code,
        .code_len = strlen(code),
        .has_message = 0,
        .has_request_id = 0,
    };
    size_t payload_len =
        feb_cbor_encode_error_payload(pairing_payload_buf, sizeof(pairing_payload_buf), &error_payload);
    if(payload_len == 0) {
        return;
    }
    static feb_pairing_envelope_t envelope;
    envelope = (feb_pairing_envelope_t){
        .version = 2,
        .type = "error",
        .type_len = sizeof("error") - 1,
        .board_id = pair_board_id,
        .board_id_len = pair_board_id_len,
        .payload_span = pairing_payload_buf,
        .payload_span_len = payload_len,
    };
    size_t record_len =
        feb_cbor_encode_pairing_envelope(pairing_record_buf, sizeof(pairing_record_buf), &envelope);
    if(record_len == 0) {
        return;
    }
    send_pairing_record(profile, pairing_record_buf, record_len);
}

/* Ends the in-progress ceremony attempt: zeroizes ephemeral secrets, resets the LED to
   off, and reports the failure phase to the UI. `send_wire_error` is false for a raw
   decode/board_id failure where we cannot trust enough of the record to safely echo a
   board_id-bearing reply (docs/PLAN.md step 5 board_id validation gap). */
static void abort_pairing(Esp32BleProfile* profile, const char* reason, bool send_wire_error) {
    Esp32App* app = profile->app;
    if(send_wire_error && pair_board_id_len > 0) {
        send_pairing_wire_error(profile, FEB_PAIRING_ERR_FAILED);
    }
    pairing_reset_state();
    if(app->notifications) {
        /* sequence_reset_blue only touches the static RGB channel; an active
           sequence_blink_start_blue runs on the separate hardware LED-blink
           subsystem and keeps blinking until explicitly stopped. */
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_reset_blue);
    }
    post_pairing_phase(app, PairingPhaseFailed, reason);
    FURI_LOG_W(TAG, "Pairing aborted: %s", reason);
}

static void handle_pair_init(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope) {
    Esp32App* app = profile->app;
    /* board_id is already charset/length-validated by the caller (profile_event_handler)
       before dispatch, so it is copied in first -- a payload decode failure below still
       has a known-good board_id to echo back in a pairing_failed reply. */
    pairing_reset_state();
    memcpy(pair_board_id, envelope->board_id, envelope->board_id_len);
    pair_board_id[envelope->board_id_len] = '\0';
    pair_board_id_len = envelope->board_id_len;

    static feb_pair_init_payload_t init_payload;
    feb_cbor_status_t status = feb_cbor_decode_pair_init_payload(
        envelope->payload_span, envelope->payload_span_len, &init_payload);
    if(status != FEB_CBOR_OK) {
        abort_pairing(profile, "malformed pair_init", true);
        return;
    }

    static uint8_t flipper_private[FEB_X25519_KEY_LEN];
    static uint8_t flipper_public[FEB_X25519_KEY_LEN];
    static uint8_t client_nonce[FEB_PAIRING_NONCE_LEN];
    static uint8_t k_shared[FEB_PAIRING_KSHARED_LEN];

    furi_hal_random_fill_buf(flipper_private, sizeof(flipper_private));
    furi_hal_random_fill_buf(client_nonce, sizeof(client_nonce));
    feb_x25519_base(flipper_public, flipper_private);
    feb_x25519(k_shared, flipper_private, init_payload.esp32_public_key);
    feb_secure_zero(flipper_private, sizeof(flipper_private));

    if(feb_is_all_zero(k_shared, sizeof(k_shared))) {
        feb_secure_zero(k_shared, sizeof(k_shared));
        abort_pairing(profile, "invalid shared secret", true);
        return;
    }

    static feb_pairing_transcript_t transcript;
    memset(&transcript, 0, sizeof(transcript));
    transcript.version = 2;
    memcpy(transcript.service_uuid, FEB_PAIRING_SERVICE_UUID, FEB_PAIRING_SERVICE_UUID_LEN);
    transcript.board_id = pair_board_id;
    transcript.board_id_len = pair_board_id_len;
    memcpy(transcript.pairing_epoch, init_payload.pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    memcpy(transcript.client_nonce, client_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(transcript.device_nonce, init_payload.device_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(transcript.esp32_public_key, init_payload.esp32_public_key, FEB_PAIRING_PUBKEY_LEN);
    memcpy(transcript.flipper_public_key, flipper_public, FEB_PAIRING_PUBKEY_LEN);

    pair_transcript_len =
        feb_pairing_encode_transcript(pair_transcript, sizeof(pair_transcript), &transcript);
    if(pair_transcript_len == 0) {
        feb_secure_zero(k_shared, sizeof(k_shared));
        abort_pairing(profile, "transcript encode failed", true);
        return;
    }

    feb_pairing_derive_kconfirm(k_shared, init_payload.pairing_epoch, pair_k_confirm);
    feb_pairing_derive_secret(
        k_shared,
        init_payload.pairing_epoch,
        client_nonce,
        init_payload.device_nonce,
        pair_board_id,
        pair_board_id_len,
        pair_secret);
    feb_secure_zero(k_shared, sizeof(k_shared));

    static uint8_t flipper_confirm[FEB_PAIRING_REPLY_CONFIRM_LEN];
    feb_pairing_flipper_confirm(pair_k_confirm, pair_transcript, pair_transcript_len, flipper_confirm);

    static feb_pair_reply_payload_t reply_payload;
    memcpy(reply_payload.client_nonce, client_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(reply_payload.flipper_public_key, flipper_public, FEB_PAIRING_PUBKEY_LEN);
    memcpy(reply_payload.confirmation, flipper_confirm, FEB_PAIRING_REPLY_CONFIRM_LEN);

    size_t payload_len = feb_cbor_encode_pair_reply_payload(
        pairing_payload_buf, sizeof(pairing_payload_buf), &reply_payload);
    if(payload_len == 0) {
        abort_pairing(profile, "pair_reply encode failed", true);
        return;
    }

    static feb_pairing_envelope_t reply_envelope;
    reply_envelope = (feb_pairing_envelope_t){
        .version = 2,
        .type = FEB_PAIR_REPLY_TYPE,
        .type_len = sizeof(FEB_PAIR_REPLY_TYPE) - 1,
        .board_id = pair_board_id,
        .board_id_len = pair_board_id_len,
        .payload_span = pairing_payload_buf,
        .payload_span_len = payload_len,
    };
    size_t record_len = feb_cbor_encode_pairing_envelope(
        pairing_record_buf, sizeof(pairing_record_buf), &reply_envelope);
    if(record_len == 0) {
        abort_pairing(profile, "pair_reply record encode failed", true);
        return;
    }

    if(!send_pairing_record(profile, pairing_record_buf, record_len)) {
        abort_pairing(profile, "pair_reply send failed", false);
        return;
    }

    pair_stage = PairStageInitReceived;
    post_pairing_phase(app, PairingPhaseConfirming, NULL);
}

static void handle_pair_confirm(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope) {
    if(pair_stage != PairStageInitReceived) {
        FURI_LOG_W(TAG, "Ignoring unexpected pair_confirm");
        return;
    }
    if(envelope->board_id_len != pair_board_id_len ||
       memcmp(envelope->board_id, pair_board_id, pair_board_id_len) != 0) {
        abort_pairing(profile, "board_id mismatch", true);
        return;
    }

    static feb_pair_confirm_payload_t confirm_payload;
    feb_cbor_status_t status = feb_cbor_decode_pair_confirm_payload(
        envelope->payload_span, envelope->payload_span_len, &confirm_payload);
    if(status != FEB_CBOR_OK) {
        abort_pairing(profile, "malformed pair_confirm", true);
        return;
    }

    static uint8_t expected[FEB_PAIRING_REPLY_CONFIRM_LEN];
    feb_pairing_esp32_confirm(pair_k_confirm, pair_transcript, pair_transcript_len, expected);
    bool match =
        feb_consttime_equal(expected, confirm_payload.confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN) != 0;
    feb_secure_zero(expected, sizeof(expected));
    if(!match) {
        abort_pairing(profile, "confirm mismatch", true);
        return;
    }

    pair_stage = PairStageConfirmReceived;
    post_pairing_phase(profile->app, PairingPhaseSaving, NULL);
}

static void handle_pair_complete(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope) {
    Esp32App* app = profile->app;
    if(pair_stage != PairStageConfirmReceived) {
        FURI_LOG_W(TAG, "Ignoring unexpected pair_complete");
        return;
    }
    if(envelope->board_id_len != pair_board_id_len ||
       memcmp(envelope->board_id, pair_board_id, pair_board_id_len) != 0) {
        abort_pairing(profile, "board_id mismatch", true);
        return;
    }

    static feb_pair_complete_payload_t complete_payload;
    feb_cbor_status_t status = feb_cbor_decode_pair_complete_payload(
        envelope->payload_span, envelope->payload_span_len, &complete_payload);
    if(status != FEB_CBOR_OK) {
        abort_pairing(profile, "malformed pair_complete", true);
        return;
    }

    static uint8_t expected[FEB_PAIRING_COMPLETE_TAG_LEN];
    feb_pairing_complete_tag(pair_k_confirm, pair_transcript, pair_transcript_len, expected);
    bool match =
        feb_consttime_equal(expected, complete_payload.confirmation, FEB_PAIRING_COMPLETE_TAG_LEN) != 0;
    feb_secure_zero(expected, sizeof(expected));
    if(!match) {
        abort_pairing(profile, "complete tag mismatch", true);
        return;
    }

    bool saved = pairing_storage_save(app->storage, pair_board_id, pair_board_id_len, pair_secret);
    static char board_id_copy[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
    memcpy(board_id_copy, pair_board_id, pair_board_id_len);
    board_id_copy[pair_board_id_len] = '\0';
    pairing_reset_state();

    if(!saved) {
        if(app->notifications) {
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_reset_blue);
        }
        post_pairing_phase(app, PairingPhaseFailed, "storage write failed");
        FURI_LOG_E(TAG, "Failed to persist pairing secret for board '%s'", board_id_copy);
        return;
    }

    if(app->notifications) {
        /* Must stop the running blink sequence explicitly - it's a separate
           hardware LED-blink subsystem that a plain RGB message can't override. */
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_set_only_blue_255);
    }
    post_pairing_phase(app, PairingPhaseDone, NULL);
    FURI_LOG_I(TAG, "Paired with board '%s'", board_id_copy);
}

/* ---- runtime session establishment (docs/PLAN.md step 6): hello/hello_ack/client_auth,
   single BLE connection, single-threaded BLE event dispatch (see profile_event_handler),
   so static storage for the one in-flight session attempt is safe and keeps these off the
   ~1280-byte BleEventWorker stack -- same rationale as the pairing-ceremony statics above.
   session_key is the one exception that survives past this handshake: it must stay alive
   for as long as the authenticated session does (step 7 will use it to
   encrypt/decrypt protected records), so it is only zeroized on disconnect/session end,
   never immediately after derivation. ---- */
typedef enum {
    SessionStageNone,
    SessionStageHelloReceived,
    SessionStageActive,
} SessionStage;

static SessionStage session_stage = SessionStageNone;
static char session_board_id[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
static size_t session_board_id_len;
static uint8_t session_id_bytes[FEB_SESSION_ID_LEN];
static uint8_t session_client_nonce[FEB_SESSION_NONCE_FIELD_LEN];
static uint8_t session_device_nonce[FEB_SESSION_NONCE_FIELD_LEN];
static uint8_t session_transcript_buf[FEB_SESSION_MAX_TRANSCRIPT_LEN];
static size_t session_transcript_len;
static uint8_t session_pairing_secret[FEB_PAIRING_SECRET_LEN];
static uint8_t session_key[FEB_SESSION_KEY_LEN];

static void session_reset_state(void) {
    session_stage = SessionStageNone;
    feb_secure_zero(session_board_id, sizeof(session_board_id));
    session_board_id_len = 0;
    feb_secure_zero(session_id_bytes, sizeof(session_id_bytes));
    feb_secure_zero(session_client_nonce, sizeof(session_client_nonce));
    feb_secure_zero(session_device_nonce, sizeof(session_device_nonce));
    feb_secure_zero(session_transcript_buf, sizeof(session_transcript_buf));
    session_transcript_len = 0;
    feb_secure_zero(session_pairing_secret, sizeof(session_pairing_secret));
    feb_secure_zero(session_key, sizeof(session_key));
}

/* docs/PROTOCOL.md's "Runtime auth failure handling": unknown_board replies use the
   pairing-record wire envelope (no session_id), since no session_id is trusted to exist
   yet -- deliberately distinct from feb_pairing_error_t (pairing.h's frozen contract),
   which has no unknown_board member; this is a session.h-domain error code, sent with the
   same envelope shape as a pairing-phase error rather than a new struct. Reuses this
   file's existing pairing_payload_buf/pairing_record_buf scratch buffers (already sized
   for this), matching send_pairing_wire_error()'s pattern. */
static void
    send_unknown_board_error(Esp32BleProfile* profile, const char* board_id, size_t board_id_len) {
    static feb_error_payload_t error_payload;
    error_payload = (feb_error_payload_t){
        .code = "unknown_board",
        .code_len = sizeof("unknown_board") - 1,
        .has_message = 0,
        .has_request_id = 0,
    };
    size_t payload_len =
        feb_cbor_encode_error_payload(pairing_payload_buf, sizeof(pairing_payload_buf), &error_payload);
    if(payload_len == 0) {
        return;
    }
    static feb_pairing_envelope_t envelope;
    envelope = (feb_pairing_envelope_t){
        .version = 2,
        .type = "error",
        .type_len = sizeof("error") - 1,
        .board_id = board_id,
        .board_id_len = board_id_len,
        .payload_span = pairing_payload_buf,
        .payload_span_len = payload_len,
    };
    size_t record_len =
        feb_cbor_encode_pairing_envelope(pairing_record_buf, sizeof(pairing_record_buf), &envelope);
    if(record_len == 0) {
        return;
    }
    send_pairing_record(profile, pairing_record_buf, record_len);
}

/* `hello` carries the ESP32's freshly-generated session_id + client_nonce and its stable
   board_id. A malformed hello payload is dropped silently (no reply) rather than answered
   with a wire error -- docs/PROTOCOL.md defines a reply only for the unknown_board case
   specifically; this project's established convention for anything else pre-authentication
   is to not hand an unauthenticated peer a reason, matching the client_auth-failure
   no-reply rule below. */
static void handle_hello(Esp32BleProfile* profile, const feb_unencrypted_record_t* envelope) {
    Esp32App* app = profile->app;
    if(!board_id_is_valid(envelope->board_id, envelope->board_id_len)) {
        FURI_LOG_W(TAG, "Rejected hello: invalid board_id");
        return;
    }

    session_reset_state();
    memcpy(session_board_id, envelope->board_id, envelope->board_id_len);
    session_board_id[envelope->board_id_len] = '\0';
    session_board_id_len = envelope->board_id_len;
    memcpy(session_id_bytes, envelope->session_id, FEB_SESSION_ID_LEN);

    static feb_hello_payload_t hello_payload;
    feb_cbor_status_t status = feb_cbor_decode_hello_payload(
        envelope->payload_span, envelope->payload_span_len, &hello_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "Rejected hello: malformed payload");
        session_reset_state();
        return;
    }
    memcpy(session_client_nonce, hello_payload.client_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    if(!pairing_storage_load(app->storage, session_board_id, session_board_id_len, session_pairing_secret)) {
        FURI_LOG_W(TAG, "hello for unknown board '%s'", session_board_id);
        send_unknown_board_error(profile, session_board_id, session_board_id_len);
        session_reset_state();
        return;
    }

    furi_hal_random_fill_buf(session_device_nonce, sizeof(session_device_nonce));

    static feb_session_transcript_t transcript;
    memset(&transcript, 0, sizeof(transcript));
    transcript.version = 2;
    transcript.board_id = session_board_id;
    transcript.board_id_len = session_board_id_len;
    memcpy(transcript.session_id, session_id_bytes, FEB_SESSION_ID_LEN);
    memcpy(transcript.client_nonce, session_client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(transcript.device_nonce, session_device_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    session_transcript_len =
        feb_session_encode_transcript(session_transcript_buf, sizeof(session_transcript_buf), &transcript);
    if(session_transcript_len == 0) {
        FURI_LOG_W(TAG, "hello: transcript encode failed");
        session_reset_state();
        return;
    }

    static uint8_t flipper_proof[FEB_SESSION_PROOF_LEN];
    feb_session_flipper_proof(
        session_pairing_secret, session_transcript_buf, session_transcript_len, flipper_proof);

    static feb_hello_ack_payload_t ack_payload;
    memcpy(ack_payload.device_nonce, session_device_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(ack_payload.proof, flipper_proof, FEB_SESSION_PROOF_LEN);

    size_t payload_len =
        feb_cbor_encode_hello_ack_payload(pairing_payload_buf, sizeof(pairing_payload_buf), &ack_payload);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "hello_ack: payload encode failed");
        session_reset_state();
        return;
    }

    static feb_unencrypted_record_t ack_record;
    memset(&ack_record, 0, sizeof(ack_record));
    ack_record.version = 2;
    ack_record.type = FEB_HELLO_ACK_TYPE;
    ack_record.type_len = sizeof(FEB_HELLO_ACK_TYPE) - 1;
    memcpy(ack_record.session_id, session_id_bytes, FEB_SESSION_ID_LEN);
    ack_record.board_id = session_board_id;
    ack_record.board_id_len = session_board_id_len;
    ack_record.payload_span = pairing_payload_buf;
    ack_record.payload_span_len = payload_len;

    size_t record_len =
        feb_cbor_encode_unencrypted(pairing_record_buf, sizeof(pairing_record_buf), &ack_record);
    if(record_len == 0) {
        FURI_LOG_W(TAG, "hello_ack: record encode failed");
        session_reset_state();
        return;
    }

    if(!send_pairing_record(profile, pairing_record_buf, record_len)) {
        FURI_LOG_W(TAG, "hello_ack: send failed");
        session_reset_state();
        return;
    }

    session_stage = SessionStageHelloReceived;
    post_pairing_phase(app, PairingPhaseAuthenticating, NULL);
}

/* `client_auth` carries the ESP32's proof over the same transcript S computed in
   handle_hello(). A proof mismatch is a real anomaly (desync/corruption/impersonation),
   not the "never paired" case -- per docs/PROTOCOL.md, it gets no reply at all (matching
   the existing no-proactive-bt_disconnect()-from-inside-profile_event_handler
   reentrancy-safety pattern; rely on the peer disconnecting) and must NOT be reported as
   unknown_board, which would let an attacker force repeated pairing-window openings by
   corrupting proofs in transit. A malformed client_auth payload is folded into the same
   no-reply bucket for the same reason: this layer must not give an unauthenticated peer a
   way to distinguish "malformed" from "wrong proof". */
static void handle_client_auth(Esp32BleProfile* profile, const feb_unencrypted_record_t* envelope) {
    if(session_stage != SessionStageHelloReceived) {
        FURI_LOG_W(TAG, "Ignoring unexpected client_auth");
        return;
    }
    if(envelope->board_id_len != session_board_id_len ||
       memcmp(envelope->board_id, session_board_id, session_board_id_len) != 0 ||
       memcmp(envelope->session_id, session_id_bytes, FEB_SESSION_ID_LEN) != 0) {
        FURI_LOG_W(TAG, "Ignoring client_auth: board_id/session_id mismatch");
        return;
    }

    static feb_client_auth_payload_t auth_payload;
    feb_cbor_status_t status = feb_cbor_decode_client_auth_payload(
        envelope->payload_span, envelope->payload_span_len, &auth_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "client_auth: malformed payload, dropping (no reply)");
        return;
    }

    static uint8_t expected_proof[FEB_SESSION_PROOF_LEN];
    feb_session_esp32_proof(
        session_pairing_secret, session_transcript_buf, session_transcript_len, expected_proof);
    bool match = feb_consttime_equal(expected_proof, auth_payload.proof, FEB_SESSION_PROOF_LEN) != 0;
    feb_secure_zero(expected_proof, sizeof(expected_proof));
    if(!match) {
        FURI_LOG_W(TAG, "client_auth: proof verification failed (no reply, per PROTOCOL.md)");
        session_reset_state();
        return;
    }

    feb_session_derive_key(
        session_pairing_secret,
        session_client_nonce,
        session_device_nonce,
        session_board_id,
        session_board_id_len,
        session_id_bytes,
        session_key);
    /* pairing_secret's in-memory copy has served its purpose (proof + key derivation);
       session_key itself is intentionally left alive -- see this section's top comment. */
    feb_secure_zero(session_pairing_secret, sizeof(session_pairing_secret));

    session_stage = SessionStageActive;
    if(profile->app->notifications) {
        /* Must stop the running blink sequence explicitly - it's a separate
           hardware LED-blink subsystem that a plain RGB message can't override. */
        notification_message(profile->app->notifications, &sequence_blink_stop);
        notification_message(profile->app->notifications, &sequence_set_only_blue_255);
    }
    post_pairing_phase(profile->app, PairingPhaseSessionActive, NULL);
    FURI_LOG_I(TAG, "Runtime session authenticated for board '%s'", session_board_id);
}

/* Runs on the Furi timer-service thread (not BleEventWorker) — see reassembly_mutex's
   comment above for why this needs the mutex. A stalled sequence is silently reclaimed
   (drop and continue): keep the BLE connection open, send no response, per PROTOCOL.md's
   malformed-fragment/message policy (docs/PLAN.md step 3). */
static void reassembly_timeout_timer_callback(void* context) {
    UNUSED(context);
    furi_mutex_acquire(reassembly_mutex, FuriWaitForever);
    feb_frame_status_t status = feb_reassembly_check_timeout(&reassembly, furi_get_tick());
    furi_mutex_release(reassembly_mutex);
    if(status == FEB_FRAME_TIMEOUT) {
        FURI_LOG_W(TAG, "Fragment reassembly timed out, buffer reclaimed");
    }
}

static BleEventAckStatus profile_event_handler(void* event, void* context) {
    Esp32BleProfile* profile = context;
    hci_event_pckt* event_packet = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);
    evt_blecore_aci* ble_event = (evt_blecore_aci*)event_packet->data;

    if(event_packet->evt == HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE &&
       ble_event->ecode == ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) {
        aci_gatt_attribute_modified_event_rp0* modified =
            (aci_gatt_attribute_modified_event_rp0*)ble_event->data;
        if(modified->Attr_Handle == profile->characteristics[CharacteristicWrite].handle + 1) {
            const uint8_t* out_record = NULL;
            size_t out_len = 0;
            furi_mutex_acquire(reassembly_mutex, FuriWaitForever);
            feb_frame_status_t status = feb_reassembly_feed(
                &reassembly,
                modified->Attr_Data,
                modified->Attr_Data_Length,
                furi_get_tick(),
                &out_record,
                &out_len);
            furi_mutex_release(reassembly_mutex);

            if(status == FEB_FRAME_OK) {
                return BleEventAckFlowEnable;
            } else if(status == FEB_FRAME_MESSAGE_COMPLETE) {
                /* Two outer envelope shapes share this one wire: the 4-field pairing
                   envelope (pair_init/pair_confirm/pair_complete/pairing-phase error) and
                   the 5-field session_id-bearing envelope (hello/client_auth/runtime
                   error). Which one a connecting peer sends depends on whether it already
                   holds a working pairing_secret -- this Flipper can't know that in
                   advance, so it peeks the map's own field count (a read-only header
                   parse, no further decoding) to route to the matching decoder, rather
                   than guessing or trying both. docs/PROTOCOL.md's "distinguish by
                   connection phase, not record content" instruction is about the
                   session-vs-pairing `error` ambiguity specifically (same `type` string,
                   two possible envelopes) -- it does not forbid this structural routing
                   step, which exists precisely because no phase state can predict which
                   envelope shape a not-yet-authenticated peer will use first. */
                feb_cbor_status_t peek_status = FEB_CBOR_OK;
                size_t field_count = 0;
                size_t peek_len = feb_cbor_decode_map_header(out_record, out_len, &field_count, &peek_status);
                if(peek_len == 0) {
                    FURI_LOG_W(TAG, "Rejected record: cbor status %d", peek_status);
                    return BleEventAckFlowEnable;
                }
                if(field_count == 4) {
                    static feb_pairing_envelope_t envelope;
                    feb_cbor_status_t decode_status =
                        feb_cbor_decode_pairing_envelope(out_record, out_len, &envelope);
                    if(decode_status != FEB_CBOR_OK) {
                        FURI_LOG_W(TAG, "Rejected pairing record: cbor status %d", decode_status);
                        return BleEventAckFlowEnable;
                    }
                    if(envelope.version != 2 ||
                       !board_id_is_valid(envelope.board_id, envelope.board_id_len)) {
                        FURI_LOG_W(TAG, "Rejected pairing record: bad version or board_id");
                        return BleEventAckFlowEnable;
                    }
                    if(text_matches(envelope.type, envelope.type_len, FEB_PAIR_INIT_TYPE)) {
                        handle_pair_init(profile, &envelope);
                    } else if(text_matches(envelope.type, envelope.type_len, FEB_PAIR_CONFIRM_TYPE)) {
                        handle_pair_confirm(profile, &envelope);
                    } else if(text_matches(envelope.type, envelope.type_len, FEB_PAIR_COMPLETE_TYPE)) {
                        handle_pair_complete(profile, &envelope);
                    } else {
                        FURI_LOG_W(
                            TAG,
                            "Ignoring pairing record with unexpected type '%.*s'",
                            (int)envelope.type_len,
                            envelope.type);
                    }
                } else if(field_count == 5) {
                    static feb_unencrypted_record_t session_envelope;
                    feb_cbor_status_t decode_status =
                        feb_cbor_decode_unencrypted(out_record, out_len, &session_envelope);
                    if(decode_status != FEB_CBOR_OK) {
                        FURI_LOG_W(TAG, "Rejected session record: cbor status %d", decode_status);
                        return BleEventAckFlowEnable;
                    }
                    if(session_envelope.version != 2) {
                        FURI_LOG_W(TAG, "Rejected session record: bad version");
                        return BleEventAckFlowEnable;
                    }
                    if(text_matches(session_envelope.type, session_envelope.type_len, FEB_HELLO_TYPE)) {
                        handle_hello(profile, &session_envelope);
                    } else if(text_matches(
                                  session_envelope.type, session_envelope.type_len, FEB_CLIENT_AUTH_TYPE)) {
                        handle_client_auth(profile, &session_envelope);
                    } else {
                        FURI_LOG_W(
                            TAG,
                            "Ignoring session record with unexpected type '%.*s'",
                            (int)session_envelope.type_len,
                            session_envelope.type);
                    }
                } else {
                    FURI_LOG_W(TAG, "Rejected record: unexpected field count %u", (unsigned)field_count);
                }
                return BleEventAckFlowEnable;
            } else {
                FURI_LOG_W(TAG, "Fragment reassembly rejected: status %d", status);
                return BleEventAckFlowEnable;
            }
        }
    }
    return BleEventNotAck;
}

static FuriHalBleProfileBase* profile_start(FuriHalBleProfileParams params) {
    feb_reassembly_reset(&reassembly);
    pairing_reset_state();
    session_reset_state();
    Esp32BleProfile* profile = malloc(sizeof(Esp32BleProfile));
    furi_check(profile);
    profile->base.config = &profile_callbacks;
    profile->app = params;
    profile->event_handler = ble_event_dispatcher_register_svc_handler(profile_event_handler, profile);
    if(!ble_gatt_service_add(UUID_TYPE_128, &service_uuid, PRIMARY_SERVICE, 6, &profile->service_handle)) {
        ble_event_dispatcher_unregister_svc_handler(profile->event_handler);
        free(profile);
        return NULL;
    }
    FURI_LOG_I(TAG, "GATT service registered: handle=%u", profile->service_handle);
    for(uint8_t index = 0; index < CharacteristicCount; index++) {
        ble_gatt_characteristic_init(
            profile->service_handle, &characteristics[index], &profile->characteristics[index]);
        FURI_LOG_I(
            TAG,
            "GATT characteristic '%s' registered: handle=%u descriptor_handle=%u",
            characteristics[index].name,
            profile->characteristics[index].handle,
            profile->characteristics[index].descriptor_handle);
    }
    return &profile->base;
}

static void profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == &profile_callbacks);
    Esp32BleProfile* profile = (Esp32BleProfile*)base;
    ble_event_dispatcher_unregister_svc_handler(profile->event_handler);
    for(uint8_t index = 0; index < CharacteristicCount; index++) {
        ble_gatt_characteristic_delete(profile->service_handle, &profile->characteristics[index]);
    }
    ble_gatt_service_delete(profile->service_handle);
    free(profile);
}

static const GapConfig profile_gap_config = {
    .adv_service = {.UUID_Type = UUID_TYPE_128, .Service_UUID_128 =
                        {0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31, 0x9e, 0xa2, 0x58, 0xa7,
                         0xa2, 0x0f, 0xb8, 0x11}},
    .appearance_char = 0x0000,
    .bonding_mode = false,
    .pairing_method = GapPairingNone,
    .conn_param = {.conn_int_min = 6, .conn_int_max = 36, .slave_latency = 0, .supervisor_timeout = 0}};

static void profile_get_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    memcpy(config, &profile_gap_config, sizeof(profile_gap_config));
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->adv_name[0] = '\0';
    config->adv_name[1] = '\0';
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = profile_start, .stop = profile_stop, .get_gap_config = profile_get_gap_config};

static void bt_status_callback(BtStatus status, void* context) {
    Esp32App* app = context;
    FURI_LOG_I(TAG, "Bluetooth status: %d", status);
    AppEvent event = {.type = AppEventBtStatus, .bt_status = status};
    furi_message_queue_put(app->queue, &event, 0);
}

static const char* pairing_phase_text(PairingPhase phase) {
    switch(phase) {
    case PairingPhaseWaiting:
        return "Waiting for ESP32...";
    case PairingPhaseExchanging:
        return "Connected, exchanging keys...";
    case PairingPhaseConfirming:
        return "Confirming...";
    case PairingPhaseSaving:
        return "Saving...";
    case PairingPhaseAuthenticating:
        return "Authenticating...";
    case PairingPhaseSessionActive:
        return "ESP32 session active";
    case PairingPhaseDone:
        return "Paired";
    case PairingPhaseFailed:
        return NULL;
    case PairingPhaseNone:
    default:
        return "OK: start pair/connect";
    }
}

static void draw_callback(Canvas* canvas, void* context) {
    Esp32App* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "ESP32 over BLE");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 27, app->has_saved_pairing ? "Have saved pairing" : "No saved pairing");
    if(app->pairing_phase == PairingPhaseFailed) {
        char line[48];
        snprintf(line, sizeof(line), "Failed: %s", app->pairing_reason);
        canvas_draw_str(canvas, 2, 40, line);
    } else {
        canvas_draw_str(canvas, 2, 40, pairing_phase_text(app->pairing_phase));
    }
    canvas_draw_str(canvas, 2, 53, "Back: exit");
}

static void input_callback(InputEvent* input, void* context) {
    Esp32App* app = context;
    AppEvent event = {.type = AppEventInput, .input = *input};
    furi_message_queue_put(app->queue, &event, 0);
}

static void stop_service(Esp32App* app) {
    furi_timer_stop(reassembly_timeout_timer);
    furi_hal_bt_stop_advertising();
    bt_disconnect(app->bt);
    if(app->profile) {
        furi_check(bt_profile_restore_default(app->bt));
        app->profile = NULL;
    }
    pairing_reset_state();
    session_reset_state();
    if(app->notifications) {
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_reset_blue);
    }
    app->pairing_phase = PairingPhaseNone;
}

/* Shared by the explicit OK-press path (no saved pairing yet) and the auto-connect path
   (docs/PLAN.md step 6: opening the FAP with any saved pairing record present starts
   advertising immediately, no OK-press needed -- that action is now reserved for the
   genuinely-new-pairing case only). Refuses to start a second profile while one is already
   waiting/active, per this app's existing UX convention -- callers must check
   `!app->profile` themselves; kept explicit at both call sites rather than hidden in here,
   matching how the rest of this file treats profile lifecycle transitions. */
static void start_profile(Esp32App* app) {
    FURI_LOG_I(TAG, "Starting custom BLE profile");
    pairing_reset_state();
    session_reset_state();
    app->profile = bt_profile_start(app->bt, &profile_callbacks, app);
    if(app->profile) {
        FURI_LOG_I(TAG, "Profile started: %p, BT active: %d", app->profile, furi_hal_bt_is_active());
        furi_hal_bt_stop_advertising();
        app->pairing_phase = PairingPhaseWaiting;
        notification_message(app->notifications, &sequence_blink_start_blue);
        furi_timer_start(
            reassembly_timeout_timer, furi_ms_to_ticks(REASSEMBLY_TIMEOUT_CHECK_PERIOD_MS));
        furi_hal_bt_start_advertising();
        FURI_LOG_I(TAG, "Advertising start requested, BT active: %d", furi_hal_bt_is_active());
    } else {
        FURI_LOG_E(TAG, "Unable to start BLE profile");
    }
}

int32_t flipper_esp32_over_ble_app(void* context) {
    UNUSED(context);
    Esp32App app = {
        .queue = furi_message_queue_alloc(8, sizeof(AppEvent)),
        .pairing_phase = PairingPhaseNone,
        .has_saved_pairing = false,
    };
    app.bt = furi_record_open(RECORD_BT);
    app.storage = furi_record_open(RECORD_STORAGE);
    app.notifications = furi_record_open(RECORD_NOTIFICATION);
    /* Defensive: also stop any blink left running by a prior crashed run of this app. */
    notification_message(app.notifications, &sequence_blink_stop);
    notification_message(app.notifications, &sequence_reset_blue);
    /* Must run before any_saved_pairing_exists()/pairing_storage_*() -- see
       resolve_pairings_dir_path()'s top comment. This app's own thread is the only safe
       place to resolve the "/data" alias; every later pairing-file path build reuses the
       cached result instead. */
    pairings_dir_ready = resolve_pairings_dir_path(app.storage);
    if(!pairings_dir_ready) {
        FURI_LOG_E(TAG, "Failed to resolve pairings directory path");
    }
    app.has_saved_pairing = any_saved_pairing_exists(app.storage);
    bt_set_status_changed_callback(app.bt, bt_status_callback, &app);

    reassembly_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(reassembly_mutex);
    reassembly_timeout_timer = furi_timer_alloc(
        reassembly_timeout_timer_callback, FuriTimerTypePeriodic, NULL);
    furi_check(reassembly_timeout_timer);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, &app);
    view_port_input_callback_set(view_port, input_callback, &app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    /* docs/PLAN.md step 6: auto-connect when a saved pairing record already exists, no
       OK-press required -- the OK-press action below is reserved for the genuinely-new-
       pairing (no saved record at all) case. */
    if(app.has_saved_pairing) {
        start_profile(&app);
        view_port_update(view_port);
    }

    AppEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app.queue, &event, FuriWaitForever) != FuriStatusOk) continue;
        if(event.type == AppEventBtStatus) {
            if(app.profile) {
                if(event.bt_status == BtStatusConnected) {
                    pairing_reset_state();
                    session_reset_state();
                    app.pairing_phase = PairingPhaseExchanging;
                } else if(event.bt_status == BtStatusAdvertising) {
                    if(app.pairing_phase != PairingPhaseDone) {
                        pairing_reset_state();
                        session_reset_state();
                        notification_message(app.notifications, &sequence_blink_start_blue);
                        app.pairing_phase = PairingPhaseWaiting;
                    }
                } else if(event.bt_status == BtStatusUnavailable) {
                    stop_service(&app);
                }
            }
        } else if(event.type == AppEventPairingPhase) {
            app.pairing_phase = event.pairing_phase;
            if(event.pairing_phase == PairingPhaseFailed) {
                strncpy(app.pairing_reason, event.pairing_reason, sizeof(app.pairing_reason) - 1);
                app.pairing_reason[sizeof(app.pairing_reason) - 1] = '\0';
            } else if(event.pairing_phase == PairingPhaseDone) {
                app.has_saved_pairing = true;
            }
        } else if(event.type == AppEventInput && event.input.type == InputTypeShort) {
            if(event.input.key == InputKeyBack) {
                running = false;
            } else if(event.input.key == InputKeyOk && !app.profile) {
                start_profile(&app);
            }
        }
        view_port_update(view_port);
    }

    bt_set_status_changed_callback(app.bt, NULL, NULL);
    if(app.profile) stop_service(&app);
    furi_timer_free(reassembly_timeout_timer);
    reassembly_timeout_timer = NULL;
    furi_mutex_free(reassembly_mutex);
    reassembly_mutex = NULL;
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_BT);
    furi_message_queue_free(app.queue);
    return 0;
}

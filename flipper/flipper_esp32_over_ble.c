#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_random.h>
#include <furi_hal_rtc.h>
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
#include <datetime/datetime.h>

#include <stdio.h>
#include <string.h>

#include "framing.h"
#include "cbor_codec.h"
#include "pairing.h"
#include "pairing_crypto.h"
#include "session.h"
#include "wardriving_csv.h"

#define TAG "Esp32OverBle"
#define PAYLOAD_MAX 64
/* Default (pre-MTU-negotiation) BLE ATT MTU. Pairing records are exchanged before/around
   MTU negotiation, so outgoing pairing-phase records are always fragmented against this
   conservative worst-case value rather than an MTU that may not have taken effect yet. */
#define FEB_DEFAULT_ATT_MTU 23
#define PAIRING_REASON_MAX_LEN 32
#define PAIRING_DIR_NAME "pairings"
#define FEB_PAIRINGS_PATH_MAX_LEN 96
/* docs/PLAN.md step 7: capability-cache file, own subdirectory next to (not inside)
   "pairings", same atomic-write pattern, one file per board_id. */
#define CAPABILITY_DIR_NAME "capabilities"
#define FEB_CAPABILITIES_PATH_MAX_LEN 96
/* wardriving WiGLE CSV export directory (docs/CAPABILITIES.md's wardriving bullet), own
   subdirectory next to "pairings"/"capabilities", same resolve-once-from-this-app's-own-
   thread pattern -- see resolve_pairings_dir_path()'s comment for why. Unlike those two,
   files here are append-only exports, not atomically-replaced state, so there is no
   "*.tmp"/rename pattern for them (see wardriving_csv_ensure_open()). */
#define WARDRIVING_EXPORT_DIR_NAME "wardriving"
#define FEB_WARDRIVING_EXPORT_PATH_MAX_LEN 96
/* Compact on-screen capability line: "<board>: <features>". Real values today are short
   ("esp32-c6-devkit", "wifi_scan"); sized with modest margin, not FEB_CBOR_MAX_TEXT_LEN's
   full 64 bytes -- a real scrollable capability view is backlogged for when `features`
   actually grows (docs/PLAN.md step 7). */
#define CAPABILITY_BOARD_MAX_LEN 31
#define CAPABILITY_FEATURES_MAX_LEN 40
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

/* docs/PLAN.md's Wi-Fi scan capability follow-on step: manual-trigger-only, results shown
   in a dedicated scrollable view, distinct from the fixed-layout main status screen. */
typedef enum {
    AppScreenHome,
    AppScreenScan,
    AppScreenGps,
    AppScreenSettings,
    AppScreenAbout,
    AppScreenLegacy,
    AppScreenWifiScanResults,
    AppScreenBleScanResults,
    AppScreenWardriving,
} AppScreen;

typedef enum {
    HomeMenuWardriving = 0,
    HomeMenuScan,
    HomeMenuGps,
    HomeMenuSettings,
    HomeMenuAbout,
    HomeMenuLegacy,
    HomeMenuCount,
} HomeMenuItem;

typedef enum {
    ScanMenuWifi = 0,
    ScanMenuBle,
    ScanMenuCount,
} ScanMenuItem;

typedef enum {
    AppEventInput,
    AppEventBtStatus,
    AppEventPairingPhase,
    AppEventSessionFatal,
    AppEventCapabilityInfo,
    AppEventWifiScanAp,
    AppEventWifiScanDone,
    AppEventWifiScanError,
    AppEventBleScanDevice,
    AppEventBleScanDone,
    AppEventBleScanError,
    /* wardriving (docs/PROTOCOL.md "`wardriving` command and status payloads"): unlike
       wifi_scan/ble_scan, records are not posted one-per-event -- a single `status`(data)
       record can carry up to 32 records (FEB_WARDRIVING_MAX_RECORDS_PER_BATCH), and this
       app's queue only holds 8 events total (furi_message_queue_alloc(8, ...) below), so
       posting one event per record risks silently dropping records past the queue's depth
       under a large/fast backlog drain (the same latent risk already exists for
       wifi_scan/ble_scan's own per-AP/per-device posts, just less likely to bite there given
       their smaller typical result counts and one-shot nature -- see this project's docs/
       PLAN.md Backlog). AppEventWardrivingBatch instead posts ONE summary event per `status`
       record; the CSV file write for every record in the batch happens synchronously inside
       handle_wardriving_status() on the BLE thread itself (see that function), not deferred
       through this queue at all. */
    AppEventWardrivingRunState,
    AppEventWardrivingBatch,
    AppEventWardrivingError,
} AppEventType;

/* wifi_scan per-AP display fields: phy/auth are copied (not aliased) because their source
   (feb_wifi_scan_ap_t, decoded on the BLE thread from a buffer valid only for the duration
   of that one profile_event_handler call) cannot outlive the event post; ssid is sanitized
   to printable ASCII here (docs/PROTOCOL.md: raw bytes on the wire, not guaranteed
   printable/UTF-8) so both this event and the display list downstream always hold a safe,
   NUL-terminated C string. */
typedef struct {
    AppEventType type;
    InputEvent input;
    BtStatus bt_status;
    PairingPhase pairing_phase;
    char pairing_reason[PAIRING_REASON_MAX_LEN];
    char capability_board[CAPABILITY_BOARD_MAX_LEN + 1];
    char capability_features[CAPABILITY_FEATURES_MAX_LEN];
    bool capability_has_wifi_scan;
    char wifi_scan_ap_ssid[FEB_WIFI_SCAN_SSID_MAX_LEN + 1];
    uint8_t wifi_scan_ap_bssid[FEB_WIFI_SCAN_BSSID_LEN];
    int32_t wifi_scan_ap_rssi_dbm;
    uint32_t wifi_scan_ap_channel;
    char wifi_scan_ap_phy[8];
    char wifi_scan_ap_auth[24];
    char wifi_scan_error_message[48];
    bool capability_has_ble_scan;
    uint8_t ble_scan_device_address[FEB_BLE_SCAN_ADDRESS_LEN];
    bool ble_scan_device_has_name;
    char ble_scan_device_name[FEB_BLE_SCAN_NAME_MAX_LEN + 1];
    int32_t ble_scan_device_rssi_dbm;
    char ble_scan_device_addr_type[8];
    char ble_scan_error_message[48];
    bool capability_has_wardriving;
    /* wardriving fields: AppEventWardrivingRunState uses wardriving_running/
       wardriving_is_fresh_start; AppEventWardrivingBatch uses the batch_count, the
       backlog_remaining, and the last_* fields; AppEventWardrivingError uses
       wardriving_error_message. Fields are shared across these three event types (like
       pairing_reason above) rather than a union, matching this struct's existing style. */
    bool wardriving_running;
    bool wardriving_is_fresh_start; /* true only for a real "started" ack -- see this event's
                                        own AppEventType comment; distinguishes a genuine new
                                        capture (reset the on-screen record counter) from a
                                        `busy`-error-inferred "it was already running"
                                        correction (do not reset the counter). */
    uint32_t wardriving_batch_count;
    uint64_t wardriving_backlog_remaining;
    bool wardriving_last_is_ble;
    char wardriving_last_summary[40];
    char wardriving_error_message[48];
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
    bool has_capability_info;
    bool connection_lost;
    char capability_board[CAPABILITY_BOARD_MAX_LEN + 1];
    char capability_features[CAPABILITY_FEATURES_MAX_LEN];
    bool capability_has_wifi_scan;
    AppScreen screen;
    HomeMenuItem home_menu_index;
    size_t home_menu_scroll_offset;
    ScanMenuItem scan_menu_index;
    bool wifi_scan_in_progress;
    bool wifi_scan_complete;
    size_t wifi_scan_scroll_offset;
    char wifi_scan_error_message[48];
    bool capability_has_ble_scan;
    bool ble_scan_in_progress;
    bool ble_scan_complete;
    size_t ble_scan_scroll_offset;
    char ble_scan_error_message[48];
    bool capability_has_wardriving;
    /* wardriving_running_known is false until this connected session has actual evidence
       either way (a "started"/"stopped" ack, or a `busy`/`not_running` error correcting a
       start/stop guess) -- there is no wire query for "is wardriving currently running"
       (docs/PROTOCOL.md has no such message), so on every fresh session this Flipper
       genuinely does not know the ESP32's current run state until it learns it one of those
       ways (docs/LESSONS.md "UI must derive from real state"). */
    bool wardriving_running_known;
    bool wardriving_running;
    uint32_t wardriving_records_this_session;
    uint64_t wardriving_backlog_remaining;
    bool wardriving_last_is_ble;
    char wardriving_last_summary[40];
    char wardriving_error_message[48];
    /* User's source selection for the next `start`, independent of capability_has_wifi_scan/
       ble_scan (the board's own advertised set). Defaults to both (preserves pre-existing
       behavior for anyone who never touches Left/Right on this screen); only meaningful --
       and only togglable -- when the board advertises both sources, since a single-source
       board has no choice to offer (send_wardriving_start_command() already ANDs these
       against capability_has_wifi_scan/ble_scan). Deliberately not reset by
       reset_scan_ui_state() -- a user preference for this app run, not scan-result state. */
    bool wardriving_use_wifi;
    bool wardriving_use_ble;
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
   so each notification carries exactly the fragment's own length over the air.

   context == NULL is not a "sending with no data" case: it's ble_gatt_characteristic_init()
   itself (targets/f7/ble_glue/furi_ble/gatt.c) probing this characteristic's maximum size at
   registration time, via this same callback with data=NULL. Reporting anything less than
   PAYLOAD_MAX here registers the Notify characteristic's max attribute length too small,
   and aci_gatt_update_char_value() then rejects every real notify (BLE_STATUS_INVALID_PARAMS,
   0x92) whose fragment exceeds that registered max -- silently breaking hello_ack and every
   other outbound notification. (Regressed 2026-09-12 by an unverified "fix" for G20 that
   assumed this NULL-context path only mattered for real sends.) */
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
static char capabilities_dir_path[FEB_CAPABILITIES_PATH_MAX_LEN];
static bool capabilities_dir_ready;
static char wardriving_export_dir_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
static bool wardriving_export_dir_ready;

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

/* Same resolve-once-from-this-app's-own-thread rationale as resolve_pairings_dir_path()
   above -- a separate subdirectory, never nested inside "pairings", so an unpair (step 8)
   can delete each independently while still deleting both together as one operation. */
static bool resolve_capabilities_dir_path(Storage* storage) {
    FuriString* resolved = furi_string_alloc_set_str(APP_DATA_PATH(CAPABILITY_DIR_NAME));
    storage_common_resolve_path_and_ensure_app_directory(storage, resolved);
    bool ok = furi_string_size(resolved) < sizeof(capabilities_dir_path);
    if(ok) {
        strncpy(
            capabilities_dir_path, furi_string_get_cstr(resolved), sizeof(capabilities_dir_path) - 1);
        capabilities_dir_path[sizeof(capabilities_dir_path) - 1] = '\0';
    } else {
        FURI_LOG_E(TAG, "Resolved capabilities path too long to cache");
    }
    furi_string_free(resolved);
    if(!ok) {
        return false;
    }

    FS_Error mkdir_err = storage_common_mkdir(storage, capabilities_dir_path);
    if(mkdir_err != FSE_OK && mkdir_err != FSE_EXIST) {
        FURI_LOG_E(TAG, "mkdir capabilities dir failed: %d", mkdir_err);
        return false;
    }
    return true;
}

static bool build_capability_path(
    char* out,
    size_t out_cap,
    const char* board_id,
    size_t board_id_len,
    bool tmp) {
    if(!capabilities_dir_ready) {
        return false;
    }
    int written = snprintf(
        out,
        out_cap,
        "%s/%.*s%s",
        capabilities_dir_path,
        (int)board_id_len,
        board_id,
        tmp ? ".dat.tmp" : ".dat");
    return written > 0 && (size_t)written < out_cap;
}

/* Same resolve-once-from-this-app's-own-thread rationale as resolve_pairings_dir_path()
   above. Unlike the pairings/capabilities directories, this one holds append-only CSV export
   files (wardriving_csv_ensure_open() below), not atomically-replaced per-board state -- no
   board_id-keyed path builder is needed here, since export files are named by timestamp, not
   by board. */
static bool resolve_wardriving_export_dir_path(Storage* storage) {
    FuriString* resolved = furi_string_alloc_set_str(APP_DATA_PATH(WARDRIVING_EXPORT_DIR_NAME));
    storage_common_resolve_path_and_ensure_app_directory(storage, resolved);
    bool ok = furi_string_size(resolved) < sizeof(wardriving_export_dir_path);
    if(ok) {
        strncpy(
            wardriving_export_dir_path,
            furi_string_get_cstr(resolved),
            sizeof(wardriving_export_dir_path) - 1);
        wardriving_export_dir_path[sizeof(wardriving_export_dir_path) - 1] = '\0';
    } else {
        FURI_LOG_E(TAG, "Resolved wardriving export path too long to cache");
    }
    furi_string_free(resolved);
    if(!ok) {
        return false;
    }

    FS_Error mkdir_err = storage_common_mkdir(storage, wardriving_export_dir_path);
    if(mkdir_err != FSE_OK && mkdir_err != FSE_EXIST) {
        FURI_LOG_E(TAG, "mkdir wardriving export dir failed: %d", mkdir_err);
        return false;
    }
    return true;
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
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            size_t len = strlen(name);
            if(len > 4 && strcmp(name + len - 4, ".dat") == 0) {
                found = true;
                break;
            }
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    return found;
}

static bool
    capability_storage_exists(Storage* storage, const char* board_id, size_t board_id_len) {
    static char path[96];
    if(!build_capability_path(path, sizeof(path), board_id, board_id_len, false)) {
        return false;
    }
    return storage_file_exists(storage, path);
}

/* Persists the raw canonical-CBOR `capability_response` payload bytes verbatim (docs/
   CAPABILITIES.md "Storage and persistence") -- same atomic temp-file/exact-write/
   storage_file_sync()/close/rename sequence as pairing_storage_save(), but a variable
   length rather than a fixed FEB_PAIRING_SECRET_LEN. */
static bool capability_storage_save(
    Storage* storage,
    const char* board_id,
    size_t board_id_len,
    const uint8_t* payload,
    size_t payload_len) {
    static char final_path[96];
    static char tmp_path[96];
    if(!build_capability_path(final_path, sizeof(final_path), board_id, board_id_len, false) ||
       !build_capability_path(tmp_path, sizeof(tmp_path), board_id, board_id_len, true)) {
        return false;
    }

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, tmp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        size_t written = storage_file_write(file, payload, payload_len);
        ok = (written == payload_len) && storage_file_sync(file);
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
        FURI_LOG_E(TAG, "rename capability file failed: %d", rename_err);
        storage_common_remove(storage, tmp_path);
        return false;
    }
    return true;
}

/* Loads a previously-cached capability_response payload for `board_id` into `out` (capacity
   `out_cap`, callers pass FEB_CBOR_MAX_PAYLOAD). Returns false uniformly for "no file",
   "unreadable", and "too large for out_cap" -- matches pairing_storage_load()'s
   don't-expose-the-cause convention, though this cache is non-sensitive. */
static bool capability_storage_load(
    Storage* storage,
    const char* board_id,
    size_t board_id_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len) {
    static char path[96];
    if(!build_capability_path(path, sizeof(path), board_id, board_id_len, false)) {
        return false;
    }
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        uint64_t size = storage_file_size(file);
        if(size == 0 || size > out_cap) {
            ok = false;
        } else {
            size_t read = storage_file_read(file, out, (size_t)size);
            ok = (read == (size_t)size);
            if(ok) {
                *out_len = read;
            }
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/* docs/PLAN.md step 7 grew AppEvent past this file's ~100-byte static-storage threshold
   (added capability_board/capability_features) -- event is now static, not stack-local, to
   keep it off the 1280-byte BleEventWorker stack (this function is reachable from
   profile_event_handler via the handle_pair_ and handle_hello/handle_client_auth
   callbacks). A static local with a designated initializer only runs that initializer once
   at program load, not per call (docs/SESSION_MEMORY.md's cmult() trap), so every field is
   explicitly reset here instead. */
static void post_pairing_phase(Esp32App* app, PairingPhase phase, const char* reason) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventPairingPhase;
    event.pairing_phase = phase;
    if(reason) {
        strncpy(event.pairing_reason, reason, sizeof(event.pairing_reason) - 1);
        event.pairing_reason[sizeof(event.pairing_reason) - 1] = '\0';
    } else {
        event.pairing_reason[0] = '\0';
    }
    furi_message_queue_put(app->queue, &event, 0);
}

static void post_session_fatal(Esp32App* app) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventSessionFatal;
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
/* docs/PLAN.md step 7: per-direction protected-record sequence counters, each beginning at
   1 for the session (docs/PROTOCOL.md#cryptographic-requirements). Not secret, so plain
   reset (not feb_secure_zero) is fine. */
static uint64_t session_seq_out;
static uint64_t session_seq_in;

/* docs/PROTOCOL.md: "must never wrap. A new BLE session is required before 2^24 - 1
   protected records are sent." feb_session_build_nonce() truncates sequence to its low
   24 bits and does not itself enforce this cap (session.h) -- both directions of a single
   session must stay strictly below this value or the AES-GCM nonce repeats. */
#define FEB_SESSION_SEQUENCE_MAX 0xFFFFFFu

/* feb_session_decrypt_record()'s plaintext output; its contract requires capacity >=
   FEB_CBOR_MAX_PAYLOAD (session.h) -- not shrunk to "today's actual capability_response
   size" on purpose, matching the project's own 256-vs-512 lesson (docs/PLAN.md step 3
   backlog) about payload buffers silently rejecting a legitimate larger record later. */
static uint8_t session_plaintext_buf[FEB_CBOR_MAX_PAYLOAD];
/* capability_query's own plaintext payload and its GCM ciphertext scratch: this firmware
   always sends an empty map (`requested` omitted, docs/PLAN.md step 7), so a few bytes of
   margin over the 1-byte real encoding is enough -- sized to what's actually reachable
   here, not FEB_CBOR_MAX_PAYLOAD's full 512 (docs/SESSION_MEMORY.md's static-buffer
   sizing guidance). */
#define FEB_CAPABILITY_QUERY_PAYLOAD_MAX_LEN 16u
static uint8_t capability_query_payload_buf[FEB_CAPABILITY_QUERY_PAYLOAD_MAX_LEN];
static uint8_t capability_query_ciphertext_buf[FEB_CAPABILITY_QUERY_PAYLOAD_MAX_LEN];

/* ---- wifi_scan capability (docs/PLAN.md's Wi-Fi scan capability follow-on step) ----
   Unlike every other outbound protected record in this file (sent synchronously from
   inside a BLE-thread callback, in direct response to an incoming record), the `command`
   that triggers a scan is sent from this app's own main thread, in direct response to a
   user OK-press on the results screen -- there is no incoming BLE event to key it off of.
   Dedicated scratch buffers (separate from pairing_record_buf/capability_query_*_buf, which
   remain BLE-thread-only) avoid any aliasing between the two independent senders, even
   though in practice they cannot run concurrently: the "Scan now" action is gated on
   app.capability_has_wifi_scan, which can only become true after capability_bootstrap()'s
   own send (if any, on the BLE thread) has already returned and its response has been
   processed -- see send_wifi_scan_command()'s own comment below for the full argument. */
/* map(1) + "capability" key(1+10) + "wifi_scan" value(1+9) + "request_id" key(1+10) +
   uint value(1-9) + "arguments" key(1+9) + empty-map value(1) = 45-53 bytes worst case.
   The original 32u only counted value bytes, forgetting the three CBOR map *key* text
   strings entirely -- feb_cbor_encode_command_payload() silently returned 0 (out_cap
   exhausted partway through encoding "request_id"'s key) on every single call, so
   send_wifi_scan_command() failed 100% of the time (hardware-verified 2026-09-07: OK-press
   never reached the ESP32). Sized with real margin now, not shaved to the byte. */
#define FEB_WIFI_SCAN_CMD_PAYLOAD_MAX_LEN 64u
static uint8_t wifi_scan_cmd_payload_buf[FEB_WIFI_SCAN_CMD_PAYLOAD_MAX_LEN];
static uint8_t wifi_scan_cmd_ciphertext_buf[FEB_WIFI_SCAN_CMD_PAYLOAD_MAX_LEN];
static uint8_t wifi_scan_cmd_record_buf[FEB_MAX_RECORD_SIZE];
static uint64_t wifi_scan_next_request_id = 1;

/* ble_scan mirrors wifi_scan's command scratch buffers exactly -- same sizing rationale
   (see FEB_WIFI_SCAN_CMD_PAYLOAD_MAX_LEN's comment above), "ble_scan" (8 bytes) being one
   byte shorter than "wifi_scan" (9 bytes) leaves even more margin against the same 64-byte
   cap. Kept as its own dedicated set of statics, not shared with wifi_scan's, for the same
   independent-sender reasoning given above. */
#define FEB_BLE_SCAN_CMD_PAYLOAD_MAX_LEN 64u
static uint8_t ble_scan_cmd_payload_buf[FEB_BLE_SCAN_CMD_PAYLOAD_MAX_LEN];
static uint8_t ble_scan_cmd_ciphertext_buf[FEB_BLE_SCAN_CMD_PAYLOAD_MAX_LEN];
static uint8_t ble_scan_cmd_record_buf[FEB_MAX_RECORD_SIZE];
static uint64_t ble_scan_next_request_id = 1;

/* `error` records (busy/not_running/invalid_command) carry no capability field
   (docs/PROTOCOL.md) -- only one manual command (wifi_scan, ble_scan, or a wardriving
   start/stop) can be in flight at a time (each gates its own trigger on its own
   *_in_progress-equivalent state, and the ESP32-side busy/not_running rules enforce the same
   radio-sharing serialization). This records which command was most recently sent, so
   handle_runtime_error() (further below) can route an incoming `error` record to the right
   capability's error display -- and, for wifi_scan/ble_scan specifically, so the BLE thread's
   `status` dispatch can pick between their two otherwise-identical "partial"/"complete"
   states (wardriving's own states are unambiguous by text alone -- see profile_event_handler's
   status routing -- so this flag is never consulted for a wardriving status). Written only by
   send_wifi_scan_command()/send_ble_scan_command()/send_wardriving_start_command()/
   send_wardriving_stop_command(), on this app's own main thread, immediately before the send
   that could provoke a reply; read only by the BLE thread once that reply actually arrives.
   This is the same cross-thread-without-a-lock argument send_wifi_scan_command()'s own
   comment already makes for session_key/session_seq_out: a reply cannot physically arrive
   before the send that provoked it has returned on this thread, so there is no window where
   both threads touch this flag at once. */
typedef enum {
    PendingCommandNone,
    PendingCommandWifiScan,
    PendingCommandBleScan,
    PendingCommandWardrivingStart,
    PendingCommandWardrivingStop,
} PendingCommandKind;
static PendingCommandKind pending_command_kind = PendingCommandNone;

/* Per-AP display state, accumulated across one or more `status` records for the results
   view. Not reachable from profile_event_handler (BLE-thread callbacks only ever post one
   AP's worth of data at a time through app->queue -- see post_wifi_scan_ap() below), but
   kept static and off the stack-resident Esp32App struct anyway: this app's own main-thread
   stack size isn't documented/pinned anywhere in this project (unlike BleEventWorker's
   1280 bytes), so a several-KB array (32 entries) is treated with the same caution rather
   than assumed safe as a local/struct-member. */
#define WIFI_SCAN_SSID_DISPLAY_LEN (FEB_WIFI_SCAN_SSID_MAX_LEN + 1)
#define WIFI_SCAN_PHY_DISPLAY_LEN 8
#define WIFI_SCAN_AUTH_DISPLAY_LEN 24
#define WIFI_SCAN_MAX_DISPLAY_APS FEB_WIFI_SCAN_MAX_APS_PER_RECORD

typedef struct {
    char ssid[WIFI_SCAN_SSID_DISPLAY_LEN];
    uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN];
    int32_t rssi_dbm;
    uint32_t channel;
    char phy[WIFI_SCAN_PHY_DISPLAY_LEN];
    char auth[WIFI_SCAN_AUTH_DISPLAY_LEN];
} WifiScanApDisplay;

static WifiScanApDisplay wifi_scan_aps[WIFI_SCAN_MAX_DISPLAY_APS];
static size_t wifi_scan_ap_count;

/* Per-device display state for ble_scan, mirroring wifi_scan_aps/wifi_scan_ap_count above --
   same off-stack-struct/static rationale (this app's own main-thread stack size isn't
   documented/pinned), populated ONLY from the main loop's event handler, never touched
   directly from the BLE thread. */
#define BLE_SCAN_NAME_DISPLAY_LEN (FEB_BLE_SCAN_NAME_MAX_LEN + 1)
#define BLE_SCAN_ADDR_TYPE_DISPLAY_LEN 8
#define BLE_SCAN_MAX_DISPLAY_DEVICES FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD

typedef struct {
    uint8_t address[FEB_BLE_SCAN_ADDRESS_LEN];
    bool has_name;
    char name[BLE_SCAN_NAME_DISPLAY_LEN];
    int32_t rssi_dbm;
    char addr_type[BLE_SCAN_ADDR_TYPE_DISPLAY_LEN];
} BleScanDeviceDisplay;

static BleScanDeviceDisplay ble_scan_devices[BLE_SCAN_MAX_DISPLAY_DEVICES];
static size_t ble_scan_device_count;

/* Solid-green-while-flushing / solid-blue-when-idle LED indicator for an active wardriving
   backlog flush (docs/PROTOCOL.md's backlog_remaining semantics) -- see
   handle_wardriving_status()'s "data" branch, further below, for both transition points.
   Declared here (rather than grouped with the other wardriving-status statics further down,
   next to wardriving_csv_write_failed) because session_reset_state(), which must clear it,
   is defined earlier in this file than that group. */
static bool wardriving_flush_led_active;

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
    session_seq_out = 0;
    session_seq_in = 0;
    wardriving_flush_led_active = false;
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

/* ---- board identity / capability registry (docs/PLAN.md step 7) ----
   Fires automatically the moment runtime auth succeeds (handle_client_auth() below), no UI
   gesture. `session_plaintext_buf`/`capability_query_payload_buf`/
   `capability_query_ciphertext_buf` are declared with this file's other session statics
   above; safe as static for the same single-in-flight-BLE-event-dispatch reason. */

static void format_capability_display(
    const feb_capability_response_payload_t* payload,
    char* board_out,
    size_t board_out_cap,
    char* features_out,
    size_t features_out_cap) {
    size_t board_len = payload->board_len;
    if(board_len > board_out_cap - 1) {
        board_len = board_out_cap - 1;
    }
    memcpy(board_out, payload->board, board_len);
    board_out[board_len] = '\0';

    size_t pos = 0;
    for(size_t i = 0; i < payload->feature_count && pos < features_out_cap - 1; i++) {
        if(i > 0 && pos < features_out_cap - 1) {
            features_out[pos++] = ',';
        }
        size_t copy_len = payload->feature_lens[i];
        if(copy_len > features_out_cap - 1 - pos) {
            copy_len = features_out_cap - 1 - pos;
        }
        memcpy(features_out + pos, payload->features[i], copy_len);
        pos += copy_len;
    }
    features_out[pos] = '\0';
}

static bool capability_has_feature(const feb_capability_response_payload_t* payload, const char* feature) {
    size_t feature_len = strlen(feature);
    for(size_t i = 0; i < payload->feature_count; i++) {
        if(payload->feature_lens[i] == feature_len &&
           memcmp(payload->features[i], feature, feature_len) == 0) {
            return true;
        }
    }
    return false;
}

static void post_capability_info(Esp32App* app, const feb_capability_response_payload_t* payload) {
    /* static, not stack-local -- see post_pairing_phase()'s comment above; same rationale
       and same reset-every-call requirement apply here. */
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventCapabilityInfo;
    format_capability_display(
        payload,
        event.capability_board,
        sizeof(event.capability_board),
        event.capability_features,
        sizeof(event.capability_features));
    event.capability_has_wifi_scan = capability_has_feature(payload, "wifi_scan");
    event.capability_has_ble_scan = capability_has_feature(payload, "ble_scan");
    event.capability_has_wardriving = capability_has_feature(payload, "wardriving");
    furi_message_queue_put(app->queue, &event, 0);
}

/* Persists the just-received capability_response payload verbatim (docs/CAPABILITIES.md)
   and posts it for display. `plaintext`/`plaintext_len` alias session_plaintext_buf,
   valid only until the next BLE event is dispatched -- both the decode and the storage
   write below happen synchronously before that can occur. */
static void handle_capability_response(
    Esp32BleProfile* profile,
    const uint8_t* plaintext,
    size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_capability_response_payload_t response;
    feb_cbor_status_t status =
        feb_cbor_decode_capability_response_payload(plaintext, plaintext_len, &response);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "capability_response payload decode failed: %d; dropping", status);
        return;
    }
    if(!capability_storage_save(app->storage, session_board_id, session_board_id_len, plaintext, plaintext_len)) {
        FURI_LOG_E(TAG, "Failed to persist capability record for board '%s'", session_board_id);
    }
    post_capability_info(app, &response);
    FURI_LOG_I(TAG, "capability_response cached for board '%s'", session_board_id);
}

/* Sends capability_query (requested omitted, docs/PLAN.md step 7) only the first time
   runtime auth succeeds for a board with no locally cached capability record yet; loads and
   displays the cached record instead when one already exists. No retry/timeout of its own
   on send/decode failure -- a dropped or malformed response is simply retried on the next
   reconnect (docs/PLAN.md step 7 implementation-level decisions). */
static void capability_bootstrap(Esp32BleProfile* profile) {
    Esp32App* app = profile->app;
    if(capability_storage_exists(app->storage, session_board_id, session_board_id_len)) {
        size_t loaded_len = 0;
        if(capability_storage_load(
               app->storage,
               session_board_id,
               session_board_id_len,
               session_plaintext_buf,
               sizeof(session_plaintext_buf),
               &loaded_len)) {
            static feb_capability_response_payload_t cached;
            if(feb_cbor_decode_capability_response_payload(session_plaintext_buf, loaded_len, &cached) ==
               FEB_CBOR_OK) {
                post_capability_info(app, &cached);
            } else {
                FURI_LOG_W(TAG, "Cached capability file for '%s' is malformed", session_board_id);
            }
        }
        return;
    }

    static feb_capability_query_payload_t query_payload;
    query_payload.has_requested = 0;
    size_t payload_len = feb_cbor_encode_capability_query_payload(
        capability_query_payload_buf, sizeof(capability_query_payload_buf), &query_payload);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "capability_query: payload encode failed");
        return;
    }
    if(session_seq_out >= FEB_SESSION_SEQUENCE_MAX) {
        FURI_LOG_W(TAG, "capability_query: session sequence at cap; reconnect required");
        return;
    }
    size_t record_len = feb_session_encrypt_record(
        session_key,
        2,
        "capability_query",
        sizeof("capability_query") - 1,
        session_id_bytes,
        session_board_id,
        session_board_id_len,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
        session_seq_out,
        capability_query_payload_buf,
        payload_len,
        capability_query_ciphertext_buf,
        sizeof(capability_query_ciphertext_buf),
        pairing_record_buf,
        sizeof(pairing_record_buf));
    if(record_len == 0) {
        FURI_LOG_W(TAG, "capability_query: record encode failed");
        return;
    }
    if(!send_pairing_record(profile, pairing_record_buf, record_len)) {
        FURI_LOG_W(TAG, "capability_query: send failed");
        return;
    }
    session_seq_out++;
    FURI_LOG_I(TAG, "capability_query sent for board '%s'", session_board_id);
}

/* ---- wifi_scan capability (docs/PLAN.md's Wi-Fi scan capability follow-on step) ---- */

/* Copies up to `dst_cap - 1` bytes of a non-NUL-terminated text span (as returned by the
   cbor_codec decoders -- phy/auth alias the decode buffer, not a C string) into `dst`,
   NUL-terminating. */
static void copy_clamped_text(char* dst, size_t dst_cap, const char* src, size_t src_len) {
    size_t n = src_len > dst_cap - 1 ? dst_cap - 1 : src_len;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Sanitizes and posts one decoded AP for display -- ssid is raw bytes on the wire (docs/
   PROTOCOL.md: "not guaranteed valid UTF-8"), so every non-printable-ASCII byte is replaced
   with '.' here, once, rather than deferring sanitization to every later draw call. */
static void post_wifi_scan_ap(Esp32App* app, const feb_wifi_scan_ap_t* ap) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventWifiScanAp;
    size_t ssid_len = ap->ssid_len > FEB_WIFI_SCAN_SSID_MAX_LEN ? FEB_WIFI_SCAN_SSID_MAX_LEN : ap->ssid_len;
    for(size_t i = 0; i < ssid_len; i++) {
        uint8_t b = ap->ssid[i];
        event.wifi_scan_ap_ssid[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
    }
    event.wifi_scan_ap_ssid[ssid_len] = '\0';
    memcpy(event.wifi_scan_ap_bssid, ap->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    event.wifi_scan_ap_rssi_dbm = (int32_t)ap->rssi_offset - 128;
    event.wifi_scan_ap_channel = (uint32_t)ap->channel;
    copy_clamped_text(event.wifi_scan_ap_phy, sizeof(event.wifi_scan_ap_phy), ap->phy, ap->phy_len);
    copy_clamped_text(event.wifi_scan_ap_auth, sizeof(event.wifi_scan_ap_auth), ap->auth, ap->auth_len);
    furi_message_queue_put(app->queue, &event, 0);
}

static void post_wifi_scan_complete(Esp32App* app) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventWifiScanDone;
    furi_message_queue_put(app->queue, &event, 0);
}

static void post_wifi_scan_error(Esp32App* app, const char* message) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventWifiScanError;
    strncpy(event.wifi_scan_error_message, message, sizeof(event.wifi_scan_error_message) - 1);
    furi_message_queue_put(app->queue, &event, 0);
}

/* `status` (docs/PROTOCOL.md's "`wifi_scan` command and status payloads"). `result` is
   generic at the outer codec layer (see cbor_codec.h) -- decoded further here, since this
   is the only capability that currently exists, into the wifi_scan-specific `{"aps": [...]}`
   shape. An unrecognized `state` (neither "partial" nor "complete") is dropped: the generic
   status codec does not validate that string, by design (see its header comment), so this
   dispatch layer is where PROTOCOL.md's two-state contract is actually enforced. */
static void
    handle_wifi_scan_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    feb_cbor_status_t status = feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "wifi_scan status payload decode failed: %d; dropping", status);
        return;
    }
    bool is_partial = text_matches(status_payload.state, status_payload.state_len, "partial");
    bool is_complete = text_matches(status_payload.state, status_payload.state_len, "complete");
    if(!is_partial && !is_complete) {
        FURI_LOG_W(
            TAG,
            "wifi_scan status: unexpected state '%.*s'; dropping",
            (int)status_payload.state_len,
            status_payload.state);
        return;
    }
    if(status_payload.has_result) {
        static feb_wifi_scan_result_payload_t result;
        feb_cbor_status_t result_status = feb_cbor_decode_wifi_scan_result_payload(
            status_payload.result_span, status_payload.result_span_len, &result);
        if(result_status != FEB_CBOR_OK) {
            FURI_LOG_W(TAG, "wifi_scan status.result decode failed: %d; dropping", result_status);
            return;
        }
        for(size_t i = 0; i < result.ap_count; i++) {
            post_wifi_scan_ap(app, &result.aps[i]);
        }
    }
    if(is_complete) {
        pending_command_kind = PendingCommandNone;
        post_wifi_scan_complete(app);
    }
}

/* ---- ble_scan capability (mirrors wifi_scan capability above, docs/PROTOCOL.md's
   "`ble_scan` command and status payloads") ---- */

/* Sanitizes and posts one decoded device for display -- `name`, while declared as a CBOR
   text string on the wire (docs/PROTOCOL.md), is still peer-controlled data with no
   structural guarantee every byte is printable/renderable by this canvas's font, so the same
   non-printable-ASCII-to-'.' treatment post_wifi_scan_ap() gives `ssid` is applied here too,
   once, rather than deferring sanitization to every later draw call. */
static void post_ble_scan_device(Esp32App* app, const feb_ble_scan_device_t* device) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventBleScanDevice;
    memcpy(event.ble_scan_device_address, device->address, FEB_BLE_SCAN_ADDRESS_LEN);
    event.ble_scan_device_has_name = device->has_name;
    if(device->has_name) {
        size_t name_len =
            device->name_len > FEB_BLE_SCAN_NAME_MAX_LEN ? FEB_BLE_SCAN_NAME_MAX_LEN : device->name_len;
        for(size_t i = 0; i < name_len; i++) {
            uint8_t b = (uint8_t)device->name[i];
            event.ble_scan_device_name[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
        }
        event.ble_scan_device_name[name_len] = '\0';
    }
    event.ble_scan_device_rssi_dbm = (int32_t)device->rssi_offset - 128;
    copy_clamped_text(
        event.ble_scan_device_addr_type,
        sizeof(event.ble_scan_device_addr_type),
        device->addr_type,
        device->addr_type_len);
    furi_message_queue_put(app->queue, &event, 0);
}

static void post_ble_scan_complete(Esp32App* app) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventBleScanDone;
    furi_message_queue_put(app->queue, &event, 0);
}

static void post_ble_scan_error(Esp32App* app, const char* message) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventBleScanError;
    strncpy(event.ble_scan_error_message, message, sizeof(event.ble_scan_error_message) - 1);
    furi_message_queue_put(app->queue, &event, 0);
}

/* `status` (docs/PROTOCOL.md's "`ble_scan` command and status payloads") -- same two-state
   ("partial"/"complete") contract as wifi_scan, enforced here for the same reason
   handle_wifi_scan_status() enforces it (the generic status codec does not validate `state`,
   by design). `result` decodes to the ble_scan-specific `{"devices": [...]}` shape. */
static void
    handle_ble_scan_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    feb_cbor_status_t status = feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "ble_scan status payload decode failed: %d; dropping", status);
        return;
    }
    bool is_partial = text_matches(status_payload.state, status_payload.state_len, "partial");
    bool is_complete = text_matches(status_payload.state, status_payload.state_len, "complete");
    if(!is_partial && !is_complete) {
        FURI_LOG_W(
            TAG,
            "ble_scan status: unexpected state '%.*s'; dropping",
            (int)status_payload.state_len,
            status_payload.state);
        return;
    }
    if(status_payload.has_result) {
        static feb_ble_scan_result_payload_t result;
        feb_cbor_status_t result_status = feb_cbor_decode_ble_scan_result_payload(
            status_payload.result_span, status_payload.result_span_len, &result);
        if(result_status != FEB_CBOR_OK) {
            FURI_LOG_W(TAG, "ble_scan status.result decode failed: %d; dropping", result_status);
            return;
        }
        for(size_t i = 0; i < result.device_count; i++) {
            post_ble_scan_device(app, &result.devices[i]);
        }
    }
    if(is_complete) {
        pending_command_kind = PendingCommandNone;
        post_ble_scan_complete(app);
    }
}

/* ---- wardriving capability (docs/PROTOCOL.md "`wardriving` command and status payloads",
   docs/CAPABILITIES.md's wardriving bullet) ---- */

static void post_wardriving_run_state(Esp32App* app, bool running, bool is_fresh_start) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventWardrivingRunState;
    event.wardriving_running = running;
    event.wardriving_is_fresh_start = is_fresh_start;
    furi_message_queue_put(app->queue, &event, 0);
}

static void post_wardriving_batch(
    Esp32App* app,
    uint32_t batch_count,
    uint64_t backlog_remaining,
    bool last_is_ble,
    const char* last_summary) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventWardrivingBatch;
    event.wardriving_batch_count = batch_count;
    event.wardriving_backlog_remaining = backlog_remaining;
    event.wardriving_last_is_ble = last_is_ble;
    strncpy(
        event.wardriving_last_summary, last_summary, sizeof(event.wardriving_last_summary) - 1);
    furi_message_queue_put(app->queue, &event, 0);
}

static void post_wardriving_error(Esp32App* app, const char* message) {
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventWardrivingError;
    strncpy(event.wardriving_error_message, message, sizeof(event.wardriving_error_message) - 1);
    furi_message_queue_put(app->queue, &event, 0);
}

/* CSV export file state -- BLE-thread-only (handle_wardriving_status(), further below, runs
   synchronously inside profile_event_handler, the same single-threaded-BLE-dispatch
   assumption every other BLE-callback-only static in this file already relies on; nothing
   outside that call chain touches these, so -- unlike `reassembly` above, which really is
   touched from two threads -- no mutex is needed here). One export file spans one
   authenticated BLE session: opened lazily on the first wardriving record this session sees
   (whether from an unsolicited backlog drain or a live capture after an explicit start), kept
   open and appended to for the rest of the session regardless of any stop/restart within it,
   and closed on disconnect/profile-teardown/app-exit (reset_scan_ui_state(), further below).
   Judgment call: this is simpler than slicing a file per start/stop, and a single connection's
   backlog-drain-then-maybe-live-capture reads naturally as one contiguous export rather than
   several fragments. The per-address dedup table and FirstSeen anchor (docs/CAPABILITIES.md)
   share this exact file-lifetime scope -- reset together with the file in
   wardriving_csv_close() only, never on a same-session "started" ack (former docs/BACKLOG.md
   G29: resetting dedup on every restart made every address still in range look brand-new
   again, defeating the whole policy). */
static File* wardriving_csv_file;
static char wardriving_csv_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
static uint64_t wardriving_csv_anchor_timestamp_ms;
static uint32_t wardriving_csv_anchor_unix_time;
static uint32_t wardriving_csv_records_since_sync;
static bool wardriving_csv_write_failed;
/* wardriving_flush_led_active (the solid-green-while-flushing / solid-blue-when-idle LED
   indicator for an active wardriving backlog flush, docs/PROTOCOL.md's backlog_remaining
   semantics) is declared earlier in this file, next to session_reset_state() which must
   clear it -- see handle_wardriving_status()'s "data" branch, further below, for both
   transition points. */

/* Same BLE-thread-only, single-owner lifetime as the fields above (see wardriving_csv_file's
   own declaration comment) -- see wardriving_csv.h's feb_wardriving_dedup_should_write() for
   the policy this table drives. */
static feb_wardriving_dedup_table_t wardriving_dedup_table;

/* storage_file_sync() every Nth record rather than every record (durability against a mid-
   session power loss vs. flash-write overhead) or only at close (would lose the whole
   session's writes since the last sync on a power loss) -- 8 chosen to match this app's
   existing message-queue depth, no other significance. */
#define FEB_WARDRIVING_CSV_SYNC_EVERY_N_RECORDS 8u

static void wardriving_csv_reset_state(void) {
    wardriving_csv_anchor_timestamp_ms = 0;
    wardriving_csv_anchor_unix_time = 0;
    wardriving_csv_records_since_sync = 0;
    wardriving_csv_write_failed = false;
    feb_wardriving_dedup_reset(&wardriving_dedup_table);
}

static void wardriving_csv_close(void) {
    if(wardriving_csv_file) {
        storage_file_sync(wardriving_csv_file);
        storage_file_close(wardriving_csv_file);
        storage_file_free(wardriving_csv_file);
        wardriving_csv_file = NULL;
    }
    wardriving_csv_reset_state();
}

/* Filename is timestamped at creation (docs/CAPABILITIES.md: "one timestamped file per flush
   session"); FSOM_OPEN_APPEND creates-if-absent and seeks to EOF, matching this file's
   append-only, not atomically-replaced, write pattern (contrast with pairing_storage_save()'s
   temp-file/rename dance, which does not fit an incrementally-appended, potentially
   hours-long export). */
static bool wardriving_csv_ensure_open(Storage* storage) {
    if(wardriving_csv_file) {
        return true;
    }
    if(!wardriving_export_dir_ready) {
        return false;
    }
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    int written = snprintf(
        wardriving_csv_path,
        sizeof(wardriving_csv_path),
        "%s/wardriving_%04u%02u%02u.csv",
        wardriving_export_dir_path,
        (unsigned)now.year,
        (unsigned)now.month,
        (unsigned)now.day);
    if(written <= 0 || (size_t)written >= sizeof(wardriving_csv_path)) {
        FURI_LOG_E(TAG, "wardriving CSV: path build failed");
        return false;
    }

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, wardriving_csv_path, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok) {
        if(storage_file_size(file) == 0) {
            static char header_buf[FEB_WARDRIVING_CSV_HEADER_MAX_LEN];
            size_t header_len = feb_wardriving_csv_format_header(header_buf, sizeof(header_buf));
            ok = header_len > 0 && storage_file_write(file, header_buf, header_len) == header_len;
        }
    }
    if(!ok) {
        FURI_LOG_E(TAG, "wardriving CSV: failed to create '%s'", wardriving_csv_path);
        storage_file_close(file);
        storage_file_free(file);
        return false;
    }
    wardriving_csv_file = file;
    FURI_LOG_I(TAG, "wardriving CSV: writing to '%s'", wardriving_csv_path);
    return true;
}

/* FirstSeen reconstruction (docs/CAPABILITIES.md): tracks the largest timestamp_ms seen so
   far this export session and the Flipper wall-clock time at the moment it was seen, then
   backdates every record (including, trivially, the anchor record itself) from that pair --
   see feb_wardriving_backdate_first_seen()'s own comment (wardriving_csv.h) for the exact
   arithmetic. Records normally arrive in non-decreasing timestamp_ms order (the ESP32's flash
   log is itself sequential), so in practice the anchor advances roughly once per record and
   tracks close to "now" for the most recent data; a real reorder would just mean an older
   anchor briefly persists, backdating slightly less accurately, not a crash or corrupt row. */
static bool
    wardriving_csv_write_record(Storage* storage, const feb_wardriving_record_t* record) {
    if(!wardriving_csv_ensure_open(storage)) {
        return false;
    }
    if(record->timestamp_ms >= wardriving_csv_anchor_timestamp_ms) {
        wardriving_csv_anchor_timestamp_ms = record->timestamp_ms;
        DateTime now;
        furi_hal_rtc_get_datetime(&now);
        wardriving_csv_anchor_unix_time = datetime_datetime_to_timestamp(&now);
    }
    uint32_t first_seen_unix = feb_wardriving_backdate_first_seen(
        record->timestamp_ms, wardriving_csv_anchor_timestamp_ms, wardriving_csv_anchor_unix_time);
    DateTime first_seen_dt;
    datetime_timestamp_to_datetime(first_seen_unix, &first_seen_dt);
    static char first_seen_str[FEB_WARDRIVING_CSV_FIRST_SEEN_LEN];
    snprintf(
        first_seen_str,
        sizeof(first_seen_str),
        "%04u-%02u-%02u %02u:%02u:%02u",
        (unsigned)first_seen_dt.year,
        (unsigned)first_seen_dt.month,
        (unsigned)first_seen_dt.day,
        (unsigned)first_seen_dt.hour,
        (unsigned)first_seen_dt.minute,
        (unsigned)first_seen_dt.second);

    static char row_buf[FEB_WARDRIVING_CSV_ROW_MAX_LEN];
    size_t row_len = feb_wardriving_csv_format_row(
        row_buf, sizeof(row_buf), record, first_seen_str, strlen(first_seen_str));
    if(row_len == 0 || storage_file_write(wardriving_csv_file, row_buf, row_len) != row_len) {
        return false;
    }
    wardriving_csv_records_since_sync++;
    if(wardriving_csv_records_since_sync >= FEB_WARDRIVING_CSV_SYNC_EVERY_N_RECORDS) {
        storage_file_sync(wardriving_csv_file);
        wardriving_csv_records_since_sync = 0;
    }
    return true;
}

/* `status` (docs/PROTOCOL.md "`wardriving` command and status payloads") -- unlike
   wifi_scan/ble_scan's `partial`/`complete` pair, wardriving's own states ("started"/"data"/
   "stopped") are never ambiguous with those or each other by text alone, so no
   pending_command_kind check is needed to route here (see profile_event_handler's status
   dispatch, further below) or within this function. Every state is handled regardless of
   `request_id`, including the `request_id == 0` unsolicited-backlog-drain sentinel
   (docs/PROTOCOL.md "Unsolicited backlog drain") -- this function never inspects
   status_payload.request_id at all, so there is nothing to special-case for it. CSV writes
   for a "data" batch happen synchronously here, one record at a time as each is decoded, on
   the BLE thread itself -- not deferred through app->queue -- so the export file is genuinely
   appended-to incrementally even under an hours-long capture (docs/CAPABILITIES.md), and a
   32-record batch can never overrun the main-thread event queue's depth (see
   AppEventWardrivingBatch's own comment). Each record is first gated through
   feb_wardriving_dedup_should_write() (wardriving_csv.h) -- a redundant repeat of an
   already-written address is skipped before it ever reaches wardriving_csv_write_record(), not
   an error path. */
static void
    handle_wardriving_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_status_payload_t status_payload;
    feb_cbor_status_t status = feb_cbor_decode_status_payload(plaintext, plaintext_len, &status_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "wardriving status payload decode failed: %d; dropping", status);
        return;
    }

    if(text_matches(status_payload.state, status_payload.state_len, "started")) {
        /* Deliberately NOT wardriving_csv_reset_state() here (docs/BACKLOG.md's former G29):
           a manual stop/restart mid-session must not wipe the FirstSeen anchor or the
           per-address dedup table, or every address still in range looks brand-new again
           and the RSSI-improvement/movement policy is defeated. Dedup scope is the CSV
           export file's lifetime (docs/CAPABILITIES.md), same as wardriving_csv_file itself
           -- both are reset together only in wardriving_csv_close(), on disconnect/teardown,
           never on a same-session restart. record->timestamp_ms is esp_timer_get_time()-based
           on the ESP32 (monotonic since its boot, not reset by a start/stop), so the anchor
           staying live across a restart cannot regress or go stale. */
        pending_command_kind = PendingCommandNone;
        post_wardriving_run_state(app, true, true);
        return;
    }
    if(text_matches(status_payload.state, status_payload.state_len, "stopped")) {
        pending_command_kind = PendingCommandNone;
        if(wardriving_csv_file) {
            storage_file_sync(wardriving_csv_file);
        }
        post_wardriving_run_state(app, false, false);
        return;
    }
    if(!text_matches(status_payload.state, status_payload.state_len, "data")) {
        FURI_LOG_W(
            TAG,
            "wardriving status: unexpected state '%.*s'; dropping",
            (int)status_payload.state_len,
            status_payload.state);
        return;
    }
    if(!status_payload.has_result) {
        return;
    }

    static feb_wardriving_status_result_payload_t result;
    feb_cbor_status_t result_status = feb_cbor_decode_wardriving_status_result_payload(
        status_payload.result_span, status_payload.result_span_len, &result);
    if(result_status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "wardriving status.result decode failed: %d; dropping", result_status);
        return;
    }

    if(!wardriving_flush_led_active && app->notifications) {
        notification_message(app->notifications, &sequence_set_only_green_255);
        wardriving_flush_led_active = true;
    }

    bool last_is_ble = false;
    static char last_summary[40];
    last_summary[0] = '\0';
    for(size_t i = 0; i < result.record_count; i++) {
        const feb_wardriving_record_t* record = &result.records[i];
        if(feb_wardriving_dedup_should_write(&wardriving_dedup_table, record)) {
            if(!wardriving_csv_write_failed && !wardriving_csv_write_record(app->storage, record)) {
                wardriving_csv_write_failed = true;
                FURI_LOG_E(
                    TAG, "wardriving CSV: write failed, no further records written this session");
                post_wardriving_error(app, "CSV export write failed");
            }
        }
        last_is_ble = record->payload_kind == FEB_WARDRIVING_PAYLOAD_BLE;
        if(last_is_ble) {
            const feb_wardriving_ble_payload_t* ble = &record->payload.ble;
            snprintf(
                last_summary,
                sizeof(last_summary),
                "%02x:%02x:%02x:%02x:%02x:%02x",
                ble->address[0],
                ble->address[1],
                ble->address[2],
                ble->address[3],
                ble->address[4],
                ble->address[5]);
        } else {
            const feb_wardriving_wifi_payload_t* wifi = &record->payload.wifi;
            size_t n =
                wifi->ssid_len > sizeof(last_summary) - 1 ? sizeof(last_summary) - 1 : wifi->ssid_len;
            for(size_t j = 0; j < n; j++) {
                uint8_t b = wifi->ssid[j];
                last_summary[j] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
            }
            last_summary[n] = '\0';
            if(n == 0) {
                strncpy(last_summary, "(hidden)", sizeof(last_summary) - 1);
                last_summary[sizeof(last_summary) - 1] = '\0';
            }
        }
    }

    post_wardriving_batch(
        app, (uint32_t)result.record_count, result.backlog_remaining, last_is_ble, last_summary);

    if(wardriving_flush_led_active && result.backlog_remaining == 0 && app->notifications) {
        notification_message(app->notifications, &sequence_set_only_blue_255);
        wardriving_flush_led_active = false;
    }
}

/* Protected-record `error` (post-session-establishment shape, docs/PROTOCOL.md "Runtime
   auth failure handling" / message-payloads table) -- surfaced for wifi_scan's/ble_scan's
   `busy` response (docs/PROTOCOL.md's "Busy handling") and wardriving's `busy`/`not_running`/
   `invalid_command` responses (docs/PROTOCOL.md's "Busy/not-running handling"); any other code
   is logged and otherwise ignored. `error` carries no capability field, so
   pending_command_kind (see its own declaration comment above) picks which capability's
   in-flight command this reply belongs to, and is reset back to PendingCommandNone at the
   end of every branch below (also on the wifi_scan/ble_scan "complete" status, the
   wardriving "started"/"stopped" status, and reset_scan_ui_state()) so a stale value from an
   already-finished command can never be misattributed to a later, unrelated reply. A `busy`
   on a wardriving start means it was already running -- corrected here rather than left
   "unknown" (docs/LESSONS.md "UI must derive from real state"); symmetrically, `not_running`
   on a stop confirms it was already stopped. `internal_error` is a wardriving self-stop
   notice (docs/PROTOCOL.md: "an error record accompanies" a proactive `stopped` sent when
   "the engine self-stops for an internal reason") only when a wardriving start/stop is still
   the pending command -- the accompanying "stopped" status (handle_wardriving_status() above)
   always drives the actual run-state update regardless, so this branch only adds the reason
   message. If some other capability's command was pending instead, this reply is routed to
   that capability's own error surface rather than assumed to be about wardriving. */
static void
    handle_runtime_error(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_error_payload_t error_payload;
    feb_cbor_status_t status = feb_cbor_decode_error_payload(plaintext, plaintext_len, &error_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "runtime error payload decode failed: %d; dropping", status);
        return;
    }
    FURI_LOG_W(
        TAG, "runtime error received: code='%.*s'", (int)error_payload.code_len, error_payload.code);
    if(text_matches(error_payload.code, error_payload.code_len, "busy")) {
        if(pending_command_kind == PendingCommandBleScan) {
            post_ble_scan_error(app, "ESP32 busy, try again");
        } else if(pending_command_kind == PendingCommandWifiScan) {
            post_wifi_scan_error(app, "ESP32 busy, try again");
        } else if(pending_command_kind == PendingCommandWardrivingStart) {
            post_wardriving_run_state(app, true, false);
            post_wardriving_error(app, "Already running");
        }
        pending_command_kind = PendingCommandNone;
    } else if(text_matches(error_payload.code, error_payload.code_len, "not_running")) {
        if(pending_command_kind == PendingCommandWardrivingStop) {
            post_wardriving_run_state(app, false, false);
            post_wardriving_error(app, "Already stopped");
        }
        pending_command_kind = PendingCommandNone;
    } else if(text_matches(error_payload.code, error_payload.code_len, "invalid_command")) {
        if(pending_command_kind == PendingCommandWardrivingStart ||
           pending_command_kind == PendingCommandWardrivingStop) {
            post_wardriving_error(app, "Invalid command");
        }
        pending_command_kind = PendingCommandNone;
    } else if(text_matches(error_payload.code, error_payload.code_len, "internal_error")) {
        /* Only a wardriving start/stop still in flight makes this "internal_error" a
           self-stop notice for *this* pending command (docs/PROTOCOL.md's "stopped" state:
           self-stop always also sends its own "stopped" status, which already drives
           post_wardriving_run_state() regardless of pending_command_kind -- see
           handle_wardriving_status() above). Otherwise this reply belongs to whichever
           other capability was actually in flight (or none), so route it through that
           capability's own error surface instead of misattributing it to wardriving. */
        if(pending_command_kind == PendingCommandWardrivingStart ||
           pending_command_kind == PendingCommandWardrivingStop) {
            post_wardriving_run_state(app, false, false);
            post_wardriving_error(app, "Engine stopped (internal error)");
        } else if(pending_command_kind == PendingCommandWifiScan) {
            post_wifi_scan_error(app, "ESP32 internal error");
        } else if(pending_command_kind == PendingCommandBleScan) {
            post_ble_scan_error(app, "ESP32 internal error");
        } else {
            FURI_LOG_W(TAG, "internal_error with no matching pending command; ignoring");
        }
        pending_command_kind = PendingCommandNone;
    }
}

/* Sends the wifi_scan `command` (capability="wifi_scan", fresh request_id, always-empty
   arguments per docs/PROTOCOL.md). Unlike every other sender in this file, this one runs on
   this app's own main thread (triggered by a user Left-press on the main screen, or an
   Ok-press to re-trigger from inside the results screen, in the main loop below), not
   synchronously from inside a BLE-thread callback -- there is no incoming BLE event to key
   it off of, since "start a scan" is a user-initiated action, not a response to the peer.
   This is safe against the `session_key`/`session_seq_out`/`outgoing_message_id` statics
   this function shares with the BLE-thread-driven senders (capability_bootstrap() etc.)
   specifically because every call site gates this function on
   app->capability_has_wifi_scan, which can only become true after processing a real
   capability_response -- and that can only happen strictly after capability_bootstrap()'s
   own send (if it took the query-not-cached branch) has already returned on the BLE thread,
   since the response is itself a later, separate BLE event. If this gating condition is
   ever loosened, this reasoning needs re-examining. */
static bool send_wifi_scan_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    uint8_t arguments_buf[2];
    size_t arguments_len = feb_cbor_encode_map_header(arguments_buf, sizeof(arguments_buf), 0);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "wifi_scan command: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "wifi_scan",
        .capability_len = sizeof("wifi_scan") - 1,
        .request_id = wifi_scan_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        wifi_scan_cmd_payload_buf, sizeof(wifi_scan_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "wifi_scan command: payload encode failed");
        return false;
    }
    if(session_seq_out >= FEB_SESSION_SEQUENCE_MAX) {
        FURI_LOG_W(TAG, "wifi_scan command: session sequence at cap; reconnect required");
        return false;
    }

    size_t record_len = feb_session_encrypt_record(
        session_key,
        2,
        "command",
        sizeof("command") - 1,
        session_id_bytes,
        session_board_id,
        session_board_id_len,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
        session_seq_out,
        wifi_scan_cmd_payload_buf,
        payload_len,
        wifi_scan_cmd_ciphertext_buf,
        sizeof(wifi_scan_cmd_ciphertext_buf),
        wifi_scan_cmd_record_buf,
        sizeof(wifi_scan_cmd_record_buf));
    if(record_len == 0) {
        FURI_LOG_W(TAG, "wifi_scan command: record encode failed");
        return false;
    }
    pending_command_kind = PendingCommandWifiScan;
    if(!send_pairing_record(profile, wifi_scan_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "wifi_scan command: send failed");
        return false;
    }
    session_seq_out++;
    FURI_LOG_I(TAG, "wifi_scan command sent (request_id=%llu)", (unsigned long long)command.request_id);
    return true;
}

/* Sends the ble_scan `command` (capability="ble_scan", fresh request_id, always-empty
   arguments per docs/PROTOCOL.md); mirrors send_wifi_scan_command() above exactly, including
   its main-thread/ordering-safety argument: this also runs on this app's own main thread
   (triggered by a user Right-press on the main screen, or an Ok-press to re-trigger from
   inside the results screen), not synchronously from inside a BLE-thread callback, and is
   safe against the `session_key`/`session_seq_out` statics it shares with the BLE-thread-driven
   senders (capability_bootstrap() etc.) specifically because every call site gates this
   function on app->capability_has_ble_scan, which can only become true after processing a
   real capability_response -- and that can only happen strictly after capability_bootstrap()'s
   own send (if it took the query-not-cached branch) has already returned on the BLE thread,
   since the response is itself a later, separate BLE event. If this gating condition is ever
   loosened, this reasoning needs re-examining. */
static bool send_ble_scan_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    uint8_t arguments_buf[2];
    size_t arguments_len = feb_cbor_encode_map_header(arguments_buf, sizeof(arguments_buf), 0);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "ble_scan command: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "ble_scan",
        .capability_len = sizeof("ble_scan") - 1,
        .request_id = ble_scan_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        ble_scan_cmd_payload_buf, sizeof(ble_scan_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "ble_scan command: payload encode failed");
        return false;
    }
    if(session_seq_out >= FEB_SESSION_SEQUENCE_MAX) {
        FURI_LOG_W(TAG, "ble_scan command: session sequence at cap; reconnect required");
        return false;
    }

    size_t record_len = feb_session_encrypt_record(
        session_key,
        2,
        "command",
        sizeof("command") - 1,
        session_id_bytes,
        session_board_id,
        session_board_id_len,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
        session_seq_out,
        ble_scan_cmd_payload_buf,
        payload_len,
        ble_scan_cmd_ciphertext_buf,
        sizeof(ble_scan_cmd_ciphertext_buf),
        ble_scan_cmd_record_buf,
        sizeof(ble_scan_cmd_record_buf));
    if(record_len == 0) {
        FURI_LOG_W(TAG, "ble_scan command: record encode failed");
        return false;
    }
    pending_command_kind = PendingCommandBleScan;
    if(!send_pairing_record(profile, ble_scan_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "ble_scan command: send failed");
        return false;
    }
    session_seq_out++;
    FURI_LOG_I(TAG, "ble_scan command sent (request_id=%llu)", (unsigned long long)command.request_id);
    return true;
}

/* map(1) + "action"key(1+6)+"start"value(1+5) + "sources"key(1+7)+array header(1)+2 text
   values ("wifi"=1+4,"ble"=1+3) == ~40 bytes worst case; sized with real margin (see
   FEB_WIFI_SCAN_CMD_PAYLOAD_MAX_LEN's own comment for why this project no longer shaves
   these to the byte). */
#define FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN 96u
static uint8_t wardriving_cmd_payload_buf[FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN];
static uint8_t wardriving_cmd_ciphertext_buf[FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN];
static uint8_t wardriving_cmd_record_buf[FEB_MAX_RECORD_SIZE];
static uint64_t wardriving_next_request_id = 1;

/* Sends the wardriving `start` command (docs/PROTOCOL.md "`wardriving` command and status
   payloads"). `sources` is built from whichever of wifi_scan/ble_scan the connected board
   actually advertises AND the user has selected via app->wardriving_use_wifi/wardriving_use_ble
   (the AppScreenWardriving Left/Right toggle, only offered when the board advertises both --
   see draw_wardriving_screen()) -- a source the board doesn't have is rejected
   `invalid_command` per PROTOCOL.md, so this only ever requests a subset of what
   capability_bootstrap() already confirmed present via app->capability_has_wifi_scan/
   ble_scan. Both selection flags default true, so a board with only one source, or a user who
   never touches the toggle, gets the original "every available source" behavior. Interval fields
   (wifi_interval_ms/ble_window_ms/ble_interval_ms) are omitted entirely so the ESP32 applies
   its own documented defaults -- v1 has no interval-entry UI either. wifi_interval_ms/
   ble_window_ms still default to the original most-aggressive/point-4 values; ble_interval_ms
   was raised from point-4's 30ms to 500ms on 2026-09-10 after real wardriving traffic on real
   hardware showed 100% BLE duty starves the connection itself (docs/PROJECT_HISTORY.md).
   Runs on this app's own main thread (OK-press on the wardriving
   screen), same session_key/session_seq_out cross-thread-safety argument as
   send_wifi_scan_command()'s own comment (gated on app->capability_has_wifi_scan/ble_scan,
   which can only become true strictly after capability_bootstrap()'s send, if any, has
   already returned on the BLE thread). */
static bool send_wardriving_start_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    feb_wardriving_command_payload_t command_args;
    memset(&command_args, 0, sizeof(command_args));
    command_args.action = "start";
    command_args.action_len = sizeof("start") - 1;
    command_args.has_sources = 1;
    size_t source_count = 0;
    if(app->capability_has_wifi_scan && app->wardriving_use_wifi) {
        command_args.sources[source_count] = "wifi";
        command_args.source_lens[source_count] = sizeof("wifi") - 1;
        source_count++;
    }
    if(app->capability_has_ble_scan && app->wardriving_use_ble) {
        command_args.sources[source_count] = "ble";
        command_args.source_lens[source_count] = sizeof("ble") - 1;
        source_count++;
    }
    command_args.source_count = source_count;
    if(source_count == 0) {
        FURI_LOG_W(TAG, "wardriving start: no source selected/available");
        return false;
    }

    uint8_t arguments_buf[FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN];
    size_t arguments_len = feb_cbor_encode_wardriving_command_payload(
        arguments_buf, sizeof(arguments_buf), &command_args);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "wardriving start: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "wardriving",
        .capability_len = sizeof("wardriving") - 1,
        .request_id = wardriving_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        wardriving_cmd_payload_buf, sizeof(wardriving_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "wardriving start: payload encode failed");
        return false;
    }
    if(session_seq_out >= FEB_SESSION_SEQUENCE_MAX) {
        FURI_LOG_W(TAG, "wardriving start: session sequence at cap; reconnect required");
        return false;
    }

    size_t record_len = feb_session_encrypt_record(
        session_key,
        2,
        "command",
        sizeof("command") - 1,
        session_id_bytes,
        session_board_id,
        session_board_id_len,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
        session_seq_out,
        wardriving_cmd_payload_buf,
        payload_len,
        wardriving_cmd_ciphertext_buf,
        sizeof(wardriving_cmd_ciphertext_buf),
        wardriving_cmd_record_buf,
        sizeof(wardriving_cmd_record_buf));
    if(record_len == 0) {
        FURI_LOG_W(TAG, "wardriving start: record encode failed");
        return false;
    }
    pending_command_kind = PendingCommandWardrivingStart;
    if(!send_pairing_record(profile, wardriving_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "wardriving start: send failed");
        return false;
    }
    session_seq_out++;
    FURI_LOG_I(
        TAG,
        "wardriving start command sent (request_id=%llu)",
        (unsigned long long)command.request_id);
    return true;
}

/* Sends the wardriving `stop` command (arguments = {action:"stop"} alone, per PROTOCOL.md);
   mirrors send_wardriving_start_command() above, same ordering-safety argument. */
static bool send_wardriving_stop_command(Esp32App* app) {
    if(app->profile == NULL || app->pairing_phase != PairingPhaseSessionActive) {
        return false;
    }
    Esp32BleProfile* profile = (Esp32BleProfile*)app->profile;

    feb_wardriving_command_payload_t command_args;
    memset(&command_args, 0, sizeof(command_args));
    command_args.action = "stop";
    command_args.action_len = sizeof("stop") - 1;

    uint8_t arguments_buf[32];
    size_t arguments_len = feb_cbor_encode_wardriving_command_payload(
        arguments_buf, sizeof(arguments_buf), &command_args);
    if(arguments_len == 0) {
        FURI_LOG_W(TAG, "wardriving stop: arguments encode failed");
        return false;
    }

    feb_command_payload_t command = {
        .capability = "wardriving",
        .capability_len = sizeof("wardriving") - 1,
        .request_id = wardriving_next_request_id++,
        .arguments_span = arguments_buf,
        .arguments_span_len = arguments_len,
    };
    size_t payload_len = feb_cbor_encode_command_payload(
        wardriving_cmd_payload_buf, sizeof(wardriving_cmd_payload_buf), &command);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "wardriving stop: payload encode failed");
        return false;
    }
    if(session_seq_out >= FEB_SESSION_SEQUENCE_MAX) {
        FURI_LOG_W(TAG, "wardriving stop: session sequence at cap; reconnect required");
        return false;
    }

    size_t record_len = feb_session_encrypt_record(
        session_key,
        2,
        "command",
        sizeof("command") - 1,
        session_id_bytes,
        session_board_id,
        session_board_id_len,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
        session_seq_out,
        wardriving_cmd_payload_buf,
        payload_len,
        wardriving_cmd_ciphertext_buf,
        sizeof(wardriving_cmd_ciphertext_buf),
        wardriving_cmd_record_buf,
        sizeof(wardriving_cmd_record_buf));
    if(record_len == 0) {
        FURI_LOG_W(TAG, "wardriving stop: record encode failed");
        return false;
    }
    pending_command_kind = PendingCommandWardrivingStop;
    if(!send_pairing_record(profile, wardriving_cmd_record_buf, record_len)) {
        FURI_LOG_W(TAG, "wardriving stop: send failed");
        return false;
    }
    session_seq_out++;
    FURI_LOG_I(
        TAG,
        "wardriving stop command sent (request_id=%llu)",
        (unsigned long long)command.request_id);
    return true;
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
        post_pairing_phase(profile->app, PairingPhaseFailed, "proof verification failed");
        session_reset_state();
        post_session_fatal(profile->app);
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
    session_seq_out = 1;
    session_seq_in = 1;
    if(profile->app->notifications) {
        /* Must stop the running blink sequence explicitly - it's a separate
           hardware LED-blink subsystem that a plain RGB message can't override. */
        notification_message(profile->app->notifications, &sequence_blink_stop);
        notification_message(profile->app->notifications, &sequence_set_only_blue_255);
    }
    post_pairing_phase(profile->app, PairingPhaseSessionActive, NULL);
    FURI_LOG_I(TAG, "Runtime session authenticated for board '%s'", session_board_id);
    capability_bootstrap(profile);
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
                } else if(field_count == 7) {
                    /* docs/PLAN.md step 7: protected (AES-256-GCM) record, e.g.
                       capability_response. Per docs/PROTOCOL.md, decode/decrypt failure or
                       a session/board_id/sequence mismatch is fatal: dropped silently (no
                       reply) and the connection is closed. This function still can't call
                       bt_disconnect() directly (BLE-thread reentrancy hazard, see this
                       file's existing no-proactive-bt_disconnect() rationale above), so it
                       posts AppEventSessionFatal instead; the main thread's event loop
                       calls bt_disconnect() on receipt. An unrecognized type is not fatal
                       and is still just dropped below. */
                    if(session_stage != SessionStageActive) {
                        FURI_LOG_W(TAG, "Ignoring protected record: no active session");
                        return BleEventAckFlowEnable;
                    }
                    static feb_session_decrypted_record_t decrypted;
                    feb_cbor_status_t decode_status = feb_session_decrypt_record(
                        session_key,
                        out_record,
                        out_len,
                        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
                        session_plaintext_buf,
                        sizeof(session_plaintext_buf),
                        &decrypted);
                    if(decode_status != FEB_CBOR_OK) {
                        FURI_LOG_W(
                            TAG,
                            "Protected record decode/decrypt failed: %d; dropping (no reply)",
                            decode_status);
                        session_reset_state();
                        post_session_fatal(profile->app);
                        return BleEventAckFlowEnable;
                    }
                    if(decrypted.version != 2 ||
                       memcmp(decrypted.session_id, session_id_bytes, FEB_SESSION_ID_LEN) != 0 ||
                       decrypted.board_id_len != session_board_id_len ||
                       memcmp(decrypted.board_id, session_board_id, session_board_id_len) != 0 ||
                       decrypted.sequence != session_seq_in ||
                       decrypted.sequence >= FEB_SESSION_SEQUENCE_MAX) {
                        FURI_LOG_W(TAG, "Protected record session/sequence mismatch; dropping (no reply)");
                        session_reset_state();
                        post_session_fatal(profile->app);
                        return BleEventAckFlowEnable;
                    }
                    session_seq_in++;

                    if(text_matches(decrypted.type, decrypted.type_len, "capability_response")) {
                        handle_capability_response(profile, decrypted.plaintext, decrypted.plaintext_len);
                    } else if(text_matches(decrypted.type, decrypted.type_len, "status")) {
                        /* `status` carries no capability field either (docs/PROTOCOL.md);
                           wardriving's own states ("started"/"data"/"stopped") are never
                           ambiguous with wifi_scan/ble_scan's ("partial"/"complete") by text
                           alone, so a cheap state-only peek is enough to route correctly --
                           including a wardriving status(state="data", request_id=0)
                           unsolicited backlog-drain record, since nothing here (or inside
                           handle_wardriving_status()) ever inspects request_id at all. This
                           peek's own decode failure is logged and dropped here rather than
                           silently falling through to a wifi_scan/ble_scan handler that would
                           just fail the exact same decode again. */
                        static feb_status_payload_t status_peek;
                        feb_cbor_status_t peek_status = feb_cbor_decode_status_payload(
                            decrypted.plaintext, decrypted.plaintext_len, &status_peek);
                        if(peek_status != FEB_CBOR_OK) {
                            FURI_LOG_W(TAG, "status payload decode failed: %d; dropping", peek_status);
                        } else if(
                            text_matches(status_peek.state, status_peek.state_len, "started") ||
                            text_matches(status_peek.state, status_peek.state_len, "data") ||
                            text_matches(status_peek.state, status_peek.state_len, "stopped")) {
                            handle_wardriving_status(profile, decrypted.plaintext, decrypted.plaintext_len);
                        } else if(pending_command_kind == PendingCommandBleScan) {
                            handle_ble_scan_status(profile, decrypted.plaintext, decrypted.plaintext_len);
                        } else {
                            handle_wifi_scan_status(profile, decrypted.plaintext, decrypted.plaintext_len);
                        }
                    } else if(text_matches(decrypted.type, decrypted.type_len, "error")) {
                        handle_runtime_error(profile, decrypted.plaintext, decrypted.plaintext_len);
                    } else {
                        FURI_LOG_W(
                            TAG,
                            "Ignoring unrecognized protected record type '%.*s'",
                            (int)decrypted.type_len,
                            decrypted.type);
                    }
                    return BleEventAckFlowEnable;
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

/* wifi_scan results view (docs/PLAN.md's Wi-Fi scan capability follow-on step): a dedicated
   scrollable list, separate from the fixed-layout main screen above, showing every reported
   AP (scrolled with Up/Down, not truncated to a summary). `wifi_scan_aps`/`wifi_scan_ap_count`
   are this file's own statics (see their declaration comment), not Esp32App members. */
#define WIFI_SCAN_RESULTS_ROW_HEIGHT 10
#define WIFI_SCAN_RESULTS_MAX_ROWS 4
#define WIFI_SCAN_RESULTS_FIRST_ROW_Y 22
#define WIFI_SCAN_RESULTS_FOOTER_Y 62

static void draw_wifi_scan_results(Canvas* canvas, const Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    char header[32];
    if(app->wifi_scan_in_progress) {
        snprintf(header, sizeof(header), "Scanning...");
    } else {
        snprintf(header, sizeof(header), "Wifi scan: %u found", (unsigned)wifi_scan_ap_count);
    }
    canvas_draw_str(canvas, 2, 11, header);
    canvas_set_font(canvas, FontSecondary);

    uint8_t y = WIFI_SCAN_RESULTS_FIRST_ROW_Y;
    size_t max_rows = WIFI_SCAN_RESULTS_MAX_ROWS;
    if(app->wifi_scan_error_message[0] != '\0') {
        canvas_draw_str(canvas, 2, y, app->wifi_scan_error_message);
        y += WIFI_SCAN_RESULTS_ROW_HEIGHT;
        max_rows--;
    }

    for(size_t row = 0; row < max_rows; row++) {
        size_t index = app->wifi_scan_scroll_offset + row;
        if(index >= wifi_scan_ap_count) {
            break;
        }
        const WifiScanApDisplay* ap = &wifi_scan_aps[index];
        char line[48];
        snprintf(
            line,
            sizeof(line),
            "%s ch%lu %ldm",
            ap->ssid[0] != '\0' ? ap->ssid : "(hidden)",
            (unsigned long)ap->channel,
            (long)ap->rssi_dbm);
        canvas_draw_str(canvas, 2, (uint8_t)(y + row * WIFI_SCAN_RESULTS_ROW_HEIGHT), line);
    }

    char footer[32];
    if(wifi_scan_ap_count == 0) {
        snprintf(footer, sizeof(footer), "Back: exit view");
    } else {
        snprintf(
            footer,
            sizeof(footer),
            "%u/%u  Back: exit",
            (unsigned)(app->wifi_scan_scroll_offset + 1),
            (unsigned)wifi_scan_ap_count);
    }
    canvas_draw_str(canvas, 2, WIFI_SCAN_RESULTS_FOOTER_Y, footer);
}

/* ble_scan results view, mirroring draw_wifi_scan_results() above exactly. `address` is
   formatted as the conventional colon-separated hex pairs; `name` shows a placeholder when
   the peer advertised none (docs/PROTOCOL.md: `name` is an optional field, distinct from an
   advertised empty string). */
#define BLE_SCAN_RESULTS_ROW_HEIGHT 10
#define BLE_SCAN_RESULTS_MAX_ROWS 4
#define BLE_SCAN_RESULTS_FIRST_ROW_Y 22
#define BLE_SCAN_RESULTS_FOOTER_Y 62

static void draw_ble_scan_results(Canvas* canvas, const Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    char header[32];
    if(app->ble_scan_in_progress) {
        snprintf(header, sizeof(header), "Scanning...");
    } else {
        snprintf(header, sizeof(header), "Ble scan: %u found", (unsigned)ble_scan_device_count);
    }
    canvas_draw_str(canvas, 2, 11, header);
    canvas_set_font(canvas, FontSecondary);

    uint8_t y = BLE_SCAN_RESULTS_FIRST_ROW_Y;
    size_t max_rows = BLE_SCAN_RESULTS_MAX_ROWS;
    if(app->ble_scan_error_message[0] != '\0') {
        canvas_draw_str(canvas, 2, y, app->ble_scan_error_message);
        y += BLE_SCAN_RESULTS_ROW_HEIGHT;
        max_rows--;
    }

    for(size_t row = 0; row < max_rows; row++) {
        size_t index = app->ble_scan_scroll_offset + row;
        if(index >= ble_scan_device_count) {
            break;
        }
        const BleScanDeviceDisplay* device = &ble_scan_devices[index];
        char address_str[18];
        snprintf(
            address_str,
            sizeof(address_str),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            device->address[0],
            device->address[1],
            device->address[2],
            device->address[3],
            device->address[4],
            device->address[5]);
        /* Worst case: 17-byte address_str + ' ' + up to 31-byte name + ' ' + up to 5-byte
           signed rssi ("-128m") + NUL == 56 bytes; sized with margin (unlike wifi_scan's
           48-byte line, whose ssid/channel/rssi worst case is smaller) so this doesn't
           trip -Werror=format-truncation. The canvas still visually clips at screen width
           regardless of this buffer's capacity. */
        char line[64];
        snprintf(
            line,
            sizeof(line),
            "%s %s %ldm",
            address_str,
            device->has_name && device->name[0] != '\0' ? device->name : "(no name)",
            (long)device->rssi_dbm);
        canvas_draw_str(canvas, 2, (uint8_t)(y + row * BLE_SCAN_RESULTS_ROW_HEIGHT), line);
    }

    char footer[32];
    if(ble_scan_device_count == 0) {
        snprintf(footer, sizeof(footer), "Back: exit view");
    } else {
        snprintf(
            footer,
            sizeof(footer),
            "%u/%u  Back: exit",
            (unsigned)(app->ble_scan_scroll_offset + 1),
            (unsigned)ble_scan_device_count);
    }
    canvas_draw_str(canvas, 2, BLE_SCAN_RESULTS_FOOTER_Y, footer);
}

/* wardriving control/status screen (docs/CAPABILITIES.md's wardriving bullet) -- a third
   fixed-layout screen alongside the main screen and the two scan-results views, not a
   scrollable list. Left/Right toggle which source(s) the next `start` requests when the
   board advertises both wifi_scan and ble_scan (see app->wardriving_use_wifi/
   wardriving_use_ble); there is still no interval-entry UI (docs/PLAN.md).
   wardriving_running_known is deliberately displayed as its own distinct "unknown" state
   (docs/LESSONS.md "UI must derive from real state") rather than defaulting to "stopped" --
   this Flipper genuinely has no evidence either way until a "started"/"stopped" ack or a
   busy/not_running error arrives this session (see Esp32App's own field comment). */
static void draw_wardriving_screen(Canvas* canvas, const Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    const char* state_text;
    if(!app->wardriving_running_known) {
        state_text = "Wardriving: unknown";
    } else if(app->wardriving_running) {
        state_text = "Wardriving: RUNNING";
    } else {
        state_text = "Wardriving: stopped";
    }
    canvas_draw_str(canvas, 2, 11, state_text);
    canvas_set_font(canvas, FontSecondary);

    /* Sized with margin over the two longest fields (wardriving_last_summary and
       wardriving_error_message, both up to ~40-48 real bytes) plus their literal prefixes,
       so -Werror=format-truncation's static worst-case analysis is satisfied -- see this
       project's docs/LESSONS.md for why a value that "can't really" overflow at runtime
       still needs a buffer GCC can prove is large enough. */
    char line[80];
    bool wardriving_is_running = app->wardriving_running_known && app->wardriving_running;
    bool both_sources_advertised = app->capability_has_wifi_scan && app->capability_has_ble_scan;
    /* While stopped and both sources are advertised, this line shows what the next `start`
       will request (toggled via Left/Right below) instead of the Recs/Backlog line -- picking
       a source is only actionable before OK is pressed, so it takes the slot back once
       running. Single-source boards have nothing to pick, so they always see Recs/Backlog. */
    if(!wardriving_is_running && both_sources_advertised) {
        const char* source_label;
        if(app->wardriving_use_wifi && app->wardriving_use_ble) {
            source_label = "WiFi+BLE";
        } else if(app->wardriving_use_wifi) {
            source_label = "WiFi only";
        } else {
            source_label = "BLE only";
        }
        snprintf(line, sizeof(line), "Source: %s", source_label);
    } else if(app->wardriving_backlog_remaining > 0) {
        snprintf(
            line,
            sizeof(line),
            "Recs: %lu  Backlog: %llu",
            (unsigned long)app->wardriving_records_this_session,
            (unsigned long long)app->wardriving_backlog_remaining);
    } else {
        snprintf(
            line, sizeof(line), "Recs: %lu  Live", (unsigned long)app->wardriving_records_this_session);
    }
    canvas_draw_str(canvas, 2, 22, line);

    if(app->wardriving_last_summary[0] != '\0') {
        snprintf(
            line,
            sizeof(line),
            "Last %s: %s",
            app->wardriving_last_is_ble ? "BLE" : "WiFi",
            app->wardriving_last_summary);
        canvas_draw_str(canvas, 2, 33, line);
    }

    if(app->wardriving_error_message[0] != '\0') {
        snprintf(line, sizeof(line), "! %s", app->wardriving_error_message);
        canvas_draw_str(canvas, 2, 44, line);
    }

    const char* footer;
    if(wardriving_is_running) {
        footer = "OK: stop  Back: exit";
    } else if(both_sources_advertised) {
        footer = "OK:start L/R:src Back:exit";
    } else {
        footer = "OK: start  Back: exit";
    }
    canvas_draw_str(canvas, 2, 56, footer);
}

/* Home screen layout: same proven 10px-pitch / y=62-footer convention as the
   WIFI_SCAN_RESULTS_ and BLE_SCAN_RESULTS_ constants above (see their declaration comments) --
   content rows run 22,32,42,52 (4 rows total budget) with the footer one more 10px stride
   below the last possible row, at 62. The header (has_saved_pairing + pairing-phase line,
   plus an optional capability line) consumes the first 2 or 3 of those 4 rows; whatever is
   left is the menu's visible window (1 row when the capability line is shown, since up to
   6 menu items -- Wardriving/Scan/GPS/Settings/About/Legacy -- can be visible at once and
   none of the 2/1 leftover rows fit them all, the menu must scroll rather than draw every
   visible item unconditionally. */
#define HOME_ROW_HEIGHT 10
#define HOME_FIRST_ROW_Y 22
#define HOME_FOOTER_Y 62
#define HOME_MAX_ROWS 4

static uint8_t home_header_height(Esp32App* app) {
    return app->has_capability_info ? 3 : 2;
}

static uint8_t home_menu_visible_rows(Esp32App* app) {
    return (uint8_t)(HOME_MAX_ROWS - home_header_height(app));
}

static bool home_menu_visible(Esp32App* app, HomeMenuItem item) {
    switch(item) {
    case HomeMenuWardriving:
        return app->pairing_phase == PairingPhaseSessionActive && app->capability_has_wardriving;
    case HomeMenuScan:
        return app->pairing_phase == PairingPhaseSessionActive &&
               (app->capability_has_wifi_scan || app->capability_has_ble_scan);
    case HomeMenuGps:
        return app->pairing_phase == PairingPhaseSessionActive && app->capability_has_wardriving;
    case HomeMenuSettings:
    case HomeMenuAbout:
    case HomeMenuLegacy:
        return true;
    default:
        return false;
    }
}

/* Keeps app->home_menu_index's rank within the currently-visible (filtered) item list inside
   the [scroll_offset, scroll_offset + visible_rows) window, the same "scroll follows
   selection" behavior draw_wifi_scan_results()/draw_ble_scan_results() get from their own
   scroll_offset + up/down clamping. Safe to call whenever the selection or the visible set
   (capability info, session state) may have changed -- it's a no-op if already in range. */
static void home_menu_scroll_into_view(Esp32App* app) {
    int total = 0;
    int rank = -1;
    for(int i = 0; i < HomeMenuCount; i++) {
        if(!home_menu_visible(app, (HomeMenuItem)i)) continue;
        if((HomeMenuItem)i == app->home_menu_index) rank = total;
        total++;
    }
    if(rank < 0) return;

    uint8_t visible_rows = home_menu_visible_rows(app);
    size_t max_offset = (size_t)total > visible_rows ? (size_t)total - visible_rows : 0;
    if(app->home_menu_scroll_offset > max_offset) {
        app->home_menu_scroll_offset = max_offset;
    }
    if((size_t)rank < app->home_menu_scroll_offset) {
        app->home_menu_scroll_offset = (size_t)rank;
    } else if((size_t)rank >= app->home_menu_scroll_offset + visible_rows) {
        app->home_menu_scroll_offset = (size_t)rank - visible_rows + 1;
    }
}

static void home_menu_step(Esp32App* app, int delta) {
    int idx = (int)app->home_menu_index;
    int count = HomeMenuCount;
    while(true) {
        idx += delta;
        if(idx < 0) idx = count - 1;
        if(idx >= count) idx = 0;
        if(home_menu_visible(app, (HomeMenuItem)idx)) {
            app->home_menu_index = (HomeMenuItem)idx;
            break;
        }
    }
    home_menu_scroll_into_view(app);
}

static void home_menu_fix_selection(Esp32App* app) {
    if(!home_menu_visible(app, app->home_menu_index)) {
        for(int i = 0; i < HomeMenuCount; i++) {
            if(home_menu_visible(app, (HomeMenuItem)i)) {
                app->home_menu_index = (HomeMenuItem)i;
                break;
            }
        }
    }
    home_menu_scroll_into_view(app);
}

static bool scan_menu_visible(Esp32App* app, ScanMenuItem item) {
    switch(item) {
    case ScanMenuWifi:
        return app->capability_has_wifi_scan;
    case ScanMenuBle:
        return app->capability_has_ble_scan;
    default:
        return false;
    }
}

static void scan_menu_step(Esp32App* app, int delta) {
    int idx = (int)app->scan_menu_index;
    int count = ScanMenuCount;
    while(true) {
        idx += delta;
        if(idx < 0) idx = count - 1;
        if(idx >= count) idx = 0;
        if(scan_menu_visible(app, (ScanMenuItem)idx)) {
            app->scan_menu_index = (ScanMenuItem)idx;
            return;
        }
    }
}

static void scan_menu_fix_selection(Esp32App* app) {
    if(!scan_menu_visible(app, app->scan_menu_index)) {
        for(int i = 0; i < ScanMenuCount; i++) {
            if(scan_menu_visible(app, (ScanMenuItem)i)) {
                app->scan_menu_index = (ScanMenuItem)i;
                return;
            }
        }
    }
}

static void draw_settings_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Settings");
    canvas_set_font(canvas, FontSecondary);

    char line[64];
    snprintf(line, sizeof(line), "Scan prefs: TBD");
    canvas_draw_str(canvas, 2, 22, line);
    snprintf(line, sizeof(line), "BLE active/passive: TBD");
    canvas_draw_str(canvas, 2, 33, line);
    snprintf(line, sizeof(line), "Pairing: %s", app->has_saved_pairing ? "saved" : "none");
    canvas_draw_str(canvas, 2, 44, line);
    canvas_draw_str(canvas, 2, 56, "Back: return");
}

static void draw_about_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "About");
    canvas_set_font(canvas, FontSecondary);

    char line[80];
    snprintf(line, sizeof(line), "ESP32 over BLE");
    canvas_draw_str(canvas, 2, 22, line);
    snprintf(
        line,
        sizeof(line),
        "Board: %s",
        app->has_capability_info && app->capability_board[0] != '\0' ? app->capability_board : "n/a");
    canvas_draw_str(canvas, 2, 33, line);
    snprintf(line, sizeof(line), "Protocol: v2");
    canvas_draw_str(canvas, 2, 44, line);
    snprintf(line, sizeof(line), "Session: %s", app->pairing_phase == PairingPhaseSessionActive ? "active" : "not active");
    canvas_draw_str(canvas, 2, 48, line);
    canvas_draw_str(canvas, 2, 56, "Back: return");
}

static void draw_legacy_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Legacy");
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_str(canvas, 2, 22, "Compatibility controls");
    if(app->profile) {
        canvas_draw_str(canvas, 2, 33, "OK: start session");
    } else {
        canvas_draw_str(canvas, 2, 33, "OK: start pair/connect");
    }
    canvas_draw_str(canvas, 2, 44, "Up: open Home");
    canvas_draw_str(canvas, 2, 56, "Back: return");
    UNUSED(app);
}

static void draw_gps_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "GPS");
    canvas_set_font(canvas, FontSecondary);

    DateTime now;
    furi_hal_rtc_get_datetime(&now);

    char line[64];
    snprintf(line, sizeof(line), "Mode: simulated");
    canvas_draw_str(canvas, 2, 22, line);
    snprintf(line, sizeof(line), "Fix: no fix (simulated)");
    canvas_draw_str(canvas, 2, 33, line);
    snprintf(
        line,
        sizeof(line),
        "Time: %04u-%02u-%02u %02u:%02u:%02u",
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second);
    canvas_draw_str(canvas, 2, 44, line);
    canvas_draw_str(canvas, 2, 48, "Lat/Lon: --   Speed: --");
    canvas_draw_str(canvas, 2, 56, "Back: return");
    UNUSED(app);
}

static void draw_scan_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Scan");
    canvas_set_font(canvas, FontSecondary);

    int menu_index = 0;
    if(app->capability_has_wifi_scan) {
        char line[24];
        snprintf(line, sizeof(line), "%sWi-Fi scan", app->scan_menu_index == ScanMenuWifi ? "> " : "  ");
        canvas_draw_str(canvas, 2, 22 + 10 * menu_index, line);
        menu_index++;
    }
    if(app->capability_has_ble_scan) {
        char line[24];
        snprintf(line, sizeof(line), "%sBLE scan", app->scan_menu_index == ScanMenuBle ? "> " : "  ");
        canvas_draw_str(canvas, 2, 22 + 10 * menu_index, line);
        menu_index++;
    }

    canvas_draw_str(canvas, 2, 56, "Up/Down: move  OK: start  Back: return");
}

static void draw_home_screen(Canvas* canvas, Esp32App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Home");
    canvas_set_font(canvas, FontSecondary);

    uint8_t y = HOME_FIRST_ROW_Y;
    canvas_draw_str(canvas, 2, y, app->has_saved_pairing ? "Have saved pairing" : "No saved pairing");
    y += HOME_ROW_HEIGHT;

    if(app->pairing_phase == PairingPhaseFailed) {
        char line[48];
        snprintf(line, sizeof(line), "Failed: %s", app->pairing_reason);
        canvas_draw_str(canvas, 2, y, line);
    } else {
        canvas_draw_str(canvas, 2, y, pairing_phase_text(app->pairing_phase));
    }
    y += HOME_ROW_HEIGHT;

    if(app->has_capability_info) {
        char line[CAPABILITY_BOARD_MAX_LEN + CAPABILITY_FEATURES_MAX_LEN + 4];
        snprintf(line, sizeof(line), "%s: %s", app->capability_board, app->capability_features);
        canvas_draw_str(canvas, 2, y, line);
        y += HOME_ROW_HEIGHT;
    }

    static const char* labels[HomeMenuCount] = {
        "Wardriving",
        "Scan",
        "GPS",
        "Settings",
        "About",
        "Legacy",
    };

    uint8_t visible_rows = home_menu_visible_rows(app);
    int rank = 0;
    for(int i = 0; i < HomeMenuCount; i++) {
        if(!home_menu_visible(app, (HomeMenuItem)i)) continue;
        if((size_t)rank >= app->home_menu_scroll_offset &&
           (size_t)rank < app->home_menu_scroll_offset + visible_rows) {
            char line[32];
            snprintf(
                line,
                sizeof(line),
                "%s%s",
                app->home_menu_index == (HomeMenuItem)i ? "> " : "  ",
                labels[i]);
            canvas_draw_str(canvas, 2, y, line);
            y += HOME_ROW_HEIGHT;
        }
        rank++;
    }

    canvas_draw_str(canvas, 2, HOME_FOOTER_Y, "Up/Down: move  OK: select");
}

static void draw_callback(Canvas* canvas, void* context) {
    Esp32App* app = context;
    /* Clear the entire viewport before every redraw; this ensures a screen transition cannot
       leave stale pixels behind when the new draw code only paints a subset of the canvas. */
    canvas_clear(canvas);

    if(app->connection_lost) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 12, "Connection lost");
    }

    if(app->screen == AppScreenHome) {
        home_menu_fix_selection(app);
        draw_home_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenScan) {
        scan_menu_fix_selection(app);
        draw_scan_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenGps) {
        draw_gps_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenSettings) {
        draw_settings_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenAbout) {
        draw_about_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenLegacy) {
        draw_legacy_screen(canvas, app);
        return;
    }
    if(app->screen == AppScreenWifiScanResults) {
        draw_wifi_scan_results(canvas, app);
        return;
    }
    if(app->screen == AppScreenBleScanResults) {
        draw_ble_scan_results(canvas, app);
        return;
    }
    if(app->screen == AppScreenWardriving) {
        draw_wardriving_screen(canvas, app);
        return;
    }

    home_menu_fix_selection(app);
    draw_home_screen(canvas, app);
}

static void input_callback(InputEvent* input, void* context) {
    Esp32App* app = context;
    AppEvent event = {.type = AppEventInput, .input = *input};
    furi_message_queue_put(app->queue, &event, 0);
}

/* Returns to the main screen and discards any in-progress/completed scan results (docs/PLAN.md's
   Wi-Fi scan capability follow-on step: "results... cleared when the user leaves the results
   view"), for wifi_scan, ble_scan, and wardriving alike. Also called whenever the underlying
   session/connection goes away (disconnect, reconnect, profile teardown) -- a lost session can
   never deliver the rest of an in-flight scan's status records, so leaving stale partial
   results on screen would be misleading, regardless of which capability was in flight.
   For wardriving specifically, this is also the CSV export session boundary: closing the
   file here (not on a "stopped" ack -- see wardriving_csv_file's own declaration comment)
   and resetting wardriving_running_known to false, since this Flipper's knowledge of the
   ESP32's run state does not survive a lost session (docs/LESSONS.md "UI must derive from
   real state") -- the next authenticated session starts genuinely not knowing either way. */
static void reset_scan_ui_state_impl(Esp32App* app, bool return_home) {
    if(return_home) {
        app->screen = AppScreenHome;
    }
    pending_command_kind = PendingCommandNone;
    app->wifi_scan_in_progress = false;
    app->wifi_scan_complete = false;
    app->wifi_scan_scroll_offset = 0;
    app->wifi_scan_error_message[0] = '\0';
    wifi_scan_ap_count = 0;
    app->ble_scan_in_progress = false;
    app->ble_scan_complete = false;
    app->ble_scan_scroll_offset = 0;
    app->ble_scan_error_message[0] = '\0';
    ble_scan_device_count = 0;
    app->wardriving_running_known = false;
    app->wardriving_running = false;
    app->wardriving_records_this_session = 0;
    app->wardriving_backlog_remaining = 0;
    app->wardriving_last_summary[0] = '\0';
    app->wardriving_error_message[0] = '\0';
    wardriving_csv_close();
}

static void reset_scan_ui_state(Esp32App* app) {
    reset_scan_ui_state_impl(app, true);
}

static void reset_scan_ui_state_keep_screen(Esp32App* app) {
    reset_scan_ui_state_impl(app, false);
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
    reset_scan_ui_state_keep_screen(app);
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
        .screen = AppScreenHome,
        .pairing_phase = PairingPhaseNone,
        .has_saved_pairing = false,
        .connection_lost = false,
        .wardriving_use_wifi = true,
        .wardriving_use_ble = true,
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
    capabilities_dir_ready = resolve_capabilities_dir_path(app.storage);
    if(!capabilities_dir_ready) {
        FURI_LOG_E(TAG, "Failed to resolve capabilities directory path");
    }
    wardriving_export_dir_ready = resolve_wardriving_export_dir_path(app.storage);
    if(!wardriving_export_dir_ready) {
        FURI_LOG_E(TAG, "Failed to resolve wardriving export directory path");
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
                    app.connection_lost = false;
                    pairing_reset_state();
                    session_reset_state();
                    reset_scan_ui_state_keep_screen(&app);
                    app.pairing_phase = PairingPhaseExchanging;
                    app.pairing_reason[0] = '\0';
                } else if(event.bt_status == BtStatusAdvertising) {
                    if(app.pairing_phase != PairingPhaseDone) {
                        app.connection_lost = false;
                        pairing_reset_state();
                        session_reset_state();
                        reset_scan_ui_state_keep_screen(&app);
                        notification_message(app.notifications, &sequence_blink_start_blue);
                        app.pairing_phase = PairingPhaseWaiting;
                        app.pairing_reason[0] = '\0';
                    }
                } else if(event.bt_status == BtStatusUnavailable) {
                    stop_service(&app);
                    app.connection_lost = true;
                    app.pairing_phase = PairingPhaseFailed;
                    strncpy(app.pairing_reason, "connection lost", sizeof(app.pairing_reason) - 1);
                    app.pairing_reason[sizeof(app.pairing_reason) - 1] = '\0';
                }
            }
        } else if(event.type == AppEventPairingPhase) {
            app.pairing_phase = event.pairing_phase;
            if(event.pairing_phase == PairingPhaseFailed) {
                app.connection_lost = strcmp(event.pairing_reason, "connection lost") == 0;
                strncpy(app.pairing_reason, event.pairing_reason, sizeof(app.pairing_reason) - 1);
                app.pairing_reason[sizeof(app.pairing_reason) - 1] = '\0';
            } else {
                app.connection_lost = false;
                app.pairing_reason[0] = '\0';
            }
            if(event.pairing_phase == PairingPhaseDone) {
                app.has_saved_pairing = true;
            }
        } else if(event.type == AppEventSessionFatal) {
            bt_disconnect(app.bt);
            app.connection_lost = true;
            app.pairing_phase = PairingPhaseFailed;
            strncpy(app.pairing_reason, "connection lost", sizeof(app.pairing_reason) - 1);
            app.pairing_reason[sizeof(app.pairing_reason) - 1] = '\0';
        } else if(event.type == AppEventCapabilityInfo) {
            app.has_capability_info = true;
            strncpy(app.capability_board, event.capability_board, sizeof(app.capability_board) - 1);
            app.capability_board[sizeof(app.capability_board) - 1] = '\0';
            strncpy(
                app.capability_features, event.capability_features, sizeof(app.capability_features) - 1);
            app.capability_features[sizeof(app.capability_features) - 1] = '\0';
            app.capability_has_wifi_scan = event.capability_has_wifi_scan;
            app.capability_has_ble_scan = event.capability_has_ble_scan;
            app.capability_has_wardriving = event.capability_has_wardriving;
        } else if(event.type == AppEventWifiScanAp) {
            if(wifi_scan_ap_count < WIFI_SCAN_MAX_DISPLAY_APS) {
                WifiScanApDisplay* slot = &wifi_scan_aps[wifi_scan_ap_count++];
                strncpy(slot->ssid, event.wifi_scan_ap_ssid, sizeof(slot->ssid) - 1);
                slot->ssid[sizeof(slot->ssid) - 1] = '\0';
                memcpy(slot->bssid, event.wifi_scan_ap_bssid, sizeof(slot->bssid));
                slot->rssi_dbm = event.wifi_scan_ap_rssi_dbm;
                slot->channel = event.wifi_scan_ap_channel;
                strncpy(slot->phy, event.wifi_scan_ap_phy, sizeof(slot->phy) - 1);
                slot->phy[sizeof(slot->phy) - 1] = '\0';
                strncpy(slot->auth, event.wifi_scan_ap_auth, sizeof(slot->auth) - 1);
                slot->auth[sizeof(slot->auth) - 1] = '\0';
            }
        } else if(event.type == AppEventWifiScanDone) {
            app.wifi_scan_in_progress = false;
            app.wifi_scan_complete = true;
        } else if(event.type == AppEventWifiScanError) {
            app.wifi_scan_in_progress = false;
            strncpy(
                app.wifi_scan_error_message,
                event.wifi_scan_error_message,
                sizeof(app.wifi_scan_error_message) - 1);
            app.wifi_scan_error_message[sizeof(app.wifi_scan_error_message) - 1] = '\0';
        } else if(event.type == AppEventBleScanDevice) {
            if(ble_scan_device_count < BLE_SCAN_MAX_DISPLAY_DEVICES) {
                BleScanDeviceDisplay* slot = &ble_scan_devices[ble_scan_device_count++];
                memcpy(slot->address, event.ble_scan_device_address, sizeof(slot->address));
                slot->has_name = event.ble_scan_device_has_name;
                strncpy(slot->name, event.ble_scan_device_name, sizeof(slot->name) - 1);
                slot->name[sizeof(slot->name) - 1] = '\0';
                slot->rssi_dbm = event.ble_scan_device_rssi_dbm;
                strncpy(slot->addr_type, event.ble_scan_device_addr_type, sizeof(slot->addr_type) - 1);
                slot->addr_type[sizeof(slot->addr_type) - 1] = '\0';
            }
        } else if(event.type == AppEventBleScanDone) {
            app.ble_scan_in_progress = false;
            app.ble_scan_complete = true;
        } else if(event.type == AppEventBleScanError) {
            app.ble_scan_in_progress = false;
            strncpy(
                app.ble_scan_error_message,
                event.ble_scan_error_message,
                sizeof(app.ble_scan_error_message) - 1);
            app.ble_scan_error_message[sizeof(app.ble_scan_error_message) - 1] = '\0';
        } else if(event.type == AppEventWardrivingRunState) {
            app.wardriving_running_known = true;
            app.wardriving_running = event.wardriving_running;
            if(event.wardriving_is_fresh_start) {
                /* A genuine new "started" ack -- reset this session's own counters, distinct
                   from a `busy`-error-inferred "it was already running" correction (which
                   must NOT reset counts we may already be accumulating this connection). */
                app.wardriving_records_this_session = 0;
                app.wardriving_backlog_remaining = 0;
                app.wardriving_last_summary[0] = '\0';
            }
            app.wardriving_error_message[0] = '\0';
        } else if(event.type == AppEventWardrivingBatch) {
            app.wardriving_records_this_session += event.wardriving_batch_count;
            app.wardriving_backlog_remaining = event.wardriving_backlog_remaining;
            if(event.wardriving_last_summary[0] != '\0') {
                app.wardriving_last_is_ble = event.wardriving_last_is_ble;
                strncpy(
                    app.wardriving_last_summary,
                    event.wardriving_last_summary,
                    sizeof(app.wardriving_last_summary) - 1);
                app.wardriving_last_summary[sizeof(app.wardriving_last_summary) - 1] = '\0';
            }
        } else if(event.type == AppEventWardrivingError) {
            strncpy(
                app.wardriving_error_message,
                event.wardriving_error_message,
                sizeof(app.wardriving_error_message) - 1);
            app.wardriving_error_message[sizeof(app.wardriving_error_message) - 1] = '\0';
        } else if(event.type == AppEventInput && event.input.type == InputTypeShort) {
            if(app.connection_lost) {
                if(event.input.key == InputKeyBack) {
                    app.connection_lost = false;
                    app.screen = AppScreenHome;
                }
            } else if(app.screen == AppScreenHome) {
                app.screen = AppScreenHome;
                if(event.input.key == InputKeyBack) {
                    running = false;
                } else if(event.input.key == InputKeyUp) {
                    home_menu_step(&app, -1);
                } else if(event.input.key == InputKeyDown) {
                    home_menu_step(&app, 1);
                } else if(event.input.key == InputKeyOk) {
                    switch(app.home_menu_index) {
                    case HomeMenuWardriving:
                        app.screen = AppScreenWardriving;
                        break;
                    case HomeMenuScan:
                        if(app.capability_has_wifi_scan && app.capability_has_ble_scan) {
                            app.scan_menu_index = ScanMenuWifi;
                            app.screen = AppScreenScan;
                        } else if(app.capability_has_wifi_scan && !app.wifi_scan_in_progress) {
                            wifi_scan_ap_count = 0;
                            app.wifi_scan_scroll_offset = 0;
                            app.wifi_scan_complete = false;
                            app.wifi_scan_error_message[0] = '\0';
                            app.screen = AppScreenWifiScanResults;
                            app.wifi_scan_in_progress = send_wifi_scan_command(&app);
                            if(!app.wifi_scan_in_progress) {
                                app.screen = AppScreenHome;
                            }
                        } else if(app.capability_has_ble_scan && !app.ble_scan_in_progress) {
                            ble_scan_device_count = 0;
                            app.ble_scan_scroll_offset = 0;
                            app.ble_scan_complete = false;
                            app.ble_scan_error_message[0] = '\0';
                            app.screen = AppScreenBleScanResults;
                            app.ble_scan_in_progress = send_ble_scan_command(&app);
                            if(!app.ble_scan_in_progress) {
                                app.screen = AppScreenHome;
                            }
                        }
                        break;
                    case HomeMenuGps:
                        app.screen = AppScreenGps;
                        break;
                    case HomeMenuSettings:
                        app.screen = AppScreenSettings;
                        break;
                    case HomeMenuAbout:
                        app.screen = AppScreenAbout;
                        break;
                    case HomeMenuLegacy:
                        app.screen = AppScreenLegacy;
                        break;
                    default:
                        break;
                    }
                }
            } else if(app.screen == AppScreenScan) {
                if(event.input.key == InputKeyBack) {
                    app.screen = AppScreenHome;
                } else if(event.input.key == InputKeyUp) {
                    scan_menu_step(&app, -1);
                } else if(event.input.key == InputKeyDown) {
                    scan_menu_step(&app, 1);
                } else if(event.input.key == InputKeyOk) {
                    if(app.scan_menu_index == ScanMenuWifi && app.capability_has_wifi_scan && !app.wifi_scan_in_progress) {
                        wifi_scan_ap_count = 0;
                        app.wifi_scan_scroll_offset = 0;
                        app.wifi_scan_complete = false;
                        app.wifi_scan_error_message[0] = '\0';
                        app.screen = AppScreenWifiScanResults;
                        app.wifi_scan_in_progress = send_wifi_scan_command(&app);
                        if(!app.wifi_scan_in_progress) {
                            app.screen = AppScreenHome;
                        }
                    } else if(app.scan_menu_index == ScanMenuBle && app.capability_has_ble_scan && !app.ble_scan_in_progress) {
                        ble_scan_device_count = 0;
                        app.ble_scan_scroll_offset = 0;
                        app.ble_scan_complete = false;
                        app.ble_scan_error_message[0] = '\0';
                        app.screen = AppScreenBleScanResults;
                        app.ble_scan_in_progress = send_ble_scan_command(&app);
                        if(!app.ble_scan_in_progress) {
                            app.screen = AppScreenHome;
                        }
                    }
                }
            } else if(app.screen == AppScreenGps || app.screen == AppScreenSettings ||
                      app.screen == AppScreenAbout || app.screen == AppScreenLegacy) {
                if(event.input.key == InputKeyBack) {
                    app.screen = AppScreenHome;
                }
            } else if(app.screen == AppScreenWifiScanResults) {
                if(event.input.key == InputKeyBack) {
                    reset_scan_ui_state(&app);
                } else if(event.input.key == InputKeyUp) {
                    if(app.wifi_scan_scroll_offset > 0) {
                        app.wifi_scan_scroll_offset--;
                    }
                } else if(event.input.key == InputKeyDown) {
                    size_t visible_rows = WIFI_SCAN_RESULTS_MAX_ROWS;
                    if(wifi_scan_ap_count > visible_rows &&
                       app.wifi_scan_scroll_offset < wifi_scan_ap_count - visible_rows) {
                        app.wifi_scan_scroll_offset++;
                    }
                } else if(event.input.key == InputKeyOk && !app.wifi_scan_in_progress) {
                    /* Re-trigger from inside the results view too, e.g. after a completed
                       scan -- "Scan now" is a repeatable manual action, not one-shot. */
                    wifi_scan_ap_count = 0;
                    app.wifi_scan_scroll_offset = 0;
                    app.wifi_scan_complete = false;
                    app.wifi_scan_error_message[0] = '\0';
                    app.wifi_scan_in_progress = send_wifi_scan_command(&app);
                }
            } else if(app.screen == AppScreenBleScanResults) {
                if(event.input.key == InputKeyBack) {
                    reset_scan_ui_state(&app);
                } else if(event.input.key == InputKeyUp) {
                    if(app.ble_scan_scroll_offset > 0) {
                        app.ble_scan_scroll_offset--;
                    }
                } else if(event.input.key == InputKeyDown) {
                    size_t visible_rows = BLE_SCAN_RESULTS_MAX_ROWS;
                    if(ble_scan_device_count > visible_rows &&
                       app.ble_scan_scroll_offset < ble_scan_device_count - visible_rows) {
                        app.ble_scan_scroll_offset++;
                    }
                } else if(event.input.key == InputKeyOk && !app.ble_scan_in_progress) {
                    /* Re-trigger from inside the results view too, e.g. after a completed
                       scan -- "Scan now" is a repeatable manual action, not one-shot. */
                    ble_scan_device_count = 0;
                    app.ble_scan_scroll_offset = 0;
                    app.ble_scan_complete = false;
                    app.ble_scan_error_message[0] = '\0';
                    app.ble_scan_in_progress = send_ble_scan_command(&app);
                }
            } else if(app.screen == AppScreenWardriving) {
                if(event.input.key == InputKeyBack) {
                    /* Unlike wifi_scan/ble_scan's Back, this does NOT stop wardriving --
                       capture runs autonomously server-side regardless of whether this
                       screen is open (docs/CAPABILITIES.md), so leaving it is pure
                       navigation, not a discard of unconfirmed state (there is none: start/
                       stop are already-sent, already-acked actions by the time this screen
                       reflects them). */
                    app.screen = AppScreenHome;
                } else if(event.input.key == InputKeyOk) {
                    if(app.wardriving_running_known && app.wardriving_running) {
                        send_wardriving_stop_command(&app);
                    } else {
                        send_wardriving_start_command(&app);
                    }
                } else if(
                    event.input.key == InputKeyLeft && app.capability_has_wifi_scan &&
                    app.capability_has_ble_scan &&
                    !(app.wardriving_running_known && app.wardriving_running)) {
                    /* Toggle Wi-Fi's membership in the next `start`'s sources, refusing to
                       drop the last remaining source (send_wardriving_start_command() already
                       guards this too, but the UI should never let the user reach a
                       zero-source selection in the first place). */
                    if(app.wardriving_use_wifi) {
                        if(app.wardriving_use_ble) app.wardriving_use_wifi = false;
                    } else {
                        app.wardriving_use_wifi = true;
                    }
                } else if(
                    event.input.key == InputKeyRight && app.capability_has_wifi_scan &&
                    app.capability_has_ble_scan &&
                    !(app.wardriving_running_known && app.wardriving_running)) {
                    if(app.wardriving_use_ble) {
                        if(app.wardriving_use_wifi) app.wardriving_use_ble = false;
                    } else {
                        app.wardriving_use_ble = true;
                    }
                }
            } else if(app.screen == AppScreenLegacy) {
                if(event.input.key == InputKeyBack) {
                    app.screen = AppScreenHome;
                } else if(event.input.key == InputKeyOk && !app.profile) {
                    start_profile(&app);
                } else if(
                    event.input.key == InputKeyUp && app.profile &&
                    app.pairing_phase == PairingPhaseSessionActive &&
                    (app.capability_has_wardriving || app.capability_has_wifi_scan ||
                     app.capability_has_ble_scan)) {
                    app.home_menu_index = HomeMenuWardriving;
                    app.screen = AppScreenHome;
                } else if(
                    event.input.key == InputKeyLeft && app.profile &&
                    app.pairing_phase == PairingPhaseSessionActive && app.capability_has_wifi_scan &&
                    !app.wifi_scan_in_progress) {
                    wifi_scan_ap_count = 0;
                    app.wifi_scan_scroll_offset = 0;
                    app.wifi_scan_complete = false;
                    app.wifi_scan_error_message[0] = '\0';
                    app.screen = AppScreenWifiScanResults;
                    app.wifi_scan_in_progress = send_wifi_scan_command(&app);
                    if(!app.wifi_scan_in_progress) {
                        app.screen = AppScreenHome;
                    }
                } else if(
                    event.input.key == InputKeyRight && app.profile &&
                    app.pairing_phase == PairingPhaseSessionActive && app.capability_has_ble_scan &&
                    !app.ble_scan_in_progress) {
                    ble_scan_device_count = 0;
                    app.ble_scan_scroll_offset = 0;
                    app.ble_scan_complete = false;
                    app.ble_scan_error_message[0] = '\0';
                    app.screen = AppScreenBleScanResults;
                    app.ble_scan_in_progress = send_ble_scan_command(&app);
                    if(!app.ble_scan_in_progress) {
                        app.screen = AppScreenHome;
                    }
                }
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

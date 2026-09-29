#pragma once

/* Unity build (TP-15 follow-up, 2026-09-29): the 12 split module .c files are all
   #include-d into flipper_esp32_over_ble.c as ONE translation unit, so the compiler sees
   exactly the linkage the pre-split monolith had (every cross-module symbol below is
   `static`, via these two macros) and can inline/elide across module boundaries the way
   -Os did before the split -- recovering the .text/.fast.rel.text heap cost the split
   otherwise added. The source stays split across files for readability/agent context;
   only the build unit changed. See flipper_esp32_over_ble.c for the #include list and
   APP_UNITY_BUILD guard convention each module .c file uses to refuse standalone
   compilation. */
#define APP_FN static
#define APP_DATA static

/* Internal shared header for the flipper_esp32_over_ble FAP (TP-15 source split,
   2026-09-29). Types, enums, cross-module externs and prototypes only -- no logic here
   beyond the three tiny `static inline` helpers below, which are hot enough on the
   BLE-dispatch path (profile_event_handler's RX routing) that every module wants its own
   private inlined copy rather than an external call.

   Lock order (unchanged from the pre-split single file): app_protocol_mutex before
   app_reassembly_mutex. Never acquire them in the other order -- see app_protocol_mutex's
   own declaration comment in ble_transport.c for the full deadlock-avoidance argument. */

#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_random.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
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
#include "mesh_nodes.h"

#define TAG "Esp32OverBle"
#define PAYLOAD_MAX FEB_WRITE_CHAR_MAX_LEN
/* Default (pre-MTU-negotiation) BLE ATT MTU. Used as negotiated_att_mtu's initial/reset value
   until the real ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE event arrives (see profile_event_handler,
   G12 fix) -- send_pairing_record() falls back to this conservative value for any record sent
   before that happens. */
#define FEB_DEFAULT_ATT_MTU 23
/* G12 fix: the real cap on an outgoing fragment is not the negotiated ATT MTU alone -- it's
   whichever is smaller of that and this Notify characteristic's own fixed declared max
   value length (PAYLOAD_MAX, = FEB_WRITE_CHAR_MAX_LEN, 244 bytes as of 2026-09-26, was 64).
   Exceeding the characteristic's own cap fails independently of MTU
   headroom (ATT_ERR_INVALID_ATTR_VALUE_LEN) -- see docs/LESSONS.md's "att-mtu-vs-attribute-
   length" entry, which is exactly this same fact on the ESP32's Write-characteristic direction
   (FEB_FLIPPER_WRITE_EFFECTIVE_MTU there); this mirrors it for the Flipper's Notify direction. */
#define FEB_NOTIFY_CHAR_EFFECTIVE_MTU (PAYLOAD_MAX + FEB_ATT_WRITE_OVERHEAD)
#define PAIRING_REASON_MAX_LEN 32
#define PAIRING_DIR_NAME "pairings"
/* Real worst case is dir("/ext/apps_data/flipper_esp32_over_ble/pairings", ~46 bytes) + "/"
   + board_id (FEB_PAIRING_BOARD_ID_MAX_LEN=32) + ".dat.tmp" (8) = ~87 -- 96 left almost no
   margin, and build_pairing_path()'s truncation check (fails closed, does not overflow) could
   still spuriously fail a persist for a legitimate max-length board_id. Sized to 160 with real
   margin, matching this project's usual buffer-sizing convention elsewhere. */
#define FEB_PAIRINGS_PATH_MAX_LEN 160
/* docs/PLAN.md step 7: capability-cache file, own subdirectory next to (not inside)
   "pairings", same atomic-write pattern, one file per board_id. */
#define CAPABILITY_DIR_NAME "capabilities"
#define FEB_CAPABILITIES_PATH_MAX_LEN 160
/* App data root (docs/WARDRIVING_PUBLISH.md "On-SD file layout"): resolved once, same
   resolve-once-from-this-app's-own-thread pattern as resolve_pairings_dir_path()'s comment.
   The publish-result file and (host-script-owned, never read/written by this FAP) the
   wdgwars credentials file live flat here -- both are publish-flow plumbing, not wardriving
   data. The wardriving CSV itself (current + host-script-archived) lives one level down, in
   its own "wardriving" subdirectory (see WARDRIVING_DIR_NAME below) alongside the older
   per-calendar-day export files that predate Phase 6 -- corrected 2026-09-18 after Phase 6
   briefly flattened it to match an earlier draft of the host script; the host script itself
   was updated instead, since only the wardriving CSV needed a directory, not the two
   publish-flow files. */
#define FEB_WARDRIVING_PUBLISH_RESULT_FILENAME "wardriving_publish_result.txt"
/* Own subdirectory of the app data root, holding only wardriving CSV exports -- current and
   host-script-archived alike (docs/WARDRIVING_PUBLISH.md "Result handling"). Same
   resolve-once-and-mkdir pattern as CAPABILITY_DIR_NAME/PAIRING_DIR_NAME above. */
#define WARDRIVING_DIR_NAME "wardriving"
#define FEB_WARDRIVING_CSV_FILENAME "wardriving_current.csv"
#define FEB_WARDRIVING_EXPORT_PATH_MAX_LEN 160
/* mesh_log capability (docs/WARDRIVING_PUBLISH.md "Mesh node publishing", docs/PROTOCOL.md
   "`mesh_log` command and status payloads") -- own subdirectory of the app data root,
   sibling of WARDRIVING_DIR_NAME, same resolve-once-and-mkdir pattern. Only the "current"
   flat-text accumulator is written here by this FAP; a future host-script pass will archive
   it on confirmed publish success, mirroring the wardriving CSV's own archiving (out of
   scope for this pass, see mesh_nodes.h). */
#define MESH_DIR_NAME "mesh"
#define FEB_MESH_LOG_FILENAME "mesh_nodes_current.txt"
#define FEB_MESH_LOG_PATH_MAX_LEN 160
/* Wardriving Stopped-screen settings (docs/WARDRIVING_REDESIGN.md "Persistence") -- flat
   `key=value` lines at the app data root, same file-shape convention as the publish-flow
   plumbing files above; global, not per-board (design doc decision 5). Real worst case is
   5 lines of "<=15-char key>=<=11-char value>\n" (~30 bytes each) plus margin -- sized with
   real margin, not shaved to the byte, matching this project's usual buffer-sizing
   convention. */
#define FEB_WARDRIVING_SETTINGS_FILENAME "wardriving_settings.txt"
#define FEB_WARDRIVING_SETTINGS_MAX_LEN 256u
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
/* `gps` poll cadence while the Wardriving screen is open (docs/PLAN.md "Real GPS driver...",
   decision 5: "GPS position changing at walking/driving speed doesn't need push latency").
   2 seconds is far below anything that would feel sluggish for a fix/no-fix indicator, and
   far above the cost of one more round-trip encrypted command every tick -- no bound is
   specified in PROTOCOL.md, so this is a judgment call, not a re-derivation of a frozen
   number. */
#define GPS_POLL_PERIOD_MS 2000
/* Publish-result poll cadence/timeout (docs/WARDRIVING_PUBLISH.md "Result handling") -- the
   host script's own network call can legitimately take a while, so the timeout is generous;
   neither number is wire-format-pinned, just a judgment call bounding an otherwise-unbounded
   wait for a file that might never appear (host script never ran, USB never got plugged in,
   etc). HP-10: raised 180s -> 600s to give the host script's own size-scaled archive step
   (a large CSV's own copy/verify) room to finish inside this window instead of racing it --
   the host side is being changed to keep its whole run, including that step, well under 600s. */
#define FEB_PUBLISH_POLL_PERIOD_MS 2000u
#define FEB_PUBLISH_POLL_TIMEOUT_MS 600000u
#define FEB_PUBLISH_RESULT_MAX_LEN 512u
/* Pinned commit for the fetched bootstrap script (docs/WARDRIVING_PUBLISH.md's BadUSB
   section: this runs unattended, with no review step, so "whatever's on the default branch
   right now" is not acceptable -- same discipline docs/PROTOCOL.md already applies to the
   wire format). This is the commit where scripts/publish_wardriving.ps1 last changed (added
   the independent mesh-node/Method-2 upload alongside the existing CSV upload), confirmed
   pushed to origin/main via `git ls-remote` -- bump it again if that script's contract
   changes after this. */
#define WARDRIVING_PUBLISH_SCRIPT_COMMIT "3fae5b156c3d0266131be8c6807986a2738c28c3"

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
    /* Repurposed from the old meshcore_scan live-poll table to the mesh_log capture/backlog
       screen (docs/WARDRIVING_PUBLISH.md "Mesh node publishing") -- see
       draw_mesh_log_screen()'s own comment for what changed and why. */
    AppScreenMeshLog,
    AppScreenWifiScanResults,
    AppScreenBleScanResults,
    /* docs/WARDRIVING_REDESIGN.md (2026-09-21): the single AppScreenWardriving is replaced
       by this Stopped/Running split -- see draw_wardriving_stopped_screen()/
       draw_wardriving_running_screen()'s own comments. */
    AppScreenWardrivingStopped,
    AppScreenWardrivingRunning,
    AppScreenPublish,
} AppScreen;

typedef enum {
    HomeMenuWardriving = 0,
    /* Publish moved to immediately after Wardriving (docs/WARDRIVING_REDESIGN.md, 2026-09-21;
       previously last in this list). */
    HomeMenuPublish,
    HomeMenuScan,
    HomeMenuGps,
    /* Renamed from HomeMenuMeshcore alongside AppScreenMeshcore -> AppScreenMeshLog above. */
    HomeMenuMeshLog,
    HomeMenuCount,
} HomeMenuItem;

/* Publish-result outcome (docs/WARDRIVING_PUBLISH.md "Result handling"): mirrors the host
   script's own `status=` values one-for-one, plus Timeout/None for states the wire format
   itself has no word for (this Flipper gave up waiting / hasn't tried yet this screen visit). */
typedef enum {
    PublishOutcomeNone = 0,
    PublishOutcomeOk,
    PublishOutcomeFail,
    PublishOutcomeNothingToPublish,
    PublishOutcomeTimeout,
} PublishOutcome;

typedef enum {
    ScanMenuWifi = 0,
    ScanMenuBle,
    ScanMenuCount,
} ScanMenuItem;

/* docs/WARDRIVING_REDESIGN.md (2026-09-21): the old single 6-value WardrivingSourceMode
   enum is replaced by five independent settings, one row each on the new Stopped screen
   (see draw_wardriving_stopped_screen()), persisted in wardriving_settings.txt
   (wardriving_settings_load()/wardriving_settings_save()). Rows are capability-gated only,
   not re-gated by each other's current value (design doc rationale #4) -- e.g. Mode=BLE
   does not hide the WiFi Swelling row. */
typedef enum {
    WardrivingModeWifiBle = 0,
    WardrivingModeWifi,
    WardrivingModeBle,
    WardrivingModeCount,
} WardrivingMode;

typedef enum {
    WardrivingSwellingNormal = 0,
    WardrivingSwellingAggressive,
    WardrivingSwellingSpeedBased,
    WardrivingSwellingCount,
} WardrivingSwelling;

typedef enum {
    WardrivingBleModeActive = 0,
    WardrivingBleModePassive,
    WardrivingBleModeCount,
} WardrivingBleMode;

typedef enum {
    WardrivingCountryBg = 0,
    WardrivingCountryRoW,
    WardrivingCountryCount,
} WardrivingCountry;

/* wifi_band (added 2026-09-26, docs/PROTOCOL.md's `wardriving` command payload row of the
   same name) -- Wi-Fi scan band selection for the OLIMEX MOD-ESP32-C5's dual-band radio.
   Sent on every board regardless of 5GHz hardware (see that row's own wording); the C6/
   Heltec accept all three values without error and always scan 2.4GHz only. Same 3-way
   cycle shape as WardrivingSwelling above, not the 2-way WardrivingCountry. */
typedef enum {
    WardrivingWifiBand24Ghz = 0,
    WardrivingWifiBand5GhzFast,
    WardrivingWifiBand5GhzFull,
    WardrivingWifiBandCount,
} WardrivingWifiBand;

/* `gps` capability status (docs/PROTOCOL.md "`gps` command and status payloads", frozen
   2026-09-12): the wire's three states, `no_signal`/`acquiring`/`fix`. Distinct from
   `gps_status_known` (Esp32App/AppEvent) tracking whether this session has polled at all
   yet -- matches wardriving_running_known's own "no evidence yet" pattern
   (docs/LESSONS.md "UI must derive from real state"). */
typedef enum {
    GpsFixStateNoSignal = 0,
    GpsFixStateAcquiring,
    GpsFixStateFix,
} GpsFixState;

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
       record; the batch's own records are deep-copied onto wardriving_csv_ring by
       wardriving_record_stream_cb() (wardriving_rx.c) as each is decoded, not posted through
       this queue at all -- the actual dedup/format/CSV write for that ring only happens once
       this event wakes the main thread (G08: wardriving_csv_drain_pending(), see its own
       comment for why BleEventWorker itself never touches storage). */
    AppEventWardrivingRunState,
    AppEventWardrivingBatch,
    AppEventWardrivingError,
    /* `gps` (docs/PLAN.md "Real GPS driver..."): AppEventGpsStatus carries a decoded status
       reply (posted from the BLE thread, see handle_gps_status()); AppEventGpsPollTick is
       posted by app_gps_poll_timer's callback (Furi timer-service thread) purely to make the
       main thread do the actual send -- see send_gps_command()'s and app_gps_poll_timer's own
       comments for why the send itself never happens directly on the timer thread. */
    AppEventGpsStatus,
    AppEventGpsPollTick,
    /* No payload -- publish_poll_timer_callback() posts this purely to make the main thread
       do the actual file-existence check (same reasoning as AppEventGpsPollTick's own
       comment). */
    AppEventPublishPollTick,
    /* G08 (docs/archive/grok-4.6-findings-2026-09-11.md "### G08"): pairing_storage_save()
       moved off BleEventWorker. Carries no payload -- the secret/board_id hand-off lives in
       session_flow.c's own dedicated static buffer (pairing_save_pending_*, guarded by
       app_protocol_mutex), not in this flat union, so a 32-byte secret never sits inside the
       promiscuously-reused app_shared_ble_event a moment longer than the queue post itself.
       Posted once, from handle_pair_complete(). */
    AppEventPairingSaveRequest,
    /* G08: mesh_log_write_record()/mesh_log_ensure_open() moved off BleEventWorker the same
       way wardriving's own CSV write did -- see mesh_log_rx.c's mesh_log_ring. Carries no
       payload; posted by handle_mesh_log_status() once per drained mesh_data reply. */
    AppEventMeshLogPending,
} AppEventType;

/* One flat member per event type used to make this struct 576 bytes -- every field of
   every event type resident simultaneously, in an app whose whole `.bss` is a single
   permanently-resident heap allocation (see app_shared_ble_event's own declaration comment and
   docs/HARDENING_BACKLOG.md H04). It is now a tagged union: `type` selects exactly one
   member, which is the only one a poster writes or a handler reads. Safe because every
   field was already written by exactly one post_*() and read by exactly one branch of the
   main loop's if/else chain on `type` -- no field was ever shared across event types. The
   three per-capability error-message fields collapse into one `error_message` for the same
   reason (AppEventWifiScanError/AppEventBleScanError/AppEventWardrivingError are three
   distinct types, never in flight as the same event).

   Size: 576 -> 104 bytes. That is 4 resident instances in `.bss` plus the 8-deep
   furi_message_queue's own heap copy plus the main loop's stack local, so the saving is
   ~1.9 KB of `.bss`, ~3.8 KB of system heap, and ~470 bytes of this app's 4 KB main-thread
   stack.

   wifi_scan per-AP display fields: phy/auth are copied (not aliased) because their source
   (feb_wifi_scan_ap_t, decoded on the BLE thread from a buffer valid only for the duration
   of that one profile_event_handler call) cannot outlive the event post; ssid is sanitized
   to printable ASCII here (docs/PROTOCOL.md: raw bytes on the wire, not guaranteed
   printable/UTF-8) so both this event and the display list downstream always hold a safe,
   NUL-terminated C string. */
#define APP_EVENT_ERROR_MESSAGE_LEN 48
#define APP_EVENT_WARDRIVING_SUMMARY_LEN 40

typedef struct {
    AppEventType type;
    union {
        InputEvent input; /* AppEventInput */
        /* generation is app_protocol_generation as read (under app_protocol_mutex) at the moment
           bt_status_callback posted this event -- see protocol_reset_if_unchanged()'s own
           comment (HP-07/G10). */
        struct {
            BtStatus status;
            uint32_t generation;
        } bt_status; /* AppEventBtStatus */
        struct {
            PairingPhase phase;
            char reason[PAIRING_REASON_MAX_LEN];
        } pairing; /* AppEventPairingPhase */
        struct {
            char board[CAPABILITY_BOARD_MAX_LEN + 1];
            char features[CAPABILITY_FEATURES_MAX_LEN];
            bool has_wifi_scan;
            bool has_ble_scan;
            bool has_wardriving;
            bool has_gps;
            /* Still decoded/stored even though the Flipper no longer polls meshcore_scan
               directly (see AppScreenMeshLog) -- kept as capability-registry metadata, same
               as every other has_* flag. */
            bool has_meshcore_scan;
            bool has_mesh_log;
        } capability; /* AppEventCapabilityInfo */
        /* AppEventWifiScanAp / AppEventBleScanDevice carry no payload (HP-08): the decoded
           AP/device is copied straight into wifi_scan_aps[]/ble_scan_devices[] under
           app_wardriving_state_mutex by the poster (see post_wifi_scan_results_updated()/
           post_ble_scan_results_updated()), and these two event types now exist only to
           make the main loop redraw -- one post per `status` record, not one per item, so a
           large batch can never silently overflow this app's 8-deep queue the way one event
           per AP/device previously could. */
        struct {
            bool running;
            bool is_fresh_start; /* true only for a real "started" ack -- see this event's
                                     own AppEventType comment; distinguishes a genuine new
                                     capture (reset the on-screen record counter) from a
                                     `busy`-error-inferred "it was already running"
                                     correction (do not reset the counter). */
        } wardriving_run_state; /* AppEventWardrivingRunState */
        struct {
            uint64_t backlog_remaining;
            uint32_t csv_rows;
            char last_wifi_summary[APP_EVENT_WARDRIVING_SUMMARY_LEN];
            char last_ble_summary[APP_EVENT_WARDRIVING_SUMMARY_LEN];
        } wardriving_batch; /* AppEventWardrivingBatch */
        /* lat_e7_offset..speed_e1_kmh are only meaningful when state == GpsFixStateFix
           (see post_gps_status()). */
        struct {
            uint64_t lat_e7_offset;
            uint64_t lon_e7_offset;
            uint64_t fix_quality;
            uint64_t satellites;
            uint64_t hdop_e1;
            uint64_t utc_timestamp_s;
            uint64_t altitude_dm_offset;
            uint64_t speed_e1_kmh;
            uint8_t state; /* GpsFixState value */
        } gps; /* AppEventGpsStatus */
        /* AppEventWifiScanError / AppEventBleScanError / AppEventWardrivingError */
        char error_message[APP_EVENT_ERROR_MESSAGE_LEN];
        /* AppEventWifiScanDone / AppEventBleScanDone / AppEventSessionFatal /
           AppEventGpsPollTick / AppEventPublishPollTick / AppEventPairingSaveRequest /
           AppEventMeshLogPending carry no payload. G08: handle_mesh_log_status() no longer
           appends directly into mesh_log_display_nodes[] -- it deep-copies onto mesh_log_ring
           and posts AppEventMeshLogPending instead; mesh_log_drain_pending() (main thread)
           does the append, under app_wardriving_state_mutex (see that function's own
           comment). */
    } u;
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
    uint32_t wardriving_csv_rows; /* file-backed CSV row count, not a per-session counter --
                                     see wardriving_csv_count_refresh() */
    uint64_t wardriving_backlog_remaining;
    char wardriving_last_wifi_summary[40];
    char wardriving_last_ble_summary[40];
    char wardriving_error_message[48];
    /* Stopped-screen settings (docs/WARDRIVING_REDESIGN.md) -- defaults set in
       flipper_esp32_over_ble_app() match the old WardrivingSourceMode default
       (WardrivingSourceWifi2Ble): WiFi+BLE, Normal, 2000ms, Active, RoW, 2.4GHz. Persisted in
       wardriving_settings.txt (wardriving_settings_load()/_save()), global not per-board
       (design doc decision 5). wardriving_settings_row is the Stopped screen's own
       Up/Down-selected row index, not persisted (resets to 0 each screen visit, same as
       every other screen's own transient scroll/cursor state in this file). */
    WardrivingMode wardriving_mode;
    WardrivingSwelling wardriving_swelling;
    uint32_t wardriving_cooldown_ms;
    WardrivingBleMode wardriving_ble_mode;
    WardrivingCountry wardriving_country;
    WardrivingWifiBand wardriving_wifi_band;
    size_t wardriving_settings_row;
    size_t wardriving_settings_scroll_offset;
    bool capability_has_gps;
    /* gps_status_known false means this session has never received a `gps` status reply yet
       (docs/LESSONS.md "UI must derive from real state") -- distinct from any particular
       GpsFixState value, same "unknown" pattern as wardriving_running_known above. Fix
       fields are only meaningful when gps_state == GpsFixStateFix. */
    bool gps_status_known;
    GpsFixState gps_state;
    uint64_t gps_lat_e7_offset;
    uint64_t gps_lon_e7_offset;
    uint64_t gps_fix_quality;
    uint64_t gps_satellites;
    uint64_t gps_hdop_e1;
    uint64_t gps_utc_timestamp_s;
    uint64_t gps_altitude_dm_offset;
    uint64_t gps_speed_e1_kmh;
    /* Still decoded/stored even though the Flipper no longer polls meshcore_scan directly
       (see AppScreenMeshLog) -- kept as capability-registry metadata, same as every other
       capability_has_* flag. */
    bool capability_has_meshcore_scan;
    /* mesh_log (docs/WARDRIVING_PUBLISH.md "Mesh node publishing") -- gates AppScreenMeshLog's
       visibility. The screen's own per-node display data lives in the file-scope static
       mesh_log_display_nodes[]/mesh_log_display_count, not here -- same off-stack-struct
       rationale as wifi_scan_aps/ble_scan_devices. mesh_log_scroll_offset is this screen's own
       transient Up/Down cursor, reset to 0 on every entry (the Home menu's HomeMenuMeshLog
       OK-case, alongside mesh_log_display_reload()). */
    bool capability_has_mesh_log;
    size_t mesh_log_scroll_offset;
    /* Publish screen state (docs/WARDRIVING_PUBLISH.md) -- publishing needs no ESP32
       connection at all, but publish_start() now actively tears down this app's own BLE
       profile for the duration of the transfer to relieve heap pressure on the GATT stack
       (docs/HARDENING_BACKLOG.md H04, 2026-09-26 field report: a real out-of-memory crash
       during a large-CSV publish). publish_waiting is true only while polling for the host
       script's result file; publish_outcome is PublishOutcomeNone until a poll or a trigger
       failure sets it. publish_bt_stopped is true only when this publish run is the one that
       stopped the profile, so publish_finish_waiting() knows whether to restart it. */
    bool publish_waiting;
    bool publish_bt_stopped;
    uint32_t publish_poll_elapsed_ms;
    PublishOutcome publish_outcome;
    uint32_t publish_imported;
    uint32_t publish_captured;
    uint32_t publish_updated;
    uint32_t publish_duplicates;
    uint32_t publish_no_gps;
    uint32_t publish_bad_rows;
    char publish_fail_message[96];
} Esp32App;

typedef struct {
    FuriHalBleProfileBase base;
    uint16_t service_handle;
    BleGattCharacteristicInstance characteristics[CharacteristicCount];
    GapSvcEventHandler* event_handler;
    Esp32App* app;
} Esp32BleProfile;
static inline int text_matches(const char* data, size_t len, const char* literal) {
    size_t literal_len = strlen(literal);
    return len == literal_len && memcmp(data, literal, literal_len) == 0;
}
static inline bool board_id_is_valid(const char* board_id, size_t len) {
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
   comment already makes for app_session_key/app_session_seq_out: a reply cannot physically arrive
   before the send that provoked it has returned on this thread, so there is no window where
   both threads touch this flag at once. */
typedef enum {
    PendingCommandNone,
    PendingCommandWifiScan,
    PendingCommandBleScan,
    PendingCommandWardrivingStart,
    PendingCommandWardrivingStop,
    PendingCommandWardrivingStatus,
} PendingCommandKind;

/* PairStage (pairing ceremony) and SessionStage (runtime session) -- declared here because
   both ble_transport.c's profile_event_handler and session_flow.c's handle_pair_ family,
   handle_hello and handle_client_auth need them; see app_pair_stage's / app_session_stage's
   own extern comments below for which .c file owns the definition. */
typedef enum {
    PairStageNone,
    PairStageInitReceived,
    PairStageConfirmReceived,
} PairStage;

typedef enum {
    SessionStageNone,
    SessionStageHelloReceived,
    SessionStageActive,
} SessionStage;

/* FEB_CMD_PAYLOAD_MAX_LEN is defined with its buffers in session_flow.c; duplicated here
   (identical token, legal under C 6.10.3p2) since the extern array declarations below need it. */
#define FEB_CMD_PAYLOAD_MAX_LEN 224u

/* Copies up to `dst_cap - 1` bytes of a non-NUL-terminated text span (as returned by the
   cbor_codec decoders -- phy/auth alias the decode buffer, not a C string) into `dst`,
   NUL-terminating. */
static inline void copy_clamped_text(char* dst, size_t dst_cap, const char* src, size_t src_len) {
    size_t n = src_len > dst_cap - 1 ? dst_cap - 1 : src_len;
    memcpy(dst, src, n);
    dst[n] = '\0';
}


typedef union {
    feb_gps_result_payload_t gps;
    feb_mesh_log_status_result_payload_t mesh_log;
} feb_shared_status_result_t;

/* Shared by wardriving_rx.c's wardriving_csv_ensure_open() and mesh_log_rx.c's
   mesh_log_ensure_open() -- see wardriving_rx.c's own comment on this tri-state shape. */
typedef enum {
    WardrivingCsvOpenOk = 0,
    WardrivingCsvOpenFailed,
    WardrivingCsvOpenDeferred,
} WardrivingCsvOpenResult;

/* APP_QUEUE_PUT_TIMEOUT_MS is defined with app_queue_put() in session_flow.c; duplicated here
   (identical token, legal under C 6.10.3p2) since every post_*() function across modules calls
   app_queue_put() with it. */
#define APP_QUEUE_PUT_TIMEOUT_MS 20u

/* Cross-module display-list counts (definition/array stay in the capability's own RX module;
   app_handle_input()'s Up/Down scroll-bound checks are the only other reader). */
APP_DATA size_t wifi_scan_ap_count;
APP_DATA size_t ble_scan_device_count;
APP_DATA size_t mesh_log_display_count;

/* Duplicated verbatim (identical token sequence, legal under C 6.10.3p2) from their owning
   .c file (scan_rx.c/scan_rx.c/mesh_log_rx.c/app_ui.c) because app_handle_input()/
   draw_publish_screen() also need them; the defining file keeps its own copy too. */
#define WIFI_SCAN_RESULTS_MAX_ROWS 4
#define BLE_SCAN_RESULTS_MAX_ROWS 4
#define MESH_LOG_RESULTS_MAX_ROWS 4
#define HOME_ROW_HEIGHT 10


/* ---- Cross-module state, defined in the .c file named in each comment. Generic names
   (reassembly, characteristics, session_key, ...) are never let onto this FAP's external
   symbol table -- every one of these carries an app_ prefix (TP-15 source split). ---- */

/* ble_transport.c */
APP_DATA FuriMutex* app_protocol_mutex;
APP_DATA uint32_t app_protocol_generation;
APP_DATA FuriMutex* app_wardriving_state_mutex;
APP_DATA feb_reassembly_t app_reassembly;
APP_DATA FuriMutex* app_reassembly_mutex;
APP_DATA FuriTimer* app_reassembly_timeout_timer;
APP_DATA FuriTimer* app_gps_poll_timer;
APP_DATA FuriTimer* app_publish_poll_timer;
APP_DATA const FuriHalBleProfileTemplate app_profile_callbacks;
APP_DATA PairStage app_pair_stage = PairStageNone;
APP_DATA char app_pair_board_id[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
APP_DATA size_t app_pair_board_id_len;
APP_DATA uint8_t app_pair_transcript[FEB_PAIRING_MAX_TRANSCRIPT_LEN];
APP_DATA size_t app_pair_transcript_len;
APP_DATA uint8_t app_pair_k_confirm[FEB_PAIRING_KCONFIRM_LEN];
APP_DATA uint8_t app_pair_secret[FEB_PAIRING_SECRET_LEN];
APP_DATA uint8_t app_pairing_payload_buf[256];
APP_DATA uint8_t app_pairing_record_buf[FEB_MAX_RECORD_SIZE];

/* app_storage.c */
APP_DATA char app_pairings_dir_path[FEB_PAIRINGS_PATH_MAX_LEN];
APP_DATA bool app_pairings_dir_ready;
APP_DATA char app_capabilities_dir_path[FEB_CAPABILITIES_PATH_MAX_LEN];
APP_DATA bool app_capabilities_dir_ready;
APP_DATA char app_data_root_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
APP_DATA bool app_data_root_ready;
APP_DATA char app_wardriving_dir_path[FEB_WARDRIVING_EXPORT_PATH_MAX_LEN];
APP_DATA bool app_wardriving_dir_ready;
APP_DATA char app_mesh_dir_path[FEB_MESH_LOG_PATH_MAX_LEN];
APP_DATA bool app_mesh_dir_ready;
APP_DATA char app_wardriving_settings_buf[FEB_WARDRIVING_SETTINGS_MAX_LEN];

/* wardriving_settings.c */
APP_DATA const uint32_t app_wardriving_cooldown_values[3] = {5000u, 2000u, 0u};
APP_DATA const char* const app_wardriving_cooldown_labels[3] = {"5s", "2s", "0s"};
APP_DATA uint32_t app_wardriving_csv_row_count;
APP_DATA uint32_t app_wardriving_csv_saved_rows;
APP_DATA uint32_t app_wardriving_csv_saved_size;

/* session_flow.c -- session/protocol state stays privately owned there (design 3.2); every
   other module only ever gets extern read/write access through these declarations. */
APP_DATA AppEvent app_shared_ble_event;
APP_DATA feb_shared_status_result_t app_shared_status_result;
APP_DATA PendingCommandKind app_pending_command_kind = PendingCommandNone;
APP_DATA SessionStage app_session_stage = SessionStageNone;
APP_DATA char app_session_board_id[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
APP_DATA size_t app_session_board_id_len;
APP_DATA uint8_t app_session_id_bytes[FEB_SESSION_ID_LEN];
APP_DATA uint8_t app_session_client_nonce[FEB_SESSION_NONCE_FIELD_LEN];
APP_DATA uint8_t app_session_device_nonce[FEB_SESSION_NONCE_FIELD_LEN];
APP_DATA uint8_t app_session_transcript_buf[FEB_SESSION_MAX_TRANSCRIPT_LEN];
APP_DATA size_t app_session_transcript_len;
APP_DATA uint8_t app_session_pairing_secret[FEB_PAIRING_SECRET_LEN];
APP_DATA uint8_t app_session_key[FEB_SESSION_KEY_LEN];
APP_DATA uint64_t app_session_seq_out;
APP_DATA uint64_t app_session_seq_in;
APP_DATA uint8_t app_session_plaintext_buf[FEB_CBOR_MAX_PAYLOAD];
APP_DATA uint8_t app_cmd_payload_buf[FEB_CMD_PAYLOAD_MAX_LEN];
APP_DATA uint8_t app_cmd_ciphertext_buf[FEB_CMD_PAYLOAD_MAX_LEN];
APP_DATA uint8_t app_cmd_record_buf[FEB_MAX_RECORD_SIZE];
APP_DATA bool app_wardriving_flush_led_active;


/* ---- Cross-module function prototypes (definition kept in the .c file that owns the
   surrounding logic; 'static' dropped there only for names in this list). ---- */

APP_FN void capability_storage_save_pending(Esp32App* app);
APP_FN void pairing_storage_save_pending(Esp32App* app);
APP_FN void wardriving_csv_drain_pending(Esp32App* app);
APP_FN void mesh_log_drain_pending(Esp32App* app);
APP_FN bool
    capability_storage_exists(Storage* storage, const char* board_id, size_t board_id_len);
APP_FN bool
    pairing_storage_load(Storage* storage, const char* board_id, size_t board_id_len, uint8_t* secret_out);
APP_FN bool
    pairing_storage_save(Storage* storage, const char* board_id, size_t board_id_len, const uint8_t* secret);
APP_FN bool any_saved_pairing_exists(Storage* storage);
APP_FN bool app_queue_put(FuriMessageQueue* queue, const AppEvent* event, uint32_t timeout_ms);
APP_FN bool build_app_data_path(char* out, size_t out_cap, const char* filename);
APP_FN bool build_mesh_path(char* out, size_t out_cap, const char* filename);
APP_FN bool build_wardriving_path(char* out, size_t out_cap, const char* filename);
APP_FN bool capability_storage_load(
    Storage* storage,
    const char* board_id,
    size_t board_id_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len);
APP_FN bool capability_storage_save(
    Storage* storage,
    const char* board_id,
    size_t board_id_len,
    const uint8_t* payload,
    size_t payload_len);
APP_FN bool protocol_reset_if_unchanged(uint32_t generation);
APP_FN bool resolve_app_data_root_path(Storage* storage);
APP_FN bool resolve_capabilities_dir_path(Storage* storage);
APP_FN bool resolve_mesh_dir_path(Storage* storage);
APP_FN bool resolve_pairings_dir_path(Storage* storage);
APP_FN bool resolve_wardriving_dir_path(Storage* storage);
APP_FN bool send_ble_scan_command(Esp32App* app);
APP_FN bool send_gps_command(Esp32App* app);
APP_FN bool send_pairing_record(Esp32BleProfile* profile, const uint8_t* record, size_t record_len);
APP_FN bool send_wardriving_start_command(Esp32App* app);
APP_FN bool send_wardriving_status_query(Esp32App* app);
APP_FN bool send_wardriving_stop_command(Esp32App* app);
APP_FN bool send_wifi_scan_command(Esp32App* app);
APP_FN bool storage_open_heap_margin_ok(const char* what, bool* logged_once);
const char* wardriving_ble_mode_label(WardrivingBleMode mode);
const char* wardriving_country_wire_value(WardrivingCountry country);
const char* wardriving_mode_label(WardrivingMode mode);
const char* wardriving_swelling_label(WardrivingSwelling swelling);
const char* wardriving_swelling_wire_value(WardrivingSwelling swelling);
const char* wardriving_wifi_band_label(WardrivingWifiBand band);
const char* wardriving_wifi_band_wire_value(WardrivingWifiBand band);
APP_FN uint32_t publish_parse_uint(const char* value, size_t value_len);
APP_FN void
    handle_ble_scan_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len);
APP_FN void
    handle_gps_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len);
APP_FN void
    handle_mesh_log_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len);
APP_FN void
    handle_runtime_error(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len);
APP_FN void
    handle_wardriving_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len);
APP_FN void
    handle_wifi_scan_status(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len);
APP_FN void ble_scan_device_count_reset(void);
APP_FN void bt_status_callback(BtStatus status, void* context);
APP_FN void draw_ble_scan_results(Canvas* canvas, const Esp32App* app);
APP_FN void draw_callback(Canvas* canvas, void* context);
APP_FN void draw_gps_screen(Canvas* canvas, const Esp32App* app);
APP_FN void draw_mesh_log_screen(Canvas* canvas, Esp32App* app);
APP_FN void draw_publish_screen(Canvas* canvas, Esp32App* app);
APP_FN void draw_wardriving_running_screen(Canvas* canvas, const Esp32App* app);
APP_FN void draw_wardriving_stopped_screen(Canvas* canvas, Esp32App* app);
APP_FN void draw_wifi_scan_results(Canvas* canvas, const Esp32App* app);
APP_FN void gps_poll_timer_callback(void* context);
APP_FN void handle_capability_response(
    Esp32BleProfile* profile,
    const uint8_t* plaintext,
    size_t plaintext_len);
APP_FN void capability_bootstrap(Esp32BleProfile* profile);
APP_FN void post_pairing_phase(Esp32App* app, PairingPhase phase, const char* reason);
APP_FN void handle_hello(Esp32BleProfile* profile, const feb_unencrypted_record_t* envelope);
APP_FN void handle_pair_complete(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope);
APP_FN void handle_pair_confirm(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope);
APP_FN void handle_pair_init(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope);
APP_FN void home_menu_scroll_into_view(Esp32App* app);
APP_FN void input_callback(InputEvent* input, void* context);
APP_FN void mesh_log_close(Esp32App* app);
APP_FN void mesh_log_display_reload(Esp32App* app);
APP_FN void pairing_reset_state(void);
APP_FN void post_ble_scan_error(Esp32App* app, const char* message);
APP_FN void post_session_fatal(Esp32App* app);
APP_FN void post_wardriving_error(Esp32App* app, const char* message);
APP_FN void post_wardriving_run_state(Esp32App* app, bool running, bool is_fresh_start);
APP_FN void post_wifi_scan_error(Esp32App* app, const char* message);
APP_FN void protocol_generation_bump(void);
APP_FN void publish_finish_waiting(Esp32App* app);
APP_FN void publish_poll_check(Esp32App* app);
APP_FN void publish_poll_timer_callback(void* context);
APP_FN void publish_start(Esp32App* app);
APP_FN void reassembly_timeout_timer_callback(void* context);
APP_FN void reset_scan_ui_state_keep_screen(Esp32App* app);
APP_FN void session_reset_state(void);
APP_FN void start_profile(Esp32App* app);
APP_FN void stop_ble_profile(Esp32App* app);
APP_FN void stop_service(Esp32App* app);
APP_FN void wardriving_csv_close(Esp32App* app);
APP_FN void wardriving_csv_count_refresh(Esp32App* app);
APP_FN void wardriving_settings_cycle_row(Esp32App* app, int delta);
APP_FN void wardriving_settings_load(Esp32App* app);
APP_FN void wardriving_settings_save(const Esp32App* app);
APP_FN void wardriving_settings_step(Esp32App* app, int delta);
APP_FN void wifi_scan_ap_count_reset(void);
APP_FN size_t session_send_encrypted_command(
    const char* log_label,
    const char* type,
    size_t type_len,
    const uint8_t* payload,
    size_t payload_len,
    uint8_t* ciphertext_buf,
    size_t ciphertext_cap,
    uint8_t* record_buf,
    size_t record_cap);
APP_FN bool app_handle_input(Esp32App* app, const InputEvent* input);

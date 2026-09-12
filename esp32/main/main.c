#include <inttypes.h>
#include <stdbool.h>
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
#include "wardriving_log.h"
#include "wardriving_record_format.h"
#include "wardriving_validate.h"
#include "wardriving_dedup.h"

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

/* docs/PLAN.md step 7: hand-maintained, opaque, per-firmware-target constants -- not
   build-injected, bumped by hand. See docs/CAPABILITIES.md. */
#define FEB_BOARD_MODEL "esp32-c6-devkit"
#define FEB_FIRMWARE_VERSION "0.1.0"

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
/* Reserve this much headroom below FEB_CBOR_MAX_PAYLOAD when packing `aps` entries into one
   wifi_scan `status` record, to leave room for the enclosing status payload's own
   request_id/state/result-key map overhead (a handful of bytes; this is a generous margin,
   not a tight bound). */
#define FEB_WIFI_SCAN_STATUS_ENCODE_HEADROOM 32u

/* `ble_scan` capability (docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder").
   Unlike wifi_scan, there is no driver-reported "total found" count to bound against --
   NimBLE just keeps delivering BLE_GAP_EVENT_DISC as long as discovery runs -- so
   FEB_BLE_SCAN_RAW_MAX is the sole bound on the per-window catalog; a window that discovers
   more distinct addresses than this silently stops cataloging new ones (existing entries
   keep updating their strongest-seen RSSI). Same self-imposed-bound rationale as
   FEB_WIFI_SCAN_RAW_MAX. */
#define FEB_BLE_SCAN_RAW_MAX 64u
/* Fixed passive-discovery window duration for a manual ble_scan command (docs/PROTOCOL.md
   doesn't pin an exact number for this capability's scan duration; ~10s is long enough to
   observe multiple advertising intervals from most nearby peripherals without making a
   manual "scan now" trigger feel unresponsive). */
#define FEB_BLE_SCAN_WINDOW_MS 10000u
/* Same headroom rationale as FEB_WIFI_SCAN_STATUS_ENCODE_HEADROOM, applied to ble_scan's
   `status` record packing. */
#define FEB_BLE_SCAN_STATUS_ENCODE_HEADROOM 32u

/* `wardriving` capability (docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub
   reorder"; docs/PROTOCOL.md "`wardriving` command and status payloads"). Interval
   bounds/defaults (FEB_WARDRIVING_WIFI_INTERVAL_MIN_MS et al.) live in wardriving_validate.h,
   next to wardriving_resolve_start_intervals(), the pure/host-testable function that
   implements the "required vs. default when omitted" resolution -- see that header's top
   comment and handle_wardriving_command()'s comment for the interpretation this codebase
   settled on (resolved 2026-09-09: an omitted interval field for a requested source
   substitutes the point-4 default, matching the v1 Flipper client's no-interval-entry-UI
   behavior). */
/* Same headroom rationale as FEB_WIFI_SCAN_STATUS_ENCODE_HEADROOM, applied to wardriving's
   status(state="data") record packing. */
#define FEB_WARDRIVING_STATUS_ENCODE_HEADROOM 32u
/* wardriving_send_next_batch()'s peek-scratch buffer: one on-flash-payload-sized slot per
   possible batch entry (see wardriving_log.h's peek/scratch_buf contract). */
#define FEB_WARDRIVING_PEEK_SCRATCH_LEN (FEB_WARDRIVING_MAX_RECORDS_PER_BATCH * WD_RECORD_MAX_PAYLOAD)
/* Judgment call (not numerically specified anywhere): how many consecutive
   wardriving_log_append() failures constitute the "persistent flash-write failure" self-stop
   condition docs/PROTOCOL.md's `status` state-model table names as an example trigger for
   an unprompted "stopped" (see wardriving_self_stop()). One-off transient failures are
   logged and tolerated; three in a row past the wear-out/hardware-fault range this project
   is willing to silently absorb. */
#define FEB_WARDRIVING_FLASH_FAILURE_LIMIT 3u
/* BLE reconnect-stall coexistence fix: the gap wardriving's Wi-Fi source backs off to while
   a reconnect attempt is in flight after a normal disconnect (see
   wardriving_wifi_interval_throttled's comment above). Matches BLE's own idle gap per cycle
   (FEB_WARDRIVING_BLE_WINDOW_DEFAULT_MS=100 on FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS=500,
   i.e. ~400ms idle) rather than an unrelated number -- ESP32-internal recovery behavior, not
   part of the wire protocol, so it lives here and not in wardriving_validate.h. */
#define FEB_WARDRIVING_WIFI_RECONNECT_GAP_MS 400u

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
    TX_DONE_CONTINUE_WIFI_SCAN, /* another wifi_scan status batch is queued behind this one */
    TX_DONE_CONTINUE_BLE_SCAN,  /* another ble_scan status batch is queued behind this one */
    TX_DONE_CONTINUE_WARDRIVING, /* wardriving_send_next_batch()'s own re-entry point -- always
                                     used (not just when a follow-up batch is already known to
                                     exist) so full delivery of the just-sent batch is confirmed
                                     before it's marked drained; see wardriving_send_next_batch()
                                     and wardriving_pending_drain_count's comments. */
    TX_DONE_SEND_WARDRIVING_STOPPED, /* wardriving_self_stop()'s error record just went out;
                                         send the accompanying status(state="stopped") next */
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
static uint32_t last_record_activity_ms; /* reset on connect and on each record received or
                                             fully sent (write_complete()) -- docs/PROTOCOL.md's
                                             "without a record" is undirected; a wardriving-style
                                             session that only ever sends (never receives) must
                                             still count as live. See docs/LESSONS.md 2026-09-10. */

/* docs/PLAN.md step 7: first protected records exchanged post-auth. Per
   docs/PROTOCOL.md, a sequence counter begins at 1 for each authenticated session and
   increases by exactly one per protected record in a direction; set to 1 when
   runtime_auth_state becomes AUTHENTICATED (write_complete()'s TX_DONE_RUNTIME_AUTHENTICATED
   case), reset alongside the rest of the per-connection state on BLE_GAP_EVENT_CONNECT. */
static uint64_t rt_tx_sequence;
static uint64_t rt_rx_sequence;
static uint8_t rt_plaintext_buf[FEB_CBOR_MAX_PAYLOAD];
static uint8_t rt_ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];

/* docs/PROTOCOL.md: "must never wrap. A new BLE session is required before 2^24 - 1
   protected records are sent." feb_session_build_nonce() truncates sequence to its low
   24 bits and does not itself enforce this cap (session.h) -- both directions of a single
   session must stay strictly below this value or the AES-GCM nonce repeats. */
#define FEB_SESSION_SEQUENCE_MAX 0xFFFFFFu

static const char *const feb_features[] = {"wifi_scan", "ble_scan", "wardriving"};
#define FEB_FEATURE_COUNT (sizeof(feb_features) / sizeof(feb_features[0]))

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": wardriving's Wi-Fi/BLE
   capture sources deliberately reuse the manual wifi_scan/ble_scan capabilities' own
   wifi_scan_in_progress/ble_scan_in_progress busy flags and done-callout plumbing below,
   rather than a second parallel set of state -- this is what makes the bidirectional busy
   guard (docs/PROTOCOL.md: manual scan rejected busy while wardriving's matching source is
   active, and vice versa) hold by construction instead of needing a separate cross-check:
   there is exactly one "who currently owns the Wi-Fi radio scan" flag and one "who currently
   owns the NimBLE discovery state machine" flag, each shared by both callers. Each *_active_source
   selects which of the two callers' completion behavior (wifi_scan_done_cb()/
   ble_scan_window_close_cb()) runs when a pass finishes. */
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
   state per docs/PROTOCOL.md ("continues across BLE disconnects, buffering results to an
   on-device flash log"). wardriving_{wifi,ble}_active track whether each source's capture
   loop is currently supposed to be running -- distinct from wifi_scan_in_progress/
   ble_scan_in_progress, which track whether the underlying radio resource is claimed *right
   now* (true for the source's entire enabled lifetime, not just mid-pass, since a manual
   scan must stay locked out for as long as wardriving holds that source, not just while a
   scan/window is actually in flight). */
static bool wardriving_wifi_active;
static bool wardriving_ble_active;
static uint32_t wardriving_wifi_interval_ms;
/* BLE reconnect-stall coexistence fix (see docs/PROJECT_HISTORY.md's "Wardriving reconnect
   stall" investigation, sixth entry): while a reconnect attempt is in flight after a normal
   disconnect, wardriving's Wi-Fi source is temporarily throttled to
   FEB_WARDRIVING_WIFI_RECONNECT_GAP_MS so it stops starving the shared 2.4GHz radio. These
   two hold the pre-throttle interval and whether throttling is currently applied, so
   BLE_GAP_EVENT_CONNECT's success path can restore the user's configured cadence exactly. */
static uint32_t wardriving_wifi_interval_saved_ms;
static bool wardriving_wifi_interval_throttled;
static uint32_t wardriving_ble_window_ms;
static uint32_t wardriving_ble_interval_ms;
static struct ble_npl_callout wardriving_wifi_interval_co;
static struct ble_npl_callout wardriving_ble_interval_co;
/* True while a wardriving status(state="data") batch's fragments are still going out (or
   another is chained behind it via TX_DONE_CONTINUE_WARDRIVING) -- gates
   wardriving_maybe_kick_send() so a live capture event during an in-flight batch doesn't
   start a second, overlapping send (fragmentation is single-in-flight per
   docs/PROTOCOL.md#fragmentation); the in-flight chain itself re-checks the log for new
   data every time it finishes a batch, so nothing is lost by waiting.

   Root cause of the 2026-09-10 GATT-write-flood hardware bug (docs/LESSONS.md): this flag
   used to be cleared as soon as queue_and_send_protected() returned true, i.e. as soon as
   the record's *first* fragment was handed to NimBLE -- not once write_complete() confirmed
   every fragment of a multi-fragment record actually went out. wardriving's own ~30ms
   capture-completion cadence (unlike wifi_scan/ble_scan, which only ever get re-kicked by a
   slow, user-initiated `command`) could then fire again before the prior record's tail
   fragments finished sending, clobbering the single shared tx_fragment_* state with a second
   concurrent send and flooding NimBLE's write-buffer pool. Fixed by always chaining through
   TX_DONE_CONTINUE_WARDRIVING (see wardriving_send_next_batch()) so this flag (and
   wardriving_pending_drain_count below) are only ever touched once write_complete() has
   confirmed full delivery of the record currently in flight. */
static bool wardriving_tx_in_flight;
/* Count of records included in the batch currently in flight, marked drained from the flash
   log only on the *next* wardriving_send_next_batch() re-entry (which only happens after
   write_complete() confirms every fragment of that batch was actually delivered) -- not at
   send time. Marking a record drained before delivery is confirmed would silently lose it
   forever on a mid-record write failure (the flash log's only copy would already be gone).
   Reset alongside wardriving_tx_in_flight on every new connection (BLE_GAP_EVENT_CONNECT):
   an abandoned in-flight count from a prior, now-dead connection must not later mark-drain
   records that were never actually delivered. */
static size_t wardriving_pending_drain_count;
static uint8_t wardriving_flash_failure_count;
/* Set from wifi_scan_done_handler()/ble_scan_catalog_advertisement()'s window-close path
   (sys_evt task / NimBLE host task respectively) when FEB_WARDRIVING_FLASH_FAILURE_LIMIT is
   reached; consumed by wifi_scan_done_cb()/ble_scan_window_close_cb() on the NimBLE host
   task, which is where wardriving_self_stop() is actually safe to run from (it touches
   connection_handle/tx_fragment_* state owned by that task, matching every other
   cross-task handoff in this file). */
static bool wardriving_wifi_self_stop_pending;
static bool wardriving_ble_self_stop_pending;

/* docs/PLAN.md "Wi-Fi scan capability" step. wifi_scan_raw_records/wifi_scan_selected are
   written exactly once per scan by wifi_scan_done_handler() (runs on the default event
   loop's own task, per docs/PLAN.md's scan-execution-model decision) and then only ever read
   by code running on the NimBLE host task (wifi_scan_done_cb() and everything it calls) --
   handed off safely via the wifi_scan_done_co callout below, the same
   cross-task-safe-scheduling mechanism reassembly_timeout_co already uses, rather than a
   second lock. No new scan can start (wifi_scan_in_progress gates handle_command()) until
   that handoff's consumer clears it, so there is never a concurrent writer while the NimBLE
   task is reading. */
static wifi_ap_record_t wifi_scan_raw_records[FEB_WIFI_SCAN_RAW_MAX];
static feb_wifi_scan_ap_t wifi_scan_selected[FEB_WIFI_SCAN_MAX_APS_PER_RECORD];
static uint16_t wifi_scan_found_count;
static uint16_t wifi_scan_send_next_index;
static volatile bool wifi_scan_in_progress;
static uint64_t wifi_scan_request_id;
static struct ble_npl_callout wifi_scan_done_co;

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": per-window BLE device
   catalog, written only from gap_event()'s BLE_GAP_EVENT_DISC case (NimBLE host task) while
   ble_scan_in_progress is set, and read only from ble_scan_window_close_cb() /
   ble_scan_send_next_batch() -- both also on the NimBLE host task via the same callout
   mechanism reassembly_timeout_co/wifi_scan_done_co already use. Unlike wifi_scan, there is
   no separate task handoff here: BLE discovery events already arrive on the NimBLE host
   task, so the window-close callout is the only synchronization primitive needed. */
typedef struct {
    uint8_t addr[FEB_BLE_SCAN_ADDRESS_LEN];
    uint8_t addr_type; /* raw ble_addr_t.type (BLE_ADDR_PUBLIC/RANDOM/PUBLIC_ID/RANDOM_ID) */
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
static void wifi_scan_send_next_batch(uint16_t conn_handle);
static void wifi_scan_done_cb(struct ble_npl_event *ev);
static void wifi_scan_done_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static void start_wifi_subsystem(void);
static void ble_scan_catalog_advertisement(const struct ble_gap_disc_desc *disc);
static void ble_scan_send_next_batch(uint16_t conn_handle);
static void ble_scan_window_close_cb(struct ble_npl_event *ev);
static void handle_wardriving_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
static void wardriving_send_next_batch(uint16_t conn_handle);
static void wardriving_maybe_kick_send(uint16_t conn_handle);
static void wardriving_self_stop(const char *error_code);
static void wardriving_wifi_interval_cb(struct ble_npl_event *ev);
static void wardriving_ble_interval_cb(struct ble_npl_event *ev);

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
    /* docs/PLAN.md "Revised long-run reconnect policy": never run a second, dedicated
       reconnect scan while wardriving's BLE source owns discovery -- its own re-arm
       (wardriving_ble_interval_cb()) already delivers every advertisement through the same
       gap_event()/BLE_GAP_EVENT_DISC handler this scan would use, including the merged
       reconnect-match check. BLE_GAP_EVENT_DISC_COMPLETE already guarded its own call this
       way; root-caused 2026-09-10 (docs/LESSONS.md) that every *other* caller of this
       function (BLE_GAP_EVENT_DISCONNECT's two reconnect paths, reconnect_task()) did not,
       so a reconnect landing while wardriving's BLE capture was active raced its own
       ble_gap_disc() re-arm and failed one of the two with BLE_HS_EBUSY. Centralized here so
       every caller is covered by construction instead of needing its own guard. */
    if (wardriving_ble_active) {
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

/* docs/PLAN.md step 7: protected (AES-256-GCM) record, ESP32-to-Flipper direction only --
   the ESP32 never originates a protected record in the other direction today. */
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

/* Shared tail of every protected-record send: encode+queue, advance rt_tx_sequence, arm
   `next_action` for write_complete() to run once every fragment of this record has gone out,
   and kick off the first fragment. `next_action` is TX_DONE_NONE for a one-shot record
   (capability_response, error) or TX_DONE_CONTINUE_WIFI_SCAN when another status batch is
   already queued behind this one (docs/PLAN.md "Wi-Fi scan capability" step). */
static bool queue_and_send_protected(uint16_t conn_handle, const char *type, size_t type_len,
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

/* docs/PLAN.md "Wi-Fi scan capability" step: highest-generation PHY string and full-fidelity
   wifi_auth_mode_t string enum, both per docs/PROTOCOL.md's "`wifi_scan` command and status
   payloads" table. This ESP-IDF is pinned at v5.5.2 -- checked against
   esp_wifi_types_generic.h's actual wifi_auth_mode_t values (this board has no 5GHz radio,
   so phy_11a/phy_11ac never observably set on a real scan here; only 11b/11g/11n/11ax have
   corresponding wire strings at all, matching PROTOCOL.md's enum exactly). */
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
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise"; /* == WIFI_AUTH_ENTERPRISE alias */
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

/* docs/PROTOCOL.md "`ble_scan` command and status payloads": "NimBLE's resolved-private-
   address variants of a random address both collapse to `random`" -- BLE_ADDR_PUBLIC_ID is
   the resolved-identity counterpart of BLE_ADDR_PUBLIC, so it collapses to "public" the same
   way BLE_ADDR_RANDOM_ID collapses to "random". */
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

/* Runs on the default event loop's own task (sys_evt), never the NimBLE host task -- per
   docs/PLAN.md's scan-execution-model decision, a multi-second blocking scan must not run
   inside the protected-record dispatch handler on the NimBLE host task. Fetches results,
   selects the FEB_WIFI_SCAN_MAX_APS_PER_RECORD (32) strongest by RSSI (PROTOCOL.md's result
   cap), converts them into wire-ready feb_wifi_scan_ap_t entries, then hands off to the
   NimBLE host task via wifi_scan_done_co (same cross-task-safe callout-scheduling mechanism
   reassembly_timeout_co uses) to actually build and send status records, since that touches
   connection_handle/rt_tx_sequence/tx_fragment_* state owned by the NimBLE host task. */
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
        /* wardriving logs every discovered AP (up to the same FEB_WIFI_SCAN_RAW_MAX
           self-imposed raw-fetch bound above), not just the top
           FEB_WIFI_SCAN_MAX_APS_PER_RECORD by RSSI -- that 32-result cap is specific to
           wifi_scan's one-shot *reporting* contract (docs/PROTOCOL.md's "Result cap"),
           which wardriving's persistent capture log has no equivalent of; its own bound is
           flash capacity (sector eviction), not a wire-result count. */
        feb_location_t fix;
        bool have_fix = location_get_fix(&fix);
        uint16_t k;

        if (!have_fix) {
            ESP_LOGW(TAG, "wardriving: discarding %u wifi result(s), no GPS fix yet", (unsigned)raw_count);
        } else {
            for (k = 0; k < raw_count; k++) {
                wifi_ap_record_t *rec = &wifi_scan_raw_records[k];
                const char *auth = wifi_scan_auth_str(rec->authmode);
                feb_wardriving_record_t record;

                memset(&record, 0, sizeof(record));
                record.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000);
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

        /* ap->ssid aliases rec->ssid directly (wifi_scan_raw_records is file-scope static,
           not reused until the next scan starts, which can't happen until this scan's
           results are fully sent -- see wifi_scan_in_progress's comment above). ESP-IDF's
           wifi_ap_record_t has no separate SSID-length field, only a 33-byte null-padded
           buffer -- an SSID containing an embedded null byte (legal per 802.11, rare in
           practice) is reported truncated at that null; this is an ESP-IDF API limitation,
           not something this code can recover from. */
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

/* Runs on the NimBLE host task (wifi_scan_done_co's queue). */
static void wifi_scan_done_cb(struct ble_npl_event *ev)
{
    (void)ev;

    if (wifi_scan_active_source == WIFI_SCAN_SOURCE_WARDRIVING) {
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
   TX_DONE_CONTINUE_WIFI_SCAN once this record's fragments finish sending -- fragmentation is
   single-in-flight (docs/PROTOCOL.md#fragmentation), so the next batch cannot be queued until
   this one's tx_fragment_* state is free again. */
static void wifi_scan_send_next_batch(uint16_t conn_handle)
{
    /* static, not stack-local: this function (and everything it calls) runs on the
       ~4084-byte nimble_host task, invoked only from write_complete()'s
       TX_DONE_CONTINUE_WIFI_SCAN case and wifi_scan_done_cb() -- both on that same task,
       never reentrant/concurrent (fragmentation is single-in-flight per
       docs/PROTOCOL.md#fragmentation, and wifi_scan_in_progress gates a second scan from
       starting). result/trial together are two full 32-entry feb_wifi_scan_result_payload_t
       arrays (~1.5 KB each); as stack-local automatics they measured 3664 bytes of frame size
       under -fstack-usage, alone consuming ~90% of the entire task stack budget and the
       confirmed root cause of the 2026-09-07 nimble_host stack-protection-fault crash.
       Explicitly reset every call below since static storage only zero-initializes once. */
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
                /* A single AP's own encoding is already too large to ever fit -- should be
                   unreachable given this codec's fixed field-size bounds, but skip it rather
                   than spin forever or send an empty batch that isn't actually the last one. */
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

/* docs/PLAN.md "Wi-Fi scan capability" step. Extracted unmodified from what used to be the
   whole body of handle_command() (docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub
   reorder": that function is now a capability-name dispatcher -- see below). */
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

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": mirrors
   handle_wifi_scan_command()'s validation/busy-check/start shape exactly (arguments must be
   an empty map; busy guard; own_addr_type-scoped discovery start), substituting a fixed-
   duration passive BLE discovery window for a Wi-Fi scan. The window itself is closed by
   ble_scan_done_co (armed here), not by any GAP "discovery complete" callback -- NimBLE
   passive discovery with BLE_HS_FOREVER runs until explicitly cancelled. */
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
       includes device names that passive advertisements omit. Trade latency for name
       discovery (docs/PLAN.md backlog: "BLE active scanning on/off toggle"). */
    /* No controller dup-filtering here (unlike start_scan()'s reconnect-scan concern) --
       ble_scan wants every advertisement so it can track each address's strongest RSSI
       itself; see ble_scan_catalog_advertisement()'s own dedup-by-address handling. */
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

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": capability-name
   dispatch. A lookup table isn't earned yet at two entries (docs/SESSION_MEMORY.md's design
   note). */
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
    } else if (!send_protected_error(conn_handle, "unsupported_capability", strlen("unsupported_capability"),
                                     1, cmd->request_id)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": parses one
   BLE_GAP_EVENT_DISC advertisement's fields (reusing ble_hs_adv_parse_fields(), the same
   primitive scan_record_matches() already uses in this same call path -- no duplicated
   parsing) and folds it into ble_scan_raw_devices[], deduping by address within the current
   window and keeping the strongest RSSI seen. `struct ble_hs_adv_fields` is a small,
   pointer/scalar-only local (no embedded arrays) -- the same stack footprint
   scan_record_matches() already carries on this exact call path, so this adds no new stack
   risk beyond what's already accepted there. */
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
            /* Raw catalog full for this window -- drop silently, matching
               FEB_WIFI_SCAN_RAW_MAX's accepted-limitation treatment. */
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

/* Builds and sends one ble_scan `status` record starting at ble_scan_send_next_index,
   mirroring wifi_scan_send_next_batch()'s under-512-byte packing/chaining exactly --
   see that function's comment for why result/trial/status_payload/result_buf are static,
   not stack-local (same nimble_host task, same ~4084-byte budget, same
   single-in-flight-fragmentation reentrancy argument). */
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

/* Fires once FEB_BLE_SCAN_WINDOW_MS after handle_ble_scan_command() armed this callout --
   same ble_npl_callout mechanism as reassembly_timeout_co/wifi_scan_done_co, running on the
   NimBLE host task. Cancels the still-running discovery, selection-sorts the top
   FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD by RSSI (mirrors wifi_scan_done_handler()'s pattern),
   then sends -- or, if the connection that requested this scan is gone, discards, exactly
   like wifi_scan_done_cb()'s "no authenticated connection" branch. */
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
        /* wardriving logs every cataloged device this window (up to the same
           FEB_BLE_SCAN_RAW_MAX self-imposed bound ble_scan_catalog_advertisement() already
           applies), not just the top FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD by RSSI -- that cap
           is specific to ble_scan's one-shot *reporting* contract, which this persistent
           capture log has no equivalent of. */
        feb_location_t fix;
        bool have_fix = location_get_fix(&fix);
        uint16_t k;

        if (!have_fix) {
            ESP_LOGW(TAG, "wardriving: discarding %u ble result(s), no GPS fix yet",
                     (unsigned)ble_scan_raw_count);
        } else {
            for (k = 0; k < ble_scan_raw_count; k++) {
                ble_scan_raw_device_t *rec = &ble_scan_raw_devices[k];
                feb_wardriving_record_t record;

                memset(&record, 0, sizeof(record));
                record.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000);
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

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder" / docs/PROTOCOL.md
   "`wardriving` command and status payloads". Re-arms the next Wi-Fi capture pass after
   wardriving_wifi_interval_ms (0 = immediate/continuous) -- runs on the NimBLE host task
   (wardriving_wifi_interval_co's queue), consistent with handle_wardriving_command() and
   handle_wifi_scan_command() both already calling esp_wifi_scan_start() from that same
   task. */
static void wardriving_wifi_interval_cb(struct ble_npl_event *ev)
{
    wifi_scan_config_t scan_cfg;
    esp_err_t err;

    (void)ev;
    if (!wardriving_wifi_active) {
        return;
    }
    memset(&scan_cfg, 0, sizeof(scan_cfg));
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wardriving: re-trigger esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        wardriving_self_stop("internal_error");
    }
}

/* Same re-arming role as wardriving_wifi_interval_cb() above, for the BLE source: opens the
   next discovery window after the configured gap (ble_interval_ms - ble_window_ms, or 0 for
   back-to-back per docs/PROTOCOL.md's window<=interval invariant). */
static void wardriving_ble_interval_cb(struct ble_npl_event *ev)
{
    struct ble_gap_disc_params params = {0};
    int rc;

    (void)ev;
    if (!wardriving_ble_active) {
        return;
    }
    ble_scan_raw_count = 0;
    /* Active, not passive (was passive through 2026-09-10): this window is also the merged
       reconnect-scan pass (docs/PLAN.md "Revised long-run reconnect policy") while wardriving's
       BLE source owns discovery. A live forced-disconnect test found reconnect silently never
       matching under a passive-only re-arm here -- root-caused and fixed 2026-09-11
       (docs/LESSONS.md); the dedicated reconnect scan in start_scan() has always used active
       scanning and does find peers reliably. */
    params.passive = 0;
    params.filter_duplicates = 0;
    params.itvl = 0;
    params.window = 0;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == BLE_HS_EBUSY) {
        /* GAP master state is owned by someone else right now -- not start_scan() (guarded
           against wardriving_ble_active, docs/LESSONS.md 2026-09-10), but this can still
           happen transiently when gap_event()'s own merged-reconnect path
           (BLE_GAP_EVENT_DISC's "found v2 peer, connecting" branch) has just cancelled
           discovery to issue ble_gap_connect(). That resolves itself once the connect
           attempt succeeds or fails, so retry opening this window after the same short
           window duration rather than tearing wardriving down over a transient condition. */
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

/* Stops whichever wardriving source(s) are active and, if connected+authenticated, sends
   the docs/PROTOCOL.md-specified unsolicited error + status(state="stopped") pair (request_id
   0, the same "ESP32-initiated, not a reply to a specific command" sentinel used for
   backlog-drain status records) -- covers both an explicit internal self-stop trigger (a
   persistent flash-write failure; see FEB_WARDRIVING_FLASH_FAILURE_LIMIT) and a scan/discovery
   restart failure partway through a run. A no-op if wardriving wasn't running. Runs on the
   NimBLE host task only (called from wifi_scan_done_cb()/ble_scan_window_close_cb() and the
   interval callbacks above, all on that task) -- never from wifi_scan_done_handler() or
   ble_scan_catalog_advertisement() directly, since this touches connection_handle/
   tx_fragment_* state owned by the NimBLE host task (see wardriving_{wifi,ble}_self_stop_pending's
   comment for the hand-off). */
static void wardriving_self_stop(const char *error_code)
{
    bool was_active = wardriving_wifi_active || wardriving_ble_active;

    if (wardriving_wifi_active) {
        wardriving_wifi_active = false;
        wifi_scan_in_progress = false;
        ble_npl_callout_stop(&wardriving_wifi_interval_co);
        {
            esp_err_t serr = esp_wifi_scan_stop();

            if (serr != ESP_OK && serr != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_scan_stop failed during wardriving self-stop: %s",
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
                ESP_LOGW(TAG, "ble_gap_disc_cancel failed during wardriving self-stop: %d", derr);
            }
        }
    }
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

/* Kicks off a wardriving status(state="data") send if a session is connected+authenticated,
   there is buffered data to send, and no batch is already in flight -- called both right
   after a live capture pass appends new records and once at session-establish time to start
   draining any pre-existing backlog (docs/PROTOCOL.md's "Unsolicited backlog drain"). Once
   started, wardriving_send_next_batch()'s own TX_DONE_CONTINUE_WARDRIVING chaining picks up
   anything appended later without needing another call here. */
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
    wardriving_send_next_batch(conn_handle);
}

/* Builds and sends one wardriving status(state="data") record from the oldest still-pending
   flash-log records, mirroring wifi_scan_send_next_batch()/ble_scan_send_next_batch()'s
   trial-encode-and-back-off packing exactly (see that function's comment for why
   peeked/peek_scratch/result/status_payload/result_buf are static, not stack-local -- same
   NimBLE host task, same reasoning). Unlike wifi_scan/ble_scan's own chaining, this always
   chains through TX_DONE_CONTINUE_WARDRIVING -- even for the batch that drains the last
   pending record -- rather than only while backlog_remaining > 0: the re-entry this produces
   is what confirms full delivery of the batch before marking it drained from the flash log
   and clearing wardriving_tx_in_flight (see wardriving_pending_drain_count's comment; fixes
   the 2026-09-10 GATT-write-flood bug in docs/LESSONS.md). */
static void wardriving_send_next_batch(uint16_t conn_handle)
{
    static feb_wardriving_record_t peeked[FEB_WARDRIVING_MAX_RECORDS_PER_BATCH];
    static uint8_t peek_scratch[FEB_WARDRIVING_PEEK_SCRATCH_LEN];
    static feb_wardriving_status_result_payload_t result;
    /* Same reasoning as peeked/result/status_payload/result_buf above (and
       ble_scan_send_next_batch()'s own `trial`) -- this holds a full
       feb_wardriving_status_result_payload_t (a 32-entry feb_wardriving_record_t array,
       ~2.6 KB) per trial-encode iteration. Originally declared as a loop-local, which
       measured at -fstack-usage's 2608 bytes for this whole function -- nearly 2/3 of the
       nimble_host task's 4096-byte budget in one frame, on top of the CBOR-encode/GATT-write
       call chain this function itself makes. Root cause of the 2026-09-10 hardware
       stack-overflow crash (docs/LESSONS.md); moved to static to match the sibling
       function's already-correct pattern. */
    static feb_wardriving_status_result_payload_t trial;
    static feb_status_payload_t status_payload;
    static uint8_t result_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t peeked_count;
    size_t include_count;
    size_t remaining_after;
    size_t result_len;
    size_t payload_len;

    /* The previous batch's records are only safe to evict from the flash log once
       write_complete() has confirmed every one of its fragments actually went out --
       this function is only ever re-entered (via TX_DONE_CONTINUE_WARDRIVING) after that
       point, never earlier. See wardriving_pending_drain_count's comment. */
    if (wardriving_pending_drain_count > 0) {
        wardriving_log_mark_drained(wardriving_pending_drain_count);
        wardriving_pending_drain_count = 0;
    }

    peeked_count = wardriving_log_peek_pending(peeked, FEB_WARDRIVING_MAX_RECORDS_PER_BATCH,
                                               peek_scratch, sizeof(peek_scratch));
    if (peeked_count == 0) {
        wardriving_tx_in_flight = false;
        return;
    }

    memset(&result, 0, sizeof(result));
    include_count = 0;
    while (include_count < peeked_count) {
        size_t trial_len;

        trial = result;
        trial.records[trial.record_count] = peeked[include_count];
        trial.record_count++;
        trial_len = feb_cbor_encode_wardriving_status_result_payload(result_buf, sizeof(result_buf), &trial);
        if (trial_len == 0 || trial_len + FEB_WARDRIVING_STATUS_ENCODE_HEADROOM > FEB_CBOR_MAX_PAYLOAD) {
            if (result.record_count == 0) {
                /* A single record's own encoding is already too large to ever fit -- should
                   be unreachable given WD_RECORD_MAX_PAYLOAD's derivation, but drop it rather
                   than spin forever re-peeking the same record every batch. Still counted in
                   include_count so wardriving_log_mark_drained() consumes it below and moves
                   the oldest-pending pointer past it. */
                ESP_LOGE(TAG, "wardriving: single record too large to encode; dropping it");
                include_count++;
                continue;
            }
            break;
        }
        result = trial;
        include_count++;
    }

    remaining_after = wardriving_log_pending_count() - include_count;
    result.backlog_remaining = remaining_after;
    result_len = feb_cbor_encode_wardriving_status_result_payload(result_buf, sizeof(result_buf), &result);

    memset(&status_payload, 0, sizeof(status_payload));
    status_payload.request_id = 0; /* unsolicited/live sentinel -- docs/PROTOCOL.md: no wire
                                       distinction from a backlog-drain batch; backlog_remaining
                                       is how a receiver tells them apart */
    status_payload.state = "data";
    status_payload.state_len = strlen("data");
    status_payload.result_span = result_buf;
    status_payload.result_span_len = result_len;
    status_payload.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(pairing_payload_encode_buf,
                                                 sizeof(pairing_payload_encode_buf), &status_payload);
    /* Always chain through TX_DONE_CONTINUE_WARDRIVING, even when remaining_after == 0:
       write_complete() only runs tx_done_action once every fragment of *this* record has
       gone out, so this is what keeps wardriving_tx_in_flight/wardriving_pending_drain_count
       honest about the record actually being fully delivered rather than just its first
       fragment queued (2026-09-10 GATT-write-flood bug, see wardriving_tx_in_flight's
       comment). The re-entry this triggers finds nothing left pending and clears the flag
       itself when this really was the last batch -- see the peeked_count == 0 branch above. */
    if (payload_len == 0 ||
        !queue_and_send_protected(conn_handle, "status", strlen("status"),
                                  pairing_payload_encode_buf, payload_len,
                                  TX_DONE_CONTINUE_WARDRIVING)) {
        ESP_LOGE(TAG, "failed to build wardriving status(data) record");
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
   `wardriving` bullet. Field-presence/bounds validation this codec's decoder deliberately
   leaves to the caller (see cbor_wardriving.h's top-of-file comment) is all done here:
   action-dependent presence of `sources`/`wifi_interval_ms`/`ble_window_ms`+`ble_interval_ms`,
   source values restricted to "wifi"/"ble" with no duplicates, board-capability gating
   against feb_features[], and the interval bounds from docs/PLAN.md step 4.

   Interpretation note on "required" vs "default when omitted" (resolved 2026-09-09,
   supersedes an earlier pass's stricter reading -- see docs/SESSION_MEMORY.md):
   docs/PROTOCOL.md's field table says wifi_interval_ms/ble_window_ms/ble_interval_ms are
   each "required" on the wire whenever their source is requested, but the "Interval bounds
   and defaults" section is more specific and explicit: "The default when a `start` omits
   these fields is the maximum/point-4 values." Since this is exactly what a v1 Flipper
   client sends -- its wardriving control screen has no interval-entry UI at all
   (docs/PLAN.md), so it always requests a source via `sources` while leaving that source's
   interval field(s) out of the CBOR map entirely -- this function treats "required" as
   "applicable" rather than "must be present on the wire": a requested source's interval
   field(s) may be omitted, in which case the point-4 default is substituted; an interval
   field that IS present is still validated against the documented bounds, and an interval
   field present for a NOT-requested source is still invalid_command (that part of the field
   table's presence rule is unambiguous and unchanged). This resolution is implemented by
   wardriving_resolve_start_intervals() (wardriving_validate.h/.c, host-tested in
   tests/esp32/test_wardriving_log.c) rather than inline here, so it's exercised without
   needing a full command-dispatch harness. `sources` itself has no default/omission concept
   -- `action = "start"` with `sources` absent or empty is always invalid_command. */
static void handle_wardriving_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    feb_wardriving_command_payload_t payload;
    feb_cbor_status_t status;
    bool is_start;
    bool is_stop;
    bool want_wifi = false;
    bool want_ble = false;
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
    if (!is_start && !is_stop) {
        if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (is_stop) {
        if (payload.has_sources || payload.has_wifi_interval_ms || payload.has_ble_params) {
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
    {
        size_t recognized_count = (want_wifi ? 1u : 0u) + (want_ble ? 1u : 0u);

        /* Catches both an unrecognized source string and a duplicate entry ("wifi","wifi")
           in one comparison: source_count can only equal recognized_count if every entry is
           exactly one of "wifi"/"ble" and neither appears twice. Neither rule is an explicit
           docs/PROTOCOL.md sentence for the duplicate case -- a judgment call, since a
           repeated source is structurally nonsensical the same way an unrecognized one is. */
        if (payload.source_count != recognized_count) {
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
        if ((want_wifi && !have_wifi_scan) || (want_ble && !have_ble_scan)) {
            if (!send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
    }

    {
        wardriving_start_request_t req = {0};
        wardriving_resolved_intervals_t resolved;

        req.want_wifi = want_wifi;
        req.want_ble = want_ble;
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

        /* Stashed back into `payload` (not local-only) so the existing want_wifi/want_ble
           start blocks below, which already read payload.wifi_interval_ms/ble_window_ms/
           ble_interval_ms, pick up the resolved (explicit-and-valid, or defaulted) values
           unchanged. */
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

    if (want_wifi) {
        wifi_scan_config_t scan_cfg;
        esp_err_t err;

        memset(&scan_cfg, 0, sizeof(scan_cfg));
        wifi_scan_in_progress = true;
        wifi_scan_active_source = WIFI_SCAN_SOURCE_WARDRIVING;
        wardriving_wifi_interval_ms = (uint32_t)payload.wifi_interval_ms;
        /* Defensive reset, not relied on in practice -- should already be false by a fresh
           start (see wardriving_wifi_interval_throttled's comment above). */
        wardriving_wifi_interval_throttled = false;
        err = esp_wifi_scan_start(&scan_cfg, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wardriving: esp_wifi_scan_start failed: %s", esp_err_to_name(err));
            wifi_scan_in_progress = false;
            if (!send_protected_error(conn_handle, "internal_error", strlen("internal_error"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
        wardriving_wifi_active = true;
    }

    if (want_ble) {
        struct ble_gap_disc_params params = {0};
        int rc;

        ble_scan_in_progress = true;
        ble_scan_active_source = BLE_SCAN_SOURCE_WARDRIVING;
        ble_scan_raw_count = 0;
        wardriving_ble_window_ms = (uint32_t)payload.ble_window_ms;
        wardriving_ble_interval_ms = (uint32_t)payload.ble_interval_ms;

        /* Active, not passive -- see wardriving_ble_interval_cb()'s comment: this window is
           also the merged reconnect-scan pass while wardriving's BLE source owns discovery,
           and a passive-only pass here was found to never catch the reconnect match. */
        params.passive = 0;
        params.filter_duplicates = 0;
        params.itvl = 0;
        params.window = 0;
        rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "wardriving: ble_gap_disc start failed: %d", rc);
            ble_scan_in_progress = false;
            if (want_wifi) {
                /* Roll back the Wi-Fi source already started above, so `start` is atomic --
                   either every requested source comes up, or none does. */
                wardriving_wifi_active = false;
                wifi_scan_in_progress = false;
                esp_wifi_scan_stop();
            }
            if (!send_protected_error(conn_handle, "internal_error", strlen("internal_error"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
        ble_npl_callout_reset(&ble_scan_done_co, ble_npl_time_ms_to_ticks32(wardriving_ble_window_ms));
        wardriving_ble_active = true;
    }

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

    /* Every outbound record (pairing or session-protected) funnels its last fragment's
       write completion through here -- the single shared point that covers all send paths
       without per-call-site duplication. Counts as activity the same as an inbound record,
       so a wardriving-style session that only ever sends never trips the idle-timeout
       below just because the peer stays quiet. */
    last_record_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);

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
        rt_tx_sequence = 1;
        rt_rx_sequence = 1;
        ESP_LOGI(TAG, "client_auth sent; runtime session authenticated");
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
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        scan_report_window_count++;
        scan_report_lifetime_total++;

        /* ble_scan_in_progress is shared by the manual ble_scan capability and
           wardriving's BLE source (see ble_scan_source_t's comment), so this single check
           already covers both callers -- ble_scan_catalog_advertisement() itself is
           source-agnostic (just fills ble_scan_raw_devices[]); ble_scan_window_close_cb()
           decides what to do with the catalog based on ble_scan_active_source. */
        if (ble_scan_in_progress) {
            ble_scan_catalog_advertisement(&event->disc);
        }

        /* This connect-attempt path is the "merged reconnect-scan" mechanism
           (docs/PLAN.md step 2's "revised long-run reconnect policy" / step 4's
           "Accepted gap"): whichever discovery is currently running -- the dedicated
           reconnect scan (start_scan()), a manual ble_scan, or wardriving's own BLE
           capture window -- delivers every advertisement through this same event handler,
           so a Flipper's v2 service UUID is matched and connected to from whatever scan
           pass happens to be active, without a second dedicated scan ever running
           alongside wardriving's BLE source (see BLE_GAP_EVENT_DISC_COMPLETE below, which
           suppresses start_scan() while wardriving_ble_active is true for exactly this
           reason). */
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
        /* Suppressed while wardriving's BLE source is active: that source re-arms its own
           next discovery window on its own cadence (wardriving_ble_interval_cb()), and
           docs/PLAN.md's reconnect policy explicitly says not to run a second dedicated
           reconnect scan alongside it -- reconnect opportunities come from the same
           passive scan pass instead (see BLE_GAP_EVENT_DISC above). */
        if (connection_handle == BLE_HS_CONN_HANDLE_NONE && !reconnect_task_active && !wardriving_ble_active) {
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
        if (wardriving_wifi_interval_throttled) {
            wardriving_wifi_interval_ms = wardriving_wifi_interval_saved_ms;
            wardriving_wifi_interval_throttled = false;
        }
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
        rt_tx_sequence = 0;
        rt_rx_sequence = 0;
        tx_done_action = TX_DONE_NONE;
        tx_fragment_total = 0;
        tx_fragment_next = 0;
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
        hello_ack_deadline_ms = 0;
        pending_disconnect_reason = DISCONNECT_REASON_NORMAL;
        tx_done_action = TX_DONE_NONE;
        tx_fragment_total = 0;
        tx_fragment_next = 0;
        feb_reassembly_reset(&rx_reassembly);
        pairing_attempt_zeroize();
        runtime_auth_zeroize();
        wardriving_tx_in_flight = false;
        if (wifi_scan_in_progress && wifi_scan_active_source == WIFI_SCAN_SOURCE_MANUAL) {
            /* Don't clear wifi_scan_in_progress directly here -- the radio scan this
               connection started may still be running, and a new connection's `command`
               could otherwise race a still-in-flight wifi_scan_done_handler() write to
               wifi_scan_raw_records/wifi_scan_selected from a stale scan. esp_wifi_scan_stop()
               still fires WIFI_EVENT_SCAN_DONE for the aborted scan; wifi_scan_done_cb() then
               finds no authenticated connection and clears the flag there, uniformly.
               Gated to the manual source only -- wardriving's Wi-Fi capture (if active) must
               keep running across this disconnect (docs/PROTOCOL.md: "continues across BLE
               disconnects"), so wifi_scan_done_cb()'s WARDRIVING branch does not check
               connection state at all. */
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
               flag there, uniformly (avoids a race with a new connection's `command` reusing
               ble_scan_raw_devices/ble_scan_selected while this window's data is still being
               finalized). */
            int derr = ble_gap_disc_cancel();

            if (derr != 0 && derr != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "ble_gap_disc_cancel failed during disconnect cleanup: %d", derr);
            }
            ESP_LOGI(TAG, "ble_scan was in progress at disconnect; stopping discovery "
                          "(pending results will be discarded)");
        }

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
            if (wardriving_wifi_active && !wardriving_wifi_interval_throttled) {
                /* Coexistence-starvation fix (docs/PROJECT_HISTORY.md's "Wardriving
                   reconnect stall" investigation): give the shared radio a real Wi-Fi gap
                   while a reconnect is pending, restored on BLE_GAP_EVENT_CONNECT success.
                   wifi_scan_done_cb()'s WIFI_SCAN_SOURCE_WARDRIVING branch re-reads
                   wardriving_wifi_interval_ms fresh on its next re-arm, so nothing in
                   flight needs to be stopped or aborted here. */
                wardriving_wifi_interval_saved_ms = wardriving_wifi_interval_ms;
                wardriving_wifi_interval_ms = FEB_WARDRIVING_WIFI_RECONNECT_GAP_MS;
                wardriving_wifi_interval_throttled = true;
            }
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
                /* docs/PLAN.md step 7: first protected (AES-256-GCM) record handling. */
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
                    /* An unrecognized protected-record type (or a well-formed `status`,
                       which is never sent Flipper->ESP32 per docs/PROTOCOL.md) is logged and
                       otherwise ignored -- matches step 3's "drop and continue" policy for
                       malformed input at the layer below this one. */
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
    ble_npl_callout_init(&wifi_scan_done_co, nimble_port_get_dflt_eventq(), wifi_scan_done_cb, NULL);
    ble_npl_callout_init(&ble_scan_done_co, nimble_port_get_dflt_eventq(), ble_scan_window_close_cb, NULL);
    ble_npl_callout_init(&wardriving_wifi_interval_co, nimble_port_get_dflt_eventq(),
                        wardriving_wifi_interval_cb, NULL);
    ble_npl_callout_init(&wardriving_ble_interval_co, nimble_port_get_dflt_eventq(),
                        wardriving_ble_interval_cb, NULL);
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

    location_init();
    wardriving_log_init();

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

    start_wifi_subsystem();

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = host_synced;
    nimble_port_freertos_init(nimble_host_task);
}

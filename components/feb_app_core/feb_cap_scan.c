#include "feb_app_internal.h"

#if FEB_DIAG_WIFI_HEAP_LOG
#include "esp_heap_caps.h"
#endif

/* FEB_WIFI_SCAN_RAW_MAX (the raw-fetch bound) lives in feb_app_internal.h: the cluster-worker
   delegation paths size against it too. */
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
/* Judgment call (not numerically specified anywhere): how many consecutive
   wardriving_log_append() failures constitute the "persistent flash-write failure" self-stop
   condition docs/PROTOCOL.md's `status` state-model table names as an example trigger for
   an unprompted "stopped" (see feb_wardriving_self_stop()). One-off transient failures are
   logged and tolerated; three in a row past the wear-out/hardware-fault range this project
   is willing to silently absorb. */
#define FEB_WARDRIVING_FLASH_FAILURE_LIMIT 3u
wifi_scan_source_t feb_wifi_scan_active_source;
ble_scan_source_t feb_ble_scan_active_source;
static uint8_t wardriving_flash_failure_count;
/* Set and consumed within the same NimBLE-host-task function (feb_wifi_scan_done_cb()/
   feb_ble_scan_window_close_cb() respectively) when FEB_WARDRIVING_FLASH_FAILURE_LIMIT is
   reached mid-loop -- deferred via this flag rather than calling feb_wardriving_self_stop()
   directly from inside the loop so the loop can `break` and unwind cleanly first. (Before
   the G30 fix, the Wi-Fi source's loop ran on sys_evt and this flag really did cross a task
   boundary; now both sources are symmetric: same task sets and reads it.) */
static bool wardriving_wifi_self_stop_pending;
static bool wardriving_ble_self_stop_pending;

/* docs/PLAN.md "Wi-Fi scan capability" step. wifi_scan_raw_records/wifi_scan_selected are
   written exactly once per scan by feb_wifi_scan_done_handler() (runs on the default event
   loop's own task, per docs/PLAN.md's scan-execution-model decision) and then only ever read
   by code running on the NimBLE host task (feb_wifi_scan_done_cb() and everything it calls) --
   handed off safely via the feb_wifi_scan_done_co callout below, the same
   cross-task-safe-scheduling mechanism feb_reassembly_timeout_co already uses, rather than a
   second lock. No new scan can start (feb_wifi_scan_in_progress gates feb_handle_command()) until
   that handoff's consumer clears it, so there is never a concurrent writer while the NimBLE
   task is reading. */
#if FEB_HAS_CLUSTER_WORKER
/* Also appended to by the board's cluster glue (see feb_app_internal.h), so external. */
#define wifi_scan_raw_records feb_wifi_scan_raw_records
#define wifi_scan_raw_count feb_wifi_scan_raw_count
wifi_ap_record_t wifi_scan_raw_records[FEB_WIFI_SCAN_RAW_MAX];
#else
static wifi_ap_record_t wifi_scan_raw_records[FEB_WIFI_SCAN_RAW_MAX];
#endif
/* G30 fix: number of valid entries in wifi_scan_raw_records[] from the scan that just
   completed -- set by feb_wifi_scan_done_handler() (sys_evt task) right before it hands off to
   feb_wifi_scan_done_cb() (NimBLE host task) via feb_wifi_scan_done_co, same handoff shape as
   feb_ble_scan_raw_count already uses for the BLE source. Needed because the wardriving-source
   dedup/append loop that used to run directly in feb_wifi_scan_done_handler() (a real cross-task
   race against the NimBLE-host-task-only wardriving log reader -- see docs/BACKLOG.md G30) now
   runs in feb_wifi_scan_done_cb() instead, so it needs this count carried across the handoff. */
#if FEB_HAS_CLUSTER_WORKER
uint16_t wifi_scan_raw_count;
#else
static uint16_t wifi_scan_raw_count;
#endif
static feb_wifi_scan_ap_t wifi_scan_selected[FEB_WIFI_SCAN_MAX_APS_PER_RECORD];
static uint16_t wifi_scan_found_count;
static uint16_t wifi_scan_send_next_index;
volatile bool feb_wifi_scan_in_progress;
static uint64_t wifi_scan_request_id;
struct ble_npl_callout feb_wifi_scan_done_co;

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": per-window BLE device
   catalog, written only from feb_gap_event()'s BLE_GAP_EVENT_DISC case (NimBLE host task) while
   feb_ble_scan_in_progress is set, and read only from feb_ble_scan_window_close_cb() /
   feb_ble_scan_send_next_batch() -- both also on the NimBLE host task via the same callout
   mechanism feb_reassembly_timeout_co/feb_wifi_scan_done_co already use. Unlike wifi_scan, there is
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
uint16_t feb_ble_scan_raw_count;
static feb_ble_scan_device_t ble_scan_selected[FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD];
static uint16_t ble_scan_found_count;
static uint16_t ble_scan_send_next_index;
volatile bool feb_ble_scan_in_progress;
static uint64_t ble_scan_request_id;
struct ble_npl_callout feb_ble_scan_done_co;
static void wifi_scan_select_top32(uint16_t raw_count);

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

/* Selects the FEB_WIFI_SCAN_MAX_APS_PER_RECORD (32) strongest of wifi_scan_raw_records[0..
   raw_count) by RSSI and converts them into wire-ready feb_wifi_scan_ap_t entries in
   wifi_scan_selected[], in place. Factored out of feb_wifi_scan_done_handler() (DR6,
   docs/SOURCE_SPLIT.md) to match the same extraction already done on Heltec, where a
   cluster-worker-sourced batch also needs it. */
static void wifi_scan_select_top32(uint16_t raw_count)
{
    uint16_t keep = (raw_count < FEB_WIFI_SCAN_MAX_APS_PER_RECORD) ? raw_count : FEB_WIFI_SCAN_MAX_APS_PER_RECORD;
    uint16_t k;

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
           results are fully sent -- see feb_wifi_scan_in_progress's comment above). ESP-IDF's
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
}

#if FEB_HAS_CLUSTER_WORKER
/* The cluster glue's worker-sourced manual scan completion reuses the same selection. */
void feb_wifi_scan_select_top32(uint16_t raw_count)
{
    wifi_scan_select_top32(raw_count);
}
#endif

/* Runs on the default event loop's own task (sys_evt), never the NimBLE host task -- per
   docs/PLAN.md's scan-execution-model decision, a multi-second blocking scan must not run
   inside the protected-record dispatch handler on the NimBLE host task. Fetches results,
   selects the FEB_WIFI_SCAN_MAX_APS_PER_RECORD (32) strongest by RSSI (PROTOCOL.md's result
   cap), converts them into wire-ready feb_wifi_scan_ap_t entries, then hands off to the
   NimBLE host task via feb_wifi_scan_done_co (same cross-task-safe callout-scheduling mechanism
   feb_reassembly_timeout_co uses) to actually build and send status records, since that touches
   feb_connection_handle/feb_rt_tx_sequence/tx_fragment_* state owned by the NimBLE host task. */
void feb_wifi_scan_done_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint16_t total_found = 0;
    uint16_t raw_count;

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

    if (feb_wifi_scan_active_source == WIFI_SCAN_SOURCE_WARDRIVING) {
        /* G30 fix: the wardriving dedup/append loop that used to run right here (on sys_evt)
           now runs in feb_wifi_scan_done_cb() on the NimBLE host task instead -- the same task
           that reads this log via feb_wardriving_send_next_batch() -- so the two can never
           interleave. This handler's only remaining job for the wardriving source is to hand
           the raw scan results across that task boundary, same shape as the non-wardriving
           branch below already uses feb_wifi_scan_done_co for. */
        wifi_scan_raw_count = raw_count;
        ble_npl_callout_reset(&feb_wifi_scan_done_co, 0);
        return;
    }

    wifi_scan_select_top32(raw_count);
    ble_npl_callout_reset(&feb_wifi_scan_done_co, 0);
}

/* Runs on the NimBLE host task (feb_wifi_scan_done_co's queue). */
void feb_wifi_scan_done_cb(struct ble_npl_event *ev)
{
    (void)ev;

    if (feb_wifi_scan_active_source == WIFI_SCAN_SOURCE_WARDRIVING) {
        /* G30 fix: this dedup/append loop used to run in feb_wifi_scan_done_handler() on the
           sys_evt task -- a real race against feb_wardriving_send_next_batch()'s log reads, which
           only ever run here on the NimBLE host task (see docs/BACKLOG.md G30). Moved here so
           the wardriving log's writer and reader are always the same task, matching the
           BLE-source sibling below (feb_ble_scan_window_close_cb()), which was already safe for
           exactly this reason. For the local-radio source, wifi_scan_raw_count/
           wifi_scan_raw_records are populated by feb_wifi_scan_done_handler() just before it
           hands off to this callout, with no concurrent writer left by the time this runs.
           For the Phase 9 cluster-delegated source (feb_wardriving_wifi_delegated), the board's
           cluster RX task may still be actively appending scan_result frames into that same
           array on its own task as this flush runs (continuous mode has no batch_done to pause
           it at) -- snapshot under the cluster spinlock into a scratch buffer first
           (feb_cluster_flush_snapshot()), rather than reading the live array concurrently with
           its writer. */
        feb_location_t fix;
#if FEB_HAS_CLUSTER_WORKER
        feb_location_state_t loc_state;
        wifi_ap_record_t *raw;
        uint16_t raw_count;
#else
        feb_location_state_t loc_state = location_get_fix(&fix);
#endif
        uint16_t k;
#if FEB_HAS_CLUSTER_WORKER
        bool flush_held = false;

        if (feb_wardriving_wifi_delegated) {
            /* HARDENING_PLAN.md HP-04: take the flush-buffer pointer and mark it busy under the
               lock that guards it -- feb_radio_kill_switch_toggle() (a different task) can
               release it via feb_wardriving_stop_internal() at any time; while busy that
               release defers the free() to the end of this flush. A NULL read (the release
               landed first) means nothing to flush. */
            flush_held = feb_cluster_flush_snapshot(&raw, &raw_count);
        } else {
            raw_count = wifi_scan_raw_count;
            raw = wifi_scan_raw_records;
        }

        loc_state = location_get_fix(&fix);
        if (loc_state != FEB_LOCATION_FIX) {
            ESP_LOGW(TAG, "wardriving: discarding %u wifi result(s), no GPS fix yet",
                     (unsigned)raw_count);
        } else {
            for (k = 0; k < raw_count; k++) {
                wifi_ap_record_t *rec = &raw[k];
#else
        if (loc_state != FEB_LOCATION_FIX) {
            ESP_LOGW(TAG, "wardriving: discarding %u wifi result(s), no GPS fix yet",
                     (unsigned)wifi_scan_raw_count);
        } else {
            for (k = 0; k < wifi_scan_raw_count; k++) {
                wifi_ap_record_t *rec = &wifi_scan_raw_records[k];
#endif
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

#if FEB_HAS_CLUSTER_WORKER
        /* Must clear busy before feb_wardriving_self_stop() below, which releases the buffer
           on this same task. */
        if (flush_held) {
            feb_cluster_flush_finish(raw);
        }

#endif
        if (wardriving_wifi_self_stop_pending) {
            wardriving_wifi_self_stop_pending = false;
            feb_wardriving_self_stop("internal_error");
            return;
        }
        if (!feb_wardriving_wifi_active) {
            /* A stop() raced this scan's completion -- feb_handle_wardriving_command()'s stop
               path already cleared feb_wifi_scan_in_progress; nothing else to do. */
            return;
        }
        {
            /* One flush-window sample per Wi-Fi pass, whether or not it had a fix. */
            uint16_t now_total = wardriving_dedup_appended_total();

            wardriving_flush_window_push(&feb_wardriving_flush_window,
                                          (uint16_t)(now_total - feb_wardriving_flush_last_appended));
            feb_wardriving_flush_last_appended = now_total;
        }
        feb_wardriving_maybe_kick_send(feb_connection_handle);
        ble_npl_callout_reset(&feb_wardriving_wifi_interval_co,
                              ble_npl_time_ms_to_ticks32(feb_wardriving_wifi_interval_ms));
        return;
    }

#if FEB_DIAG_WIFI_HEAP_LOG
    /* docs/HARDENING_PLAN.md HP-21 (BL18 heap-pressure hypothesis): measurement only, not a
       fix -- see the matching "before scan start" log in feb_handle_wifi_scan_command(). */
    ESP_LOGI(TAG, "wifi_scan: free_heap=%" PRIu32 " largest_free_block=%u at scan done (%u AP(s) found)",
             esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)wifi_scan_found_count);
#endif

    if (feb_connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        feb_runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        ESP_LOGW(TAG, "wifi_scan completed with no authenticated connection; discarding %u result(s)",
                 (unsigned)wifi_scan_found_count);
        feb_wifi_scan_in_progress = false;
        return;
    }
    feb_wifi_scan_send_next_batch(feb_connection_handle);
}

/* Builds and sends one wifi_scan `status` record starting at wifi_scan_send_next_index,
   packing as many remaining APs as fit under FEB_CBOR_MAX_PAYLOAD (minus headroom for the
   status payload's own wrapper fields), then chains the next batch (if any) via
   TX_DONE_CONTINUE_WIFI_SCAN once this record's fragments finish sending -- fragmentation is
   single-in-flight (docs/PROTOCOL.md#fragmentation), so the next batch cannot be queued until
   this one's tx_fragment_* state is free again. */
void feb_wifi_scan_send_next_batch(uint16_t conn_handle)
{
    /* static, not stack-local: this function (and everything it calls) runs on the
       ~4084-byte nimble_host task, invoked only from feb_write_complete()'s
       TX_DONE_CONTINUE_WIFI_SCAN case and feb_wifi_scan_done_cb() -- both on that same task,
       never reentrant/concurrent (fragmentation is single-in-flight per
       docs/PROTOCOL.md#fragmentation, and feb_wifi_scan_in_progress gates a second scan from
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

    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf), &status_payload);
    if (payload_len == 0 ||
        !feb_queue_and_send_protected(conn_handle, "status", strlen("status"),
                                  feb_pairing_payload_encode_buf, payload_len,
                                  is_complete ? TX_DONE_NONE : TX_DONE_CONTINUE_WIFI_SCAN)) {
        ESP_LOGE(TAG, "failed to build wifi_scan status record");
        feb_wifi_scan_in_progress = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "sending wifi_scan status (%s, %u AP(s) this batch)",
             is_complete ? "complete" : "partial", (unsigned)result.ap_count);

    if (is_complete) {
        feb_wifi_scan_in_progress = false;
    }
}

/* docs/PLAN.md "Wi-Fi scan capability" step. Extracted unmodified from what used to be the
   whole body of feb_handle_command() (docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub
   reorder": that function is now a capability-name dispatcher -- see below). */
void feb_handle_wifi_scan_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    size_t arg_count;
    feb_cbor_status_t status;
#if !FEB_HAS_CLUSTER_WORKER
    esp_err_t err;
    wifi_scan_config_t scan_cfg;
#endif

    if (feb_cbor_decode_map_header(cmd->arguments_span, cmd->arguments_span_len, &arg_count, &status) == 0 ||
        arg_count != 0) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (feb_wifi_scan_in_progress) {
        if (!feb_send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    /* docs/PROTOCOL.md's wifi_scan busy-handling rule ("also rejected busy if wardriving's
       Wi-Fi source is currently active") is already satisfied by the feb_wifi_scan_in_progress
       check above -- wardriving's Wi-Fi source sets that same flag for its entire enabled
       lifetime (see wifi_scan_source_t's comment), so there is nothing extra to check here. */

    feb_wifi_scan_in_progress = true;
    feb_wifi_scan_active_source = WIFI_SCAN_SOURCE_MANUAL;
    wifi_scan_request_id = cmd->request_id;
#if FEB_HAS_CLUSTER_WORKER
    /* Phase 9 (docs/CLUSTER.md): a present 2.4GHz cluster worker (esp32/cluster_worker/) takes
       the scan instead of the local radio; the Flipper-facing reply shape is identical either
       way (docs/CLUSTER.md's "What doesn't change": no PROTOCOL.md change). */
    if (feb_cluster_wifi_scan_delegate(cmd->request_id)) {
        return;
    }
    feb_wifi_scan_start_local(conn_handle);
#else
    memset(&scan_cfg, 0, sizeof(scan_cfg));
    if (feb_app_hooks->wifi_scan_cfg_ext != NULL) {
        feb_app_hooks->wifi_scan_cfg_ext(&scan_cfg);
    }
#if FEB_DIAG_WIFI_HEAP_LOG
    /* docs/HARDENING_PLAN.md HP-21 (BL18 heap-pressure hypothesis): dual-band scans see more
       APs than the 2.4GHz-only case, and the IDF driver's internal AP list scales with that,
       outside this app's own bounded FEB_WIFI_SCAN_RAW_MAX array. Measurement only -- not a
       fix -- to have real numbers on hand for a future hardware chase of BL18. */
    ESP_LOGI(TAG, "wifi_scan: free_heap=%" PRIu32 " largest_free_block=%u before scan start",
             esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        feb_wifi_scan_in_progress = false;
        if (!feb_send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    ESP_LOGI(TAG, "wifi_scan started (request_id=%llu)", (unsigned long long)cmd->request_id);
#endif
}

#if FEB_HAS_CLUSTER_WORKER
/* Local-radio manual scan start, shared by feb_handle_wifi_scan_command() and the cluster
   glue's worker-timeout fallback (so that fallback re-enters this exact path without
   duplicating it) -- uses wifi_scan_request_id/feb_wifi_scan_in_progress, already set by the
   caller before either this or the worker-delegation path is chosen. */
void feb_wifi_scan_start_local(uint16_t conn_handle)
{
    wifi_scan_config_t scan_cfg;
    esp_err_t err;

    memset(&scan_cfg, 0, sizeof(scan_cfg));
    if (feb_app_hooks->wifi_scan_cfg_ext != NULL) {
        feb_app_hooks->wifi_scan_cfg_ext(&scan_cfg);
    }
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        feb_wifi_scan_in_progress = false;
        if (!feb_send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                      1, wifi_scan_request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    ESP_LOGI(TAG, "wifi_scan started locally (request_id=%llu)",
             (unsigned long long)wifi_scan_request_id);
}
#endif

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": mirrors
   feb_handle_wifi_scan_command()'s validation/busy-check/start shape exactly (arguments must be
   an empty map; busy guard; feb_own_addr_type-scoped discovery start), substituting a fixed-
   duration passive BLE discovery window for a Wi-Fi scan. The window itself is closed by
   feb_ble_scan_done_co (armed here), not by any GAP "discovery complete" callback -- NimBLE
   passive discovery with BLE_HS_FOREVER runs until explicitly cancelled. */
void feb_handle_ble_scan_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    size_t arg_count;
    feb_cbor_status_t status;
    struct ble_gap_disc_params params = {0};
    int rc;

    if (feb_cbor_decode_map_header(cmd->arguments_span, cmd->arguments_span_len, &arg_count, &status) == 0 ||
        arg_count != 0) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (feb_ble_scan_in_progress) {
        if (!feb_send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    /* docs/PROTOCOL.md's ble_scan busy-handling rule ("also rejected busy if wardriving's
       BLE source is currently active") is already satisfied by the feb_ble_scan_in_progress
       check above -- wardriving's BLE source sets that same flag for its entire enabled
       lifetime (see ble_scan_source_t's comment), so there is nothing extra to check here. */

    feb_ble_scan_in_progress = true;
    feb_ble_scan_active_source = BLE_SCAN_SOURCE_MANUAL;
    ble_scan_request_id = cmd->request_id;
    feb_ble_scan_raw_count = 0;

    params.passive = 0;
    /* Active scanning: send scan requests and collect scan response data, which often
       includes device names that passive advertisements omit. Trade latency for name
       discovery (docs/PLAN.md backlog: "BLE active scanning on/off toggle"). */
    /* No controller dup-filtering here (unlike feb_start_scan()'s reconnect-scan concern) --
       ble_scan wants every advertisement so it can track each address's strongest RSSI
       itself; see feb_ble_scan_catalog_advertisement()'s own dedup-by-address handling. */
    params.filter_duplicates = 0;
    params.itvl = 0;
    params.window = 0;
    rc = ble_gap_disc(feb_own_addr_type, BLE_HS_FOREVER, &params, feb_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_scan: ble_gap_disc start failed: %d", rc);
        feb_ble_scan_in_progress = false;
        if (!feb_send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    ble_npl_callout_reset(&feb_ble_scan_done_co, ble_npl_time_ms_to_ticks32(FEB_BLE_SCAN_WINDOW_MS));
    ESP_LOGI(TAG, "ble_scan started (request_id=%llu)", (unsigned long long)cmd->request_id);
}

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": parses one
   BLE_GAP_EVENT_DISC advertisement's fields (reusing ble_hs_adv_parse_fields(), the same
   primitive scan_record_matches() already uses in this same call path -- no duplicated
   parsing) and folds it into ble_scan_raw_devices[], deduping by address within the current
   window and keeping the strongest RSSI seen. `struct ble_hs_adv_fields` is a small,
   pointer/scalar-only local (no embedded arrays) -- the same stack footprint
   scan_record_matches() already carries on this exact call path, so this adds no new stack
   risk beyond what's already accepted there. */
void feb_ble_scan_catalog_advertisement(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    uint16_t index;
    ble_scan_raw_device_t *dev;

    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return;
    }

    for (index = 0; index < feb_ble_scan_raw_count; index++) {
        if (memcmp(ble_scan_raw_devices[index].addr, disc->addr.val, FEB_BLE_SCAN_ADDRESS_LEN) == 0) {
            break;
        }
    }

    if (index == feb_ble_scan_raw_count) {
        if (feb_ble_scan_raw_count >= FEB_BLE_SCAN_RAW_MAX) {
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
        feb_ble_scan_raw_count++;
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
   mirroring feb_wifi_scan_send_next_batch()'s under-512-byte packing/chaining exactly --
   see that function's comment for why result/trial/status_payload/result_buf are static,
   not stack-local (same nimble_host task, same ~4084-byte budget, same
   single-in-flight-fragmentation reentrancy argument). */
void feb_ble_scan_send_next_batch(uint16_t conn_handle)
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

    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf), &status_payload);
    if (payload_len == 0 ||
        !feb_queue_and_send_protected(conn_handle, "status", strlen("status"),
                                  feb_pairing_payload_encode_buf, payload_len,
                                  is_complete ? TX_DONE_NONE : TX_DONE_CONTINUE_BLE_SCAN)) {
        ESP_LOGE(TAG, "failed to build ble_scan status record");
        feb_ble_scan_in_progress = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "sending ble_scan status (%s, %u device(s) this batch)",
             is_complete ? "complete" : "partial", (unsigned)result.device_count);

    if (is_complete) {
        feb_ble_scan_in_progress = false;
    }
}

/* Fires once FEB_BLE_SCAN_WINDOW_MS after feb_handle_ble_scan_command() armed this callout --
   same ble_npl_callout mechanism as feb_reassembly_timeout_co/feb_wifi_scan_done_co, running on the
   NimBLE host task. Cancels the still-running discovery, selection-sorts the top
   FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD by RSSI (mirrors feb_wifi_scan_done_handler()'s pattern),
   then sends -- or, if the connection that requested this scan is gone, discards, exactly
   like feb_wifi_scan_done_cb()'s "no authenticated connection" branch. */
void feb_ble_scan_window_close_cb(struct ble_npl_event *ev)
{
    uint16_t keep;
    uint16_t k;
    int rc;

    (void)ev;

    rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_scan: ble_gap_disc_cancel at window close failed: %d", rc);
    }

    if (feb_ble_scan_active_source == BLE_SCAN_SOURCE_WARDRIVING) {
        /* wardriving logs every cataloged device this window (up to the same
           FEB_BLE_SCAN_RAW_MAX self-imposed bound feb_ble_scan_catalog_advertisement() already
           applies), not just the top FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD by RSSI -- that cap
           is specific to ble_scan's one-shot *reporting* contract, which this persistent
           capture log has no equivalent of. */
        feb_location_t fix;
        feb_location_state_t loc_state = location_get_fix(&fix);
        uint16_t k;

        if (loc_state != FEB_LOCATION_FIX) {
            ESP_LOGW(TAG, "wardriving: discarding %u ble result(s), no GPS fix yet",
                     (unsigned)feb_ble_scan_raw_count);
        } else {
            for (k = 0; k < feb_ble_scan_raw_count; k++) {
                ble_scan_raw_device_t *rec = &ble_scan_raw_devices[k];
                feb_wardriving_record_t record;

                /* BL09: never capture the paired Flipper's own advertisement into the
                   wardriving flash log/CSV -- it's always in range (it's the device we're
                   connected to, or reconnecting to) and isn't a wardriving *find*. Manual
                   ble_scan results are untouched; that path never reaches this branch. */
                if (feb_ble_addr_is_paired_peer(rec->addr)) {
                    continue;
                }

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
            feb_wardriving_self_stop("internal_error");
            return;
        }
        if (!feb_wardriving_ble_active) {
            /* A stop() raced this window's completion -- feb_handle_wardriving_command()'s stop
               path already cleared feb_ble_scan_in_progress; nothing else to do. */
            return;
        }
        /* BLE-only runs have no Wi-Fi pass to sample the flush window from -- sub-sample
           every FEB_WARDRIVING_FLUSH_BLE_WINDOWS_PER_SAMPLE window closes instead. */
        if (!feb_wardriving_wifi_active &&
            ++feb_wardriving_flush_ble_windows >= FEB_WARDRIVING_FLUSH_BLE_WINDOWS_PER_SAMPLE) {
            uint16_t now_total = wardriving_dedup_appended_total();

            wardriving_flush_window_push(&feb_wardriving_flush_window,
                                          (uint16_t)(now_total - feb_wardriving_flush_last_appended));
            feb_wardriving_flush_last_appended = now_total;
            feb_wardriving_flush_ble_windows = 0;
        }
        feb_wardriving_maybe_kick_send(feb_connection_handle);
        {
            uint32_t gap_ms = (feb_wardriving_ble_interval_ms > feb_wardriving_ble_window_ms) ?
                              (feb_wardriving_ble_interval_ms - feb_wardriving_ble_window_ms) : 0u;

            ble_npl_callout_reset(&feb_wardriving_ble_interval_co, ble_npl_time_ms_to_ticks32(gap_ms));
        }
        return;
    }

    keep = (feb_ble_scan_raw_count < FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) ?
           feb_ble_scan_raw_count : FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD;
    for (k = 0; k < keep; k++) {
        uint16_t best = k;
        uint16_t j;
        ble_scan_raw_device_t *rec;
        feb_ble_scan_device_t *out;

        for (j = (uint16_t)(k + 1); j < feb_ble_scan_raw_count; j++) {
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

    if (feb_connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        feb_runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        ESP_LOGW(TAG, "ble_scan window closed with no authenticated connection; discarding %u result(s)",
                 (unsigned)ble_scan_found_count);
        feb_ble_scan_in_progress = false;
        return;
    }
    feb_ble_scan_send_next_batch(feb_connection_handle);
}

#include "feb_app_internal.h"

#define MAX_RECONNECT_RETRIES 5
#define FEB_RX_FRAGMENT_BUFFER_SIZE 256u
/* Cadence for the aggregate BLE_GAP_EVENT_DISC summary log emitted from
   feb_reassembly_timeout_cb() -- piggybacks on that existing callout rather than a second timer. */
#define FEB_SCAN_SUMMARY_INTERVAL_MS 10000u

/* docs/PLAN.md step 6: how long the ESP32 waits for hello_ack after sending hello before
   treating it as a proof-verification-class failure (docs/PROTOCOL.md doesn't pin a number;
   a couple of small BLE round trips over an already-established connection are realistically
   sub-second, so 5s is generous margin, not a tight bound). */
#define FEB_HELLO_ACK_TIMEOUT_MS 5000u
/* Same RTT budget as FEB_HELLO_ACK_TIMEOUT_MS: one round trip over the same BLE link,
   bounding how long the ESP32 waits for the Flipper's pair_reply after pair_init. */
#define FEB_PAIR_REPLY_TIMEOUT_MS 5000u
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
   gap proactively. Only enforced once feb_runtime_auth_state is AUTHENTICATED, matching the
   spec text ("idle authenticated connection"). */
#define FEB_IDLE_TIMEOUT_MS 30000u

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

/* docs/HARDENING_BACKLOG.md H05 Phase A: tighten the connection interval for faster
   wardriving-record bulk transfer. itvl 6-12 (7.5-15ms) sits inside the Flipper's own
   accepted range (flipper_esp32_over_ble.c's conn_int_min=6/conn_int_max=36) and matches
   its own preference for the low end. supervision_timeout=400 (4s) clears the spec's
   supervision_timeout_ms > (1+latency)*itvl_max_ms*2 requirement (30ms here) with large
   margin -- also stricter than NimBLE's own connect-default of 2.56s. scan_itvl/scan_window/
   min_ce_len/max_ce_len left at NimBLE's own ble_gap_connect() defaults (0x0010/0x0010/0/0).
   Applied for the whole connection lifetime, not just during wardriving publish -- this is
   a battery-tolerant, screen-on use case, and a transfer-scoped toggle would add a second
   state machine to reason about alongside the existing TX single-flight logic. */
static const struct ble_gap_conn_params tight_conn_params = {
    .scan_itvl = 0x0010,
    .scan_window = 0x0010,
    .itvl_min = 6,
    .itvl_max = 12,
    .latency = 0,
    .supervision_timeout = 400,
    .min_ce_len = 0,
    .max_ce_len = 0,
};

uint8_t feb_own_addr_type;
uint16_t feb_connection_handle = BLE_HS_CONN_HANDLE_NONE;
/* BL09: last-known BLE (OTA) address of the paired Flipper peer, captured from
   ble_gap_conn_find() on every successful BLE_GAP_EVENT_CONNECT below. Deliberately not
   cleared on disconnect -- wardriving's BLE capture keeps running across a disconnect
   (docs/PROTOCOL.md), and the Flipper is typically still advertising with the same address
   during that gap, so the filter in feb_cap_scan.c's wardriving branch must keep matching it
   even while feb_connection_handle is BLE_HS_CONN_HANDLE_NONE. Only 6 bytes, so a plain
   static (not a ring buffer of past peers) is enough -- this project pairs with exactly one
   Flipper at a time. */
static bool feb_peer_ble_addr_known;
static uint8_t feb_peer_ble_addr[FEB_BLE_SCAN_ADDRESS_LEN];
static uint16_t service_start_handle;
static uint16_t service_end_handle;
uint16_t feb_write_value_handle;
static uint16_t notify_value_handle;
static uint16_t notify_cccd_handle;
uint16_t feb_negotiated_att_mtu = 23;
static uint8_t reconnect_retries;
static bool reconnect_task_active;
static uint32_t scan_report_window_count;
static uint32_t scan_report_lifetime_total;
static uint32_t scan_summary_elapsed_ms;

static feb_reassembly_t rx_reassembly;
struct ble_npl_callout feb_reassembly_timeout_co;
/* G19 fix: a single reused callout for both reconnect backoff paths below, instead of
   xTaskCreate()-ing a 3072-byte one-shot task just to vTaskDelay() once then feb_start_scan() --
   same cross-task-timer pattern as every other *_co callout in this component. reconnect_task_active
   still gates re-entrancy exactly as before; only the sleep mechanism changed. */
struct ble_npl_callout feb_reconnect_co;
static uint32_t last_record_activity_ms; /* reset on connect and on each record received or
                                             fully sent (feb_write_complete()) -- docs/PROTOCOL.md's
                                             "without a record" is undirected; a wardriving-style
                                             session that only ever sends (never receives) must
                                             still count as live. See docs/LESSONS.md 2026-09-10. */
static uint64_t rt_rx_sequence;
static uint8_t rt_plaintext_buf[FEB_CBOR_MAX_PAYLOAD];
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
static void schedule_runtime_auth_backoff(void);

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

/* BL09: true if `addr` (a raw 6-byte BLE address, as recorded from ble_gap_disc_desc.addr.val
   by feb_ble_scan_catalog_advertisement()) matches the paired Flipper's last-known OTA
   address. Used by feb_cap_scan.c's wardriving-BLE-capture branch to keep the Flipper's own
   advertisement out of the wardriving flash log/CSV -- manual ble_scan results are untouched,
   they don't call this. Returns false (never filters) before any successful connection has
   ever captured an address, e.g. a factory-reset board's first boot. */
bool feb_ble_addr_is_paired_peer(const uint8_t addr[FEB_BLE_SCAN_ADDRESS_LEN])
{
    return feb_peer_ble_addr_known && memcmp(addr, feb_peer_ble_addr, FEB_BLE_SCAN_ADDRESS_LEN) == 0;
}

void feb_start_scan(void)
{
    struct ble_gap_disc_params params = {0};
    int rc;

#if FEB_HAS_LINK_HOOKS
    /* Heltec: the touch-pad radio kill switch gates every scan/reconnect attempt, so a
       shutdown in progress cannot race a fresh scan being armed. */
    if (feb_app_hooks->radio_permitted != NULL && !feb_app_hooks->radio_permitted()) {
        return;
    }
#endif
    if (feb_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    /* docs/PLAN.md "Revised long-run reconnect policy": never run a second, dedicated
       reconnect scan while wardriving's BLE source owns discovery -- its own re-arm
       (feb_wardriving_ble_interval_cb()) already delivers every advertisement through the same
       feb_gap_event()/BLE_GAP_EVENT_DISC handler this scan would use, including the merged
       reconnect-match check. BLE_GAP_EVENT_DISC_COMPLETE already guarded its own call this
       way; root-caused 2026-09-10 (docs/LESSONS.md) that every *other* caller of this
       function (BLE_GAP_EVENT_DISCONNECT's two reconnect paths, reconnect_task()) did not,
       so a reconnect landing while wardriving's BLE capture was active raced its own
       ble_gap_disc() re-arm and failed one of the two with BLE_HS_EBUSY. Centralized here so
       every caller is covered by construction instead of needing its own guard. */
    if (feb_wardriving_ble_active) {
        return;
    }
    if (!feb_connecting_permitted()) {
        ESP_LOGI(TAG, "pairing window closed; staying idle");
#if FEB_HAS_LINK_HOOKS
        feb_hook_ble_state(FEB_BLE_LINK_DISCONNECTED);
#endif
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
    rc = ble_gap_disc(feb_own_addr_type, BLE_HS_FOREVER, &params, feb_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
#if FEB_HAS_LINK_HOOKS
        feb_hook_ble_state(FEB_BLE_LINK_DISCONNECTED);
    } else {
        feb_hook_ble_state(feb_pairing_window_is_open() ? FEB_BLE_LINK_PAIRING_MODE :
                                                          FEB_BLE_LINK_SCANNING);
#endif
    }
}

/* Runs on the NimBLE host task (feb_reconnect_co's queue) once the scheduled backoff delay
   elapses -- see feb_reconnect_co's own comment for why this replaced a per-call xTaskCreate(). */
void feb_reconnect_timer_cb(struct ble_npl_event *ev)
{
    (void)ev;
    reconnect_task_active = false;
    feb_start_scan();
}

/* Mirrors feb_runtime_auth_backoff_delay_ms()'s exponential-then-flatten shape, but with its
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

    if (!feb_connecting_permitted()) {
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
    ble_npl_callout_reset(&feb_reconnect_co, ble_npl_time_ms_to_ticks32(delay_ms));
}

/* Independent of reconnect_retries/MAX_RECONNECT_RETRIES above (which now means "length of
   the exponential ramp before flattening," not a hard cap -- reconnect_backoff_delay_ms()
   governs GAP-level connect() failures): a runtime-auth proof failure can recur even when
   the physical link connects cleanly every time, and per docs/PLAN.md step 6 must never
   stop retrying outright, only slow down -- see FEB_RUNTIME_AUTH_BACKOFF_MAX_EXP's comment. */
static void schedule_runtime_auth_backoff(void)
{
    uint32_t delay_ms = feb_runtime_auth_backoff_delay_ms();

    if (reconnect_task_active) {
        return;
    }
    reconnect_task_active = true;
    ESP_LOGI(TAG, "runtime auth backoff: retry in %lu ms (consecutive failures=%u)",
             (unsigned long)delay_ms, feb_runtime_auth_failure_count);
    ble_npl_callout_reset(&feb_reconnect_co, ble_npl_time_ms_to_ticks32(delay_ms));
}

static int mtu_exchanged(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         uint16_t mtu, void *arg)
{
    int rc;

    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "MTU exchange unavailable: %d", error->status);
    } else {
        feb_negotiated_att_mtu = mtu;
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
            feb_write_value_handle = characteristic->val_handle;
        } else if (uuid128_matches(&characteristic->uuid, &notify_uuid)) {
            notify_value_handle = characteristic->val_handle;
        }
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (feb_write_value_handle == 0 || notify_value_handle == 0) {
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
                              sizeof(notify_enabled), feb_write_complete, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "notification subscribe failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

int feb_write_complete(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    tx_done_action_t action;
    bool pending_started;

    if (error->status != 0) {
        ESP_LOGE(TAG, "GATT write failed: %d", error->status);
        feb_pairing_attempt_zeroize();
        feb_runtime_auth_zeroize();
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    if (notify_cccd_handle != 0) {
        notify_cccd_handle = 0;
        if (feb_boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH) {
            ESP_LOGI(TAG, "notifications subscribed; beginning runtime auth");
            feb_begin_runtime_auth(conn_handle);
        } else {
            ESP_LOGI(TAG, "notifications subscribed; beginning pairing ceremony");
            feb_begin_pairing(conn_handle);
        }
        return 0;
    }
    if (feb_tx_fragment_next < feb_tx_fragment_total) {
        feb_send_next_tx_fragment(conn_handle);
        return 0;
    }

    /* Every outbound record (pairing or session-protected) funnels its last fragment's
       write completion through here -- the single shared point that covers all send paths
       without per-call-site duplication. Counts as activity the same as an inbound record,
       so a wardriving-style session that only ever sends never trips the idle-timeout
       below just because the peer stays quiet. */
    last_record_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);

    action = feb_tx_done_action;
    feb_tx_done_action = TX_DONE_NONE;
    feb_tx_dispatching_completion = true;
    switch (action) {
    case TX_DONE_AWAIT_PAIR_REPLY:
        ESP_LOGI(TAG, "pair_init sent; awaiting pair_reply");
        break;
    case TX_DONE_SEND_PAIR_COMPLETE:
        feb_finish_pairing_after_confirm(conn_handle);
        break;
    case TX_DONE_DISCONNECT:
        ESP_LOGI(TAG, "pairing record sent; closing connection");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        break;
    case TX_DONE_AWAIT_HELLO_ACK:
        ESP_LOGI(TAG, "hello sent; awaiting hello_ack");
        break;
    case TX_DONE_RUNTIME_AUTHENTICATED:
        feb_runtime_auth_state = RUNTIME_AUTH_STATE_AUTHENTICATED;
#if FEB_HAS_LINK_HOOKS
        feb_hook_ble_state(FEB_BLE_LINK_AUTHENTICATED);
#endif
        feb_rt_tx_sequence = 1;
        rt_rx_sequence = 1;
        ESP_LOGI(TAG, "client_auth sent; runtime session authenticated");
        feb_status_led_set(FEB_STATUS_LED_CONNECTED);
        /* docs/PROTOCOL.md "Unsolicited backlog drain": on every authenticated session
           establishment, if the flash log holds buffered records, start draining them now
           without waiting for a `command`. */
        feb_wardriving_maybe_kick_send(conn_handle);
#if FEB_HAS_LINK_HOOKS
        /* Heltec: docs/PROTOCOL.md `mesh_log` section, same unsolicited-drain convention for
           its own independent flash log. */
        if (feb_app_hooks->on_authenticated != NULL) {
            feb_app_hooks->on_authenticated(conn_handle);
        }
#endif
        break;
    case TX_DONE_CONTINUE_WIFI_SCAN:
        feb_wifi_scan_send_next_batch(conn_handle);
        break;
    case TX_DONE_CONTINUE_BLE_SCAN:
        feb_ble_scan_send_next_batch(conn_handle);
        break;
    case TX_DONE_CONTINUE_WARDRIVING:
        feb_wardriving_send_next_batch(conn_handle);
        break;
    case TX_DONE_SEND_WARDRIVING_STOPPED: {
        feb_status_payload_t status_payload = {0};
        size_t payload_len;

        status_payload.request_id = 0;
        status_payload.state = "stopped";
        status_payload.state_len = strlen("stopped");
        payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                     sizeof(feb_pairing_payload_encode_buf), &status_payload);
        if (payload_len == 0 ||
            !feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;
    }
    case TX_DONE_BOARD_EXT:
        if (feb_app_hooks->on_tx_done_ext != NULL) {
            feb_app_hooks->on_tx_done_ext(conn_handle);
        }
        break;
    default:
        break;
    }
    feb_tx_dispatching_completion = false;
    pending_started = feb_start_next_pending_protected_send();
    if (!pending_started) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

int feb_gap_event(struct ble_gap_event *event, void *arg)
{
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        scan_report_window_count++;
        scan_report_lifetime_total++;

        /* feb_ble_scan_in_progress is shared by the manual ble_scan capability and
           wardriving's BLE source (see ble_scan_source_t's comment), so this single check
           already covers both callers -- feb_ble_scan_catalog_advertisement() itself is
           source-agnostic (just fills ble_scan_raw_devices[]); feb_ble_scan_window_close_cb()
           decides what to do with the catalog based on feb_ble_scan_active_source. */
        if (feb_ble_scan_in_progress) {
            feb_ble_scan_catalog_advertisement(&event->disc);
        }

        /* This connect-attempt path is the "merged reconnect-scan" mechanism
           (docs/PLAN.md step 2's "revised long-run reconnect policy" / step 4's
           "Accepted gap"): whichever discovery is currently running -- the dedicated
           reconnect scan (feb_start_scan()), a manual ble_scan, or wardriving's own BLE
           capture window -- delivers every advertisement through this same event handler,
           so a Flipper's v2 service UUID is matched and connected to from whatever scan
           pass happens to be active, without a second dedicated scan ever running
           alongside wardriving's BLE source (see BLE_GAP_EVENT_DISC_COMPLETE below, which
           suppresses feb_start_scan() while feb_wardriving_ble_active is true for exactly this
           reason). */
        if (feb_connection_handle == BLE_HS_CONN_HANDLE_NONE &&
            scan_record_matches(event->disc.data, event->disc.length_data)) {
            if (!feb_connecting_permitted()) {
                ESP_LOGI(TAG, "pairing window closed; not connecting to discovered peer");
                ble_gap_disc_cancel();
                return 0;
            }
            ESP_LOGI(TAG, "found v2 peer, connecting");
            ble_gap_disc_cancel();
            rc = ble_gap_connect(feb_own_addr_type, &event->disc.addr, 30000, &tight_conn_params,
                                 feb_gap_event, NULL);
            if (rc != 0) {
#if FEB_HAS_LINK_HOOKS
                feb_hook_ble_state(FEB_BLE_LINK_DISCONNECTED);
#endif
                ESP_LOGW(TAG, "connect start failed: %d", rc);
                schedule_reconnect();
            }
#if FEB_HAS_LINK_HOOKS
            else {
                feb_hook_ble_state(FEB_BLE_LINK_CONNECTING);
            }
#endif
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* Suppressed while wardriving's BLE source is active: that source re-arms its own
           next discovery window on its own cadence (feb_wardriving_ble_interval_cb()), and
           docs/PLAN.md's reconnect policy explicitly says not to run a second dedicated
           reconnect scan alongside it -- reconnect opportunities come from the same
           passive scan pass instead (see BLE_GAP_EVENT_DISC above). */
        if (feb_connection_handle == BLE_HS_CONN_HANDLE_NONE && !reconnect_task_active && !feb_wardriving_ble_active) {
            feb_start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
#if FEB_HAS_LINK_HOOKS
            feb_hook_ble_state(FEB_BLE_LINK_DISCONNECTED);
#endif
            ESP_LOGW(TAG, "connection failed: %d", event->connect.status);
            schedule_reconnect();
            return 0;
        }
#if FEB_HAS_LINK_HOOKS
        feb_hook_ble_state(feb_pairing_window_is_open() ? FEB_BLE_LINK_PAIRING :
                                                          FEB_BLE_LINK_AUTHENTICATING);
#endif
        feb_connection_handle = event->connect.conn_handle;
        reconnect_retries = 0;
        service_start_handle = 0;
        service_end_handle = 0;
        feb_write_value_handle = 0;
        notify_value_handle = 0;
        notify_cccd_handle = 0;
        feb_negotiated_att_mtu = 23;
        feb_pairing_state = PAIRING_STATE_IDLE;
        feb_runtime_auth_state = RUNTIME_AUTH_STATE_IDLE;
        feb_status_led_set(FEB_STATUS_LED_CONNECTING);
        feb_pending_disconnect_reason = DISCONNECT_REASON_NORMAL;
        feb_hello_ack_start_ms = 0;
        feb_pair_reply_wait_start_ms = 0;
        last_record_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
        feb_rt_tx_sequence = 0;
        rt_rx_sequence = 0;
        feb_tx_done_action = TX_DONE_NONE;
        feb_tx_fragment_total = 0;
        feb_tx_fragment_next = 0;
        feb_pending_protected_tx_head = 0;
        feb_pending_protected_tx_count = 0;
        feb_tx_dispatching_completion = false;
        feb_reassembly_reset(&rx_reassembly);
        feb_wardriving_tx_in_flight = false; /* per-connection only -- wardriving_{wifi,ble}_active
                                             deliberately persist across connect/disconnect */
        feb_wardriving_pending_drain_count = 0;
        feb_wardriving_last_reported_backlog = UINT64_MAX; /* force a fresh count report on the new
            session -- the Flipper resets its own displayed count to 0 on every reconnect */
#if FEB_HAS_LINK_HOOKS
        if (feb_app_hooks->on_connect != NULL) {
            feb_app_hooks->on_connect();
        }
#endif
        {
            /* BL09: remember this peer's OTA address for the wardriving BLE-capture filter
               (feb_ble_addr_is_paired_peer() below). peer_ota_addr, not peer_id_addr -- it's
               the address actually used over the air for this connection, the same value/type
               feb_ble_scan_catalog_advertisement() records from ble_gap_disc_desc.addr during
               discovery, so a raw byte compare against a captured wardriving BLE record's
               address (also sourced from ble_gap_disc_desc.addr) never has to reconcile a
               type mismatch against an RPA-resolved identity address. */
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(feb_connection_handle, &desc) == 0) {
                memcpy(feb_peer_ble_addr, desc.peer_ota_addr.val, FEB_BLE_SCAN_ADDRESS_LEN);
                feb_peer_ble_addr_known = true;
            } else {
                ESP_LOGW(TAG, "ble_gap_conn_find failed; wardriving peer-address filter stale");
            }
        }
        rc = ble_gap_set_prefered_le_phy(feb_connection_handle, BLE_GAP_LE_PHY_2M_MASK,
                                         BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_CODED_ANY);
        if (rc != 0) {
            ESP_LOGW(TAG, "2M PHY request failed: %d (staying on negotiated PHY)", rc);
        } else {
            ESP_LOGI(TAG, "2M PHY requested");
        }

        ESP_LOGI(TAG, "connected; exchanging MTU");
        rc = ble_gattc_exchange_mtu(feb_connection_handle, mtu_exchanged, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "MTU exchange start failed: %d", rc);
            mtu_exchanged(feb_connection_handle, &(struct ble_gatt_error){0}, 23, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        disconnect_reason_t reason = feb_pending_disconnect_reason;

        ESP_LOGW(TAG, "disconnected: reason=%d", event->disconnect.reason);
        feb_connection_handle = BLE_HS_CONN_HANDLE_NONE;
#if FEB_HAS_LINK_HOOKS
        feb_hook_ble_state(FEB_BLE_LINK_DISCONNECTED);
#endif
        service_start_handle = 0;
        service_end_handle = 0;
        feb_write_value_handle = 0;
        notify_value_handle = 0;
        notify_cccd_handle = 0;
        feb_pairing_state = PAIRING_STATE_IDLE;
        feb_runtime_auth_state = RUNTIME_AUTH_STATE_IDLE;
        feb_status_led_set(FEB_STATUS_LED_CONNECTING);
        feb_hello_ack_start_ms = 0;
        feb_pair_reply_wait_start_ms = 0;
        feb_pending_disconnect_reason = DISCONNECT_REASON_NORMAL;
        feb_tx_done_action = TX_DONE_NONE;
        feb_tx_fragment_total = 0;
        feb_tx_fragment_next = 0;
        feb_pending_protected_tx_head = 0;
        feb_pending_protected_tx_count = 0;
        feb_tx_dispatching_completion = false;
        feb_reassembly_reset(&rx_reassembly);
        feb_pairing_attempt_zeroize();
        feb_runtime_auth_zeroize();
        feb_wardriving_tx_in_flight = false;
#if FEB_HAS_LINK_HOOKS
        if (feb_app_hooks->on_disconnect != NULL) {
            feb_app_hooks->on_disconnect();
        }
#endif

        if (feb_wifi_scan_in_progress && feb_wifi_scan_active_source == WIFI_SCAN_SOURCE_MANUAL) {
#if FEB_HAS_CLUSTER_WORKER
            /* Phase 9 cluster-worker-sourced scan in flight: unlike the local-scan branch
               below, no esp_wifi_scan_stop()-triggered WIFI_EVENT_SCAN_DONE will ever arrive
               to clear feb_wifi_scan_in_progress uniformly (the radio scan runs on the worker
               board) -- feb_cluster_wifi_scan_cancel() clears it directly and tells the worker
               to go idle. */
            if (!feb_cluster_wifi_scan_cancel()) {
#endif
            /* Don't clear feb_wifi_scan_in_progress directly here -- the radio scan this
               connection started may still be running, and a new connection's `command`
               could otherwise race a still-in-flight feb_wifi_scan_done_handler() write to
               wifi_scan_raw_records/wifi_scan_selected from a stale scan. esp_wifi_scan_stop()
               still fires WIFI_EVENT_SCAN_DONE for the aborted scan; feb_wifi_scan_done_cb() then
               finds no authenticated connection and clears the flag there, uniformly.
               Gated to the manual source only -- wardriving's Wi-Fi capture (if active) must
               keep running across this disconnect (docs/PROTOCOL.md: "continues across BLE
               disconnects"), so feb_wifi_scan_done_cb()'s WARDRIVING branch does not check
               connection state at all. */
            esp_err_t serr = esp_wifi_scan_stop();

            if (serr != ESP_OK && serr != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_scan_stop failed during disconnect cleanup: %s",
                         esp_err_to_name(serr));
            }
            ESP_LOGI(TAG, "wifi_scan was in progress at disconnect; stopping it "
                          "(pending results will be discarded)");
#if FEB_HAS_CLUSTER_WORKER
            }
#endif
        }

        if (feb_ble_scan_in_progress && feb_ble_scan_active_source == BLE_SCAN_SOURCE_MANUAL) {
            /* Same disconnect-safety shape as feb_wifi_scan_in_progress above, and gated to the
               manual source for the same reason (wardriving's BLE capture must survive this
               disconnect): stop the radio scan immediately, but don't clear
               feb_ble_scan_in_progress here -- the already-armed feb_ble_scan_done_co window-close
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

        if (feb_boot_mode == FEB_BOOT_MODE_PAIRING) {
            /* Any established connection's outcome consumes the one-shot pairing
               attempt (docs/PAIRING.md); go idle until the next physical reset. */
            feb_pairing_window_closed = true;
            ESP_LOGI(TAG, "pairing attempt consumed; staying idle until next reset");
            return 0;
        }

        /* feb_boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH: docs/PLAN.md step 6's three outcomes. */
        switch (reason) {
        case DISCONNECT_REASON_UNKNOWN_BOARD:
            feb_boot_mode = FEB_BOOT_MODE_PAIRING;
            esp_fill_random(feb_pairing_epoch, sizeof(feb_pairing_epoch));
            feb_pairing_window_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            feb_pairing_window_closed = false;
            reconnect_retries = 0;
            ESP_LOGI(TAG, "falling back to pairing window on next connection attempt "
                          "(board_id=%s unknown to flipper)", feb_board_id_buf);
            feb_start_scan();
            break;
        case DISCONNECT_REASON_AUTH_FAILED:
            schedule_runtime_auth_backoff();
            break;
        case DISCONNECT_REASON_NORMAL:
        default:
            /* Plain link loss / idle timeout / graceful close -- reconnect promptly,
               no rate-limit penalty (docs/PLAN.md step 6). wardriving's Wi-Fi source (if
               active) keeps scanning at its own configured feb_wardriving_wifi_interval_ms
               unthrottled during reconnect -- the steady-state default (5s, see
               wardriving_validate.h) already leaves enough radio headroom for the BLE
               reconnect scan; an earlier version of this path throttled the interval down to
               a fixed 400ms specifically during reconnect, which was backwards (it made Wi-Fi
               scan *more* often, not less, exactly when BLE needed the radio most) and has
               been removed. */
            feb_start_scan();
            break;
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
        /* G25: was a stack-local; single NimBLE host task (this whole switch runs on it, no
           reentrancy), so static drops 256 B off this frame. Its contents are only read
           synchronously below (os_mbuf_copydata fills it, then feb_reassembly_feed() copies
           whatever it needs into rx_reassembly's own buffer before returning) -- nothing
           retains a pointer into it past this case block, so losing the "fresh stack copy"
           guarantee costs nothing here. */
        static uint8_t buffer[FEB_RX_FRAGMENT_BUFFER_SIZE];
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
            if (feb_boot_mode == FEB_BOOT_MODE_PAIRING) {
                feb_pairing_envelope_t envelope;
                feb_cbor_status_t decode_status;

                if (feb_pairing_state != PAIRING_STATE_INIT_SENT) {
                    ESP_LOGW(TAG, "unexpected pairing record received in state %d", (int)feb_pairing_state);
                    feb_fail_pairing_ceremony(feb_connection_handle, FEB_PAIRING_ERR_FAILED);
                    break;
                }
                decode_status = feb_cbor_decode_pairing_envelope(record, record_len, &envelope);
                if (decode_status != FEB_CBOR_OK) {
                    ESP_LOGW(TAG, "pairing envelope decode failed: %d", (int)decode_status);
                    feb_fail_pairing_ceremony(feb_connection_handle, FEB_PAIRING_ERR_FAILED);
                    break;
                }
                if (envelope.version != 2 || envelope.board_id_len != feb_board_id_len ||
                    memcmp(envelope.board_id, feb_board_id_buf, feb_board_id_len) != 0) {
                    ESP_LOGW(TAG, "pairing envelope version/board_id mismatch");
                    feb_fail_pairing_ceremony(feb_connection_handle, FEB_PAIRING_ERR_FAILED);
                    break;
                }
                if (envelope.type_len == strlen(FEB_PAIR_REPLY_TYPE) &&
                    memcmp(envelope.type, FEB_PAIR_REPLY_TYPE, envelope.type_len) == 0) {
                    feb_handle_pair_reply(feb_connection_handle, &envelope);
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
                    feb_pairing_attempt_zeroize();
                    ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                } else {
                    ESP_LOGW(TAG, "expected pair_reply, got type=%.*s",
                             (int)envelope.type_len, envelope.type);
                    feb_fail_pairing_ceremony(feb_connection_handle, FEB_PAIRING_ERR_FAILED);
                }
                break;
            }

            /* feb_boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH */
            if (feb_runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED) {
                /* docs/PLAN.md step 7: first protected (AES-256-GCM) record handling. */
                feb_session_decrypted_record_t decrypted;
                feb_cbor_status_t decode_status;

                decode_status = feb_session_decrypt_record(feb_rt_session_key, record, record_len,
                                                           FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
                                                           rt_plaintext_buf, sizeof(rt_plaintext_buf),
                                                           &decrypted);
                if (decode_status != FEB_CBOR_OK) {
                    ESP_LOGW(TAG, "protected record decode/decrypt failed: %d; closing without reply",
                             (int)decode_status);
                    ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                if (decrypted.version != 2 ||
                    memcmp(decrypted.session_id, feb_rt_session_id, FEB_SESSION_ID_LEN) != 0 ||
                    decrypted.board_id_len != feb_board_id_len ||
                    memcmp(decrypted.board_id, feb_board_id_buf, feb_board_id_len) != 0 ||
                    decrypted.sequence != rt_rx_sequence ||
                    decrypted.sequence >= FEB_SESSION_SEQUENCE_MAX) {
                    ESP_LOGW(TAG, "protected record session/sequence mismatch; closing without reply");
                    ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                rt_rx_sequence++;

                if (decrypted.type_len == strlen("capability_query") &&
                    memcmp(decrypted.type, "capability_query", decrypted.type_len) == 0) {
                    feb_capability_query_payload_t query;

                    if (feb_cbor_decode_capability_query_payload(decrypted.plaintext, decrypted.plaintext_len,
                                                                  &query) != FEB_CBOR_OK) {
                        ESP_LOGW(TAG, "capability_query payload decode failed; closing without reply");
                        ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                        break;
                    }
                    feb_handle_capability_query(feb_connection_handle);
                } else if (decrypted.type_len == strlen("command") &&
                          memcmp(decrypted.type, "command", decrypted.type_len) == 0) {
                    feb_command_payload_t cmd;

                    if (feb_cbor_decode_command_payload(decrypted.plaintext, decrypted.plaintext_len,
                                                        &cmd) != FEB_CBOR_OK) {
                        ESP_LOGW(TAG, "command payload decode failed; closing without reply");
                        ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                        break;
                    }
                    feb_handle_command(feb_connection_handle, &cmd);
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
            if (feb_runtime_auth_state != RUNTIME_AUTH_STATE_HELLO_SENT) {
                ESP_LOGW(TAG, "unexpected runtime record received in state %d", (int)feb_runtime_auth_state);
                feb_fail_runtime_auth(feb_connection_handle);
                break;
            }
            {
                feb_unencrypted_record_t envelope;
                feb_cbor_status_t decode_status;

                decode_status = feb_cbor_decode_unencrypted(record, record_len, &envelope);
                if (decode_status == FEB_CBOR_OK && envelope.version == 2 &&
                    memcmp(envelope.session_id, feb_rt_session_id, FEB_SESSION_ID_LEN) == 0 &&
                    envelope.board_id_len == feb_board_id_len &&
                    memcmp(envelope.board_id, feb_board_id_buf, feb_board_id_len) == 0) {
                    if (envelope.type_len == strlen(FEB_HELLO_ACK_TYPE) &&
                        memcmp(envelope.type, FEB_HELLO_ACK_TYPE, envelope.type_len) == 0) {
                        feb_handle_hello_ack(feb_connection_handle, &envelope);
                    } else {
                        ESP_LOGW(TAG, "expected hello_ack, got type=%.*s",
                                 (int)envelope.type_len, envelope.type);
                        feb_fail_runtime_auth(feb_connection_handle);
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
                        perr_env.board_id_len == feb_board_id_len &&
                        memcmp(perr_env.board_id, feb_board_id_buf, feb_board_id_len) == 0 &&
                        perr_env.type_len == strlen("error") &&
                        memcmp(perr_env.type, "error", perr_env.type_len) == 0) {
                        feb_error_payload_t perr_payload;

                        if (feb_cbor_decode_error_payload(perr_env.payload_span, perr_env.payload_span_len,
                                                          &perr_payload) == FEB_CBOR_OK &&
                            perr_payload.code_len == strlen("unknown_board") &&
                            memcmp(perr_payload.code, "unknown_board", perr_payload.code_len) == 0) {
                            feb_handle_runtime_auth_unknown_board(feb_connection_handle);
                            break;
                        }
                    }
                }

                ESP_LOGW(TAG, "unrecognized record while awaiting hello_ack");
                feb_fail_runtime_auth(feb_connection_handle);
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

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        if (event->phy_updated.status != 0) {
            ESP_LOGW(TAG, "PHY update failed: status=%d (staying on 1M PHY)",
                     event->phy_updated.status);
        } else {
            ESP_LOGI(TAG, "PHY updated: tx=%u rx=%u",
                     (unsigned)event->phy_updated.tx_phy,
                     (unsigned)event->phy_updated.rx_phy);
        }
        return 0;

    default:
        return 0;
    }
}

void feb_reassembly_timeout_cb(struct ble_npl_event *ev)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    (void)ev;
    feb_status_led_tick();
    if (feb_reassembly_check_timeout(&rx_reassembly, now_ms) == FEB_FRAME_TIMEOUT) {
        ESP_LOGW(TAG, "rx reassembly timed out; discarding partial message");
    }
    if (feb_boot_mode == FEB_BOOT_MODE_RUNTIME_AUTH &&
        feb_runtime_auth_state == RUNTIME_AUTH_STATE_HELLO_SENT &&
        feb_hello_ack_start_ms != 0 &&
        (uint32_t)(now_ms - feb_hello_ack_start_ms) >= FEB_HELLO_ACK_TIMEOUT_MS &&
        feb_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "timed out waiting for hello_ack");
        feb_fail_runtime_auth(feb_connection_handle);
    }
    if (feb_pairing_state == PAIRING_STATE_INIT_SENT &&
        feb_pair_reply_wait_start_ms != 0 &&
        (uint32_t)(now_ms - feb_pair_reply_wait_start_ms) >= FEB_PAIR_REPLY_TIMEOUT_MS &&
        feb_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "timed out waiting for pair_reply");
        feb_pair_reply_wait_start_ms = 0;
        feb_fail_pairing_ceremony(feb_connection_handle, FEB_PAIRING_ERR_EXPIRED);
    }
    if (feb_connection_handle != BLE_HS_CONN_HANDLE_NONE &&
        feb_runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED &&
        (uint32_t)(now_ms - last_record_activity_ms) >= FEB_IDLE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "idle authenticated connection (%lu ms without a record); terminating",
                 (unsigned long)(now_ms - last_record_activity_ms));
        ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
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
    ble_npl_callout_reset(&feb_reassembly_timeout_co,
                          ble_npl_time_ms_to_ticks32(FEB_REASSEMBLY_CHECK_INTERVAL_MS));
}

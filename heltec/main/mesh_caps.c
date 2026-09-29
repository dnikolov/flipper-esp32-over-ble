#include "feb_app_internal.h"
#include "board_hooks.h"
#include "mesh_log.h"
#include "meshcore_table.h"
#include "meshtastic_table.h"

/* Shared by feb_handle_meshcore_command() and feb_handle_meshtastic_command() only -- both encode a
   single `result` payload synchronously within one BLE command callback (never concurrently:
   NimBLE's host task processes one incoming command at a time per connection), so giving each
   its own dedicated 512-byte static buffer would just be two copies of the same scratch
   space -- a real .dram0.bss overflow was hit on this classic-ESP32 target (docs/
   SESSION_MEMORY.md's meshtastic_scan entry) before this was consolidated. Matches this
   project's existing "consolidate duplicate static scratch" cost-efficiency backlog item in
   spirit, scoped narrowly to just these two LoRa-adjacent capabilities added together this
   session rather than every capability's own result_buf (gps/wifi_scan/etc keep their own --
   not touched here). */
static uint8_t lora_capability_result_buf[FEB_CBOR_MAX_PAYLOAD];

/* `mesh_log` backlog-drain state, mirroring feb_wardriving_tx_in_flight/
   feb_wardriving_pending_drain_count exactly (see feb_mesh_log_maybe_kick_send()/
   feb_mesh_log_send_next_batch() below) -- an entirely independent single-flight guard, since
   mesh_log's own drain is its own message type over the same shared protected-TX queue. */
static bool mesh_log_tx_in_flight;
/* bool, not size_t like feb_wardriving_pending_drain_count -- FEB_MESH_LOG_MAX_RECORDS_PER_BATCH
   is 1 (cbor_mesh_log.h), so this can only ever be 0 or 1; every static byte on this board is
   scarce (docs/BACKLOG.md BL23), so this is a deliberate width cut, not an oversight. */
static bool mesh_log_pending_drain;

/* docs/PLAN.md "MeshCore Scan Capability -- Heltec Board (Phase 1)": poll-only, single
   `status` action (no start/stop) -- the SX1276 RX task runs continuously from boot
   (lora_shared_radio_init(), called unconditionally in app_main() regardless of BLE connection
   state, same posture as location_init()), so there is nothing here to start or stop and no
   busy/exclusivity concept, matching feb_handle_gps_command()'s shape exactly. `arguments` is
   always an empty map -- this codebase has no existing single-operation capability that
   wire-encodes an explicit "action" string (see cbor_meshcore.h's top comment); `wardriving`
   is the only capability with one, and only because it genuinely has two operations
   (start/stop) to distinguish. */
void feb_handle_meshcore_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    size_t arg_count;
    feb_cbor_status_t status;
    feb_status_payload_t status_payload = {0};
    static feb_meshcore_status_result_payload_t result;
    static meshcore_table_entry_t entries[FEB_MESHCORE_MAX_NODES_PER_RESULT];
    size_t entry_count;
    uint64_t total_known;
    size_t i;
    size_t result_len;
    size_t payload_len;

    if (feb_cbor_decode_map_header(cmd->arguments_span, cmd->arguments_span_len, &arg_count, &status) == 0 ||
        arg_count != 0) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    memset(&result, 0, sizeof(result));
    entry_count = meshcore_table_snapshot(entries, FEB_MESHCORE_MAX_NODES_PER_RESULT, &total_known);

    for (i = 0; i < entry_count; i++) {
        feb_meshcore_node_t *node = &result.nodes[i];
        int32_t rssi_offset_signed;

        node->node_id = entries[i].node_id_hex;
        node->node_id_len = strlen(entries[i].node_id_hex);

        if (entries[i].has_name) {
            size_t name_len = strlen(entries[i].name);

            if (name_len > FEB_MESHCORE_NAME_MAX_LEN) {
                name_len = FEB_MESHCORE_NAME_MAX_LEN; /* codec bound; parser already caps
                                                          its own name buffer well above
                                                          this, see meshcore_proto.h */
            }
            node->name = entries[i].name;
            node->name_len = name_len;
            node->has_name = 1;
        }

        node->role = meshcore_role_to_string(entries[i].role);
        node->role_len = strlen(node->role);

        /* Clamp both directions before the unsigned +128 offset conversion: RadioLib's
           getRSSI() is a real hardware reading (trusted), but is not itself bounded to
           [-128, 127] dBm by contract, and an out-of-range value cast to the codec's
           unsigned rssi_offset field would either wrap (if negative) or be rejected outright
           by the encoder's own >255 check (if too high) -- clamping here keeps a single
           implausible reading from failing the whole status reply. */
        rssi_offset_signed = entries[i].rssi_dbm + 128;
        if (rssi_offset_signed < 0) {
            rssi_offset_signed = 0;
        } else if (rssi_offset_signed > 255) {
            rssi_offset_signed = 255;
        }
        node->rssi_offset = (uint64_t)rssi_offset_signed;

        node->last_seen_ms = entries[i].last_seen_ms;

        if (entries[i].has_location) {
            node->has_location = 1;
            node->lat_e7_offset = (uint64_t)((int64_t)entries[i].lat_e7 + 900000000LL);
            node->lon_e7_offset = (uint64_t)((int64_t)entries[i].lon_e7 + 1800000000LL);
        }
    }
    result.node_count = entry_count;
    result.total_known_nodes = total_known;

    result_len = feb_cbor_encode_meshcore_status_result_payload(lora_capability_result_buf,
                                                                 sizeof(lora_capability_result_buf), &result);
    if (result_len == 0) {
        if (!feb_send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    status_payload.request_id = cmd->request_id;
    status_payload.state = "ok";
    status_payload.state_len = strlen("ok");
    status_payload.result_span = lora_capability_result_buf;
    status_payload.result_span_len = result_len;
    status_payload.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf), &status_payload);
    if (payload_len == 0 ||
        !feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "meshcore_scan status query answered (request_id=%llu, %u/%llu node(s))",
             (unsigned long long)cmd->request_id, (unsigned)entry_count,
             (unsigned long long)total_known);
}

/* docs/PLAN.md "Meshtastic Scan Capability -- Heltec Board (Phase 1)": poll-only, single
   `status` action, same shape as feb_handle_meshcore_command() above -- see lora_shared_radio.h's
   radio-sharing decision for why this capability and `meshcore_scan` share one SX1276 via
   time-multiplexing rather than each getting a dedicated radio. */
void feb_handle_meshtastic_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    size_t arg_count;
    feb_cbor_status_t status;
    feb_status_payload_t status_payload = {0};
    static feb_meshtastic_status_result_payload_t result;
    static meshtastic_table_entry_t entries[FEB_MESHTASTIC_MAX_NODES_PER_RESULT];
    size_t entry_count;
    uint64_t total_known;
    size_t i;
    size_t result_len;
    size_t payload_len;

    if (feb_cbor_decode_map_header(cmd->arguments_span, cmd->arguments_span_len, &arg_count, &status) == 0 ||
        arg_count != 0) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    memset(&result, 0, sizeof(result));
    entry_count = meshtastic_table_snapshot(entries, FEB_MESHTASTIC_MAX_NODES_PER_RESULT, &total_known);

    for (i = 0; i < entry_count; i++) {
        feb_meshtastic_node_t *node = &result.nodes[i];
        int32_t rssi_offset_signed;

        node->node_id = entries[i].node_id_hex;
        node->node_id_len = strlen(entries[i].node_id_hex);

        if (entries[i].has_name) {
            size_t name_len = strlen(entries[i].name);

            if (name_len > FEB_MESHTASTIC_NAME_MAX_LEN) {
                name_len = FEB_MESHTASTIC_NAME_MAX_LEN; /* codec bound; parser already caps
                                                            its own name buffer well above
                                                            this, see meshtastic_proto.h */
            }
            node->name = entries[i].name;
            node->name_len = name_len;
            node->has_name = 1;
        }

        /* Clamp both directions before the unsigned +128 offset conversion -- see
           feb_handle_meshcore_command()'s identical comment on why. */
        rssi_offset_signed = entries[i].rssi_dbm + 128;
        if (rssi_offset_signed < 0) {
            rssi_offset_signed = 0;
        } else if (rssi_offset_signed > 255) {
            rssi_offset_signed = 255;
        }
        node->rssi_offset = (uint64_t)rssi_offset_signed;

        node->last_seen_ms = entries[i].last_seen_ms;
    }
    result.node_count = entry_count;
    result.total_known_nodes = total_known;

    result_len = feb_cbor_encode_meshtastic_status_result_payload(lora_capability_result_buf,
                                                                   sizeof(lora_capability_result_buf), &result);
    if (result_len == 0) {
        if (!feb_send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    status_payload.request_id = cmd->request_id;
    status_payload.state = "ok";
    status_payload.state_len = strlen("ok");
    status_payload.result_span = lora_capability_result_buf;
    status_payload.result_span_len = result_len;
    status_payload.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf), &status_payload);
    if (payload_len == 0 ||
        !feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "meshtastic_scan status query answered (request_id=%llu, %u/%llu node(s))",
             (unsigned long long)cmd->request_id, (unsigned)entry_count,
             (unsigned long long)total_known);
}

/* `mesh_log` backlog drain, mirroring feb_wardriving_maybe_kick_send()/feb_wardriving_send_next_batch()
   above but pointed at a second, independent flash log (mesh_log.c) and a batch size of
   exactly 1 (FEB_MESH_LOG_MAX_RECORDS_PER_BATCH, cbor_mesh_log.h -- this board's tight DRAM
   budget forces this, see that constant's comment) rather than wardriving's dynamic
   pack-as-many-as-fit batching. Only ever kicked from TX_DONE_RUNTIME_AUTHENTICATED (session
   establishment) -- unlike wardriving, never re-kicked from a periodic scan-cycle callback,
   since mesh_log's own capture happens on a different FreeRTOS task (the LoRa RX task) with
   no existing safe path to signal the NimBLE host task promptly; a node captured while a
   session is already open and fully drained waits for the next reconnect (docs/PROTOCOL.md's
   `mesh_log` section documents this explicitly as an accepted scope cut, not an oversight). */
void feb_mesh_log_maybe_kick_send(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || feb_runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        return;
    }
    if (mesh_log_tx_in_flight) {
        return;
    }
    if (mesh_log_pending_count() == 0) {
        return;
    }
    mesh_log_tx_in_flight = true;
    feb_status_led_set(FEB_STATUS_LED_FLUSHING);
    feb_mesh_log_send_next_batch(conn_handle);
}

/* Builds and sends one mesh_log status(state="mesh_data") record, carrying at most
   FEB_MESH_LOG_MAX_RECORDS_PER_BATCH (1) record -- no dynamic pack-until-it-doesn't-fit trial
   loop is needed (unlike feb_wardriving_send_next_batch()) since a single <mesh-log-record> is
   provably far smaller than FEB_CBOR_MAX_PAYLOAD (see cbor_mesh_log.h's sizing comment).
   `result`/`result_buf` reuse lora_capability_result_buf (shared with
   feb_handle_meshcore_command()/feb_handle_meshtastic_command()) rather than a dedicated static
   buffer -- safe because this board's whole capability set runs one operation at a time on a
   single NimBLE host task with only one BLE write ever in flight (see that buffer's own
   comment), and this board's DRAM budget has no room for a fourth copy of the same 512-byte
   scratch space. */
void feb_mesh_log_send_next_batch(uint16_t conn_handle)
{
    /* Plain locals, not `static` -- this board's DRAM budget has no room to spare (see
       mesh_log.h's top comment), and none of these need to survive past this one synchronous
       call: `result`/`status_payload` are tiny (FEB_MESH_LOG_MAX_RECORDS_PER_BATCH == 1), and
       `peek_scratch` is mesh_log_peek_pending()'s caller-supplied scratch buffer (its own
       comment explains why a stack-local here, not a second static buffer inside mesh_log.c,
       is the safe choice). This mirrors feb_handle_wardriving_command()'s own existing
       non-static `feb_wardriving_command_payload_t payload` precedent in this same file --
       moderate-sized plain locals on the NimBLE host task's stack are an established,
       already-safe pattern here, not a new risk. */
    feb_mesh_log_status_result_payload_t result = {0};
    feb_status_payload_t status_payload = {0};
    uint8_t peek_scratch[ML_RECORD_MAX_PAYLOAD];
    size_t peeked_count;
    size_t result_len;
    size_t payload_len;
    size_t pending_now;

    if (mesh_log_pending_drain) {
        mesh_log_mark_drained(1);
        mesh_log_pending_drain = false;
    }

    peeked_count = mesh_log_peek_pending(result.records, FEB_MESH_LOG_MAX_RECORDS_PER_BATCH,
                                        peek_scratch, sizeof(peek_scratch));
    if (peeked_count == 0) {
        mesh_log_tx_in_flight = false;
        feb_status_led_set(FEB_STATUS_LED_CONNECTED);
        return;
    }

    pending_now = mesh_log_pending_count();
    result.record_count = peeked_count;
    result.backlog_remaining = (pending_now >= peeked_count) ? (pending_now - peeked_count) : 0;

    result_len = feb_cbor_encode_mesh_log_status_result_payload(lora_capability_result_buf,
                                                                sizeof(lora_capability_result_buf), &result);

    status_payload.request_id = 0;
    status_payload.state = "mesh_data";
    status_payload.state_len = strlen("mesh_data");
    status_payload.result_span = lora_capability_result_buf;
    status_payload.result_span_len = result_len;
    status_payload.has_result = 1;

    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf), &status_payload);
    if (result_len == 0) {
        ESP_LOGE(TAG, "mesh_log status(mesh_data) result encode failed (records=%u)",
                 (unsigned)result.record_count);
    } else if (payload_len == 0) {
        ESP_LOGE(TAG, "mesh_log status(mesh_data) wrapper encode failed (result_len=%u)",
                 (unsigned)result_len);
    }
    if (payload_len == 0) {
        ESP_LOGE(TAG, "failed to build mesh_log status(mesh_data) record");
        mesh_log_tx_in_flight = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    if (!feb_queue_and_send_protected(conn_handle, "status", strlen("status"),
                                  feb_pairing_payload_encode_buf, payload_len,
                                  TX_DONE_BOARD_EXT)) {
        ESP_LOGE(TAG, "failed to queue mesh_log status(mesh_data) record (payload_len=%u)",
                 (unsigned)payload_len);
        mesh_log_tx_in_flight = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    mesh_log_pending_drain = (peeked_count > 0);
    ESP_LOGI(TAG, "sending mesh_log status(mesh_data) (%u record(s), backlog_remaining=%llu)",
             (unsigned)result.record_count, (unsigned long long)result.backlog_remaining);
}

/* feb_app_hooks_t.on_connect: per-connection only, mirroring feb_wardriving_tx_in_flight --
   mesh_log's own capture keeps running regardless of connection state. */
void feb_mesh_log_on_connect(void)
{
    mesh_log_tx_in_flight = false;
    mesh_log_pending_drain = false;
}

/* feb_app_hooks_t.on_disconnect. */
void feb_mesh_log_on_disconnect(void)
{
    mesh_log_tx_in_flight = false;
}

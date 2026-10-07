#include "feb_app_internal.h"

#if FEB_HAS_AGPS

#include "feb_agps_blob.h"
#include "feb_agps_casic.h"
#include "feb_agps_cmd.h"
#include "location_agps.h"

/* docs/AGPS.md "Build contract" / docs/PROTOCOL.md "`agps` command and status payloads".
   The Flipper streams one <=4096 B window blob as sequential <=256 B chunks; the final chunk
   is validated (every BA CE frame boundary + checksum) and then fed to the GPS by a short-lived
   task, so the NimBLE host task never blocks for the ~3-5 s the feed takes. The task only
   touches the blob (immutable while RUNNING), the result struct, and the GPS UART (through
   location_agps_*, which serializes against location.c's NMEA task); every BLE send happens on
   the host task, from feb_agps_poll() on the reassembly-check tick (no extra callout: the
   8-callout NimBLE cap, docs/SOURCE_SPLIT.md invariant 4). Nothing here ever sends a $PCAS
   command: the only bytes written to the module are the AID-INI built by feb_agps_build_ini()
   and frames from the validated blob, which feb_casic_validate_frames() restricts to class 0x08. */

#define AGPS_FEED_TASK_STACK 4096u
#define AGPS_FEED_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define AGPS_UART_LOCK_TIMEOUT_MS 1000u

enum { AGPS_IDLE = 0, AGPS_RUNNING, AGPS_DONE };

/* ~4.1 KB of DRAM, C5 only. Written by the host task only, except that the feed task reads it
   while agps_state == AGPS_RUNNING (host task then rejects every chunk as busy). */
static feb_agps_blob_t agps_blob;
static volatile uint8_t agps_state;
/* Set by the host task before the task starts; read by the feed task. */
static uint64_t agps_utc_s;
static uint32_t agps_utc_rx_ms;
static feb_agps_position_t agps_position;
static uint64_t agps_request_id;
/* Written by the feed task before it publishes AGPS_DONE. */
static feb_agps_result_t agps_result;
/* Host task only: the link that started the feed went away, drop the completion. */
static bool agps_orphaned;

static uint32_t agps_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void agps_io_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    location_agps_write(data, len);
}

static size_t agps_io_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    return location_agps_read(buf, cap, timeout_ms);
}

static uint32_t agps_io_now(void *ctx)
{
    (void)ctx;
    return agps_now_ms();
}

static void agps_io_frame_log(void *ctx, const uint8_t *frame, int outcome, uint32_t waited_ms)
{
    static const char *const names[] = {"TIMEOUT", "ACK", "NACK"};

    (void)ctx;
    if (frame[4] == FEB_CASIC_CLASS_MSG && frame[5] == 0x07 && frame[2] == 72 && frame[3] == 0) {
        ESP_LOGI(TAG, "agps 08/07 svid=%u %s %ums", (unsigned)frame[6 + 68], names[outcome], (unsigned)waited_ms);
    } else {
        ESP_LOGI(TAG, "agps %02x/%02x %s %ums", (unsigned)frame[4], (unsigned)frame[5], names[outcome],
                 (unsigned)waited_ms);
    }
}

static void agps_feed_task(void *arg)
{
    static const feb_agps_io_t io = {NULL, agps_io_write, agps_io_read, agps_io_now, agps_io_frame_log};
    uint8_t ini[FEB_AGPS_INI_LEN];
    feb_agps_result_t result = {0};

    (void)arg;
    if (location_agps_begin(AGPS_UART_LOCK_TIMEOUT_MS)) {
        feb_agps_build_ini(ini, agps_utc_s * 1000u + (uint32_t)(agps_now_ms() - agps_utc_rx_ms),
                           &agps_position);
        feb_agps_feed(&io, ini, agps_blob.data, agps_blob.total, &result);
        location_agps_end();
    } else {
        ESP_LOGW(TAG, "agps: GPS UART busy, nothing sent");
    }
    agps_result = result;
    __atomic_store_n(&agps_state, AGPS_DONE, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

static void agps_reply_error(uint16_t conn_handle, const char *code, uint64_t request_id)
{
    if (!feb_send_protected_error(conn_handle, code, strlen(code), 1, request_id)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static bool agps_send_status(uint16_t conn_handle, uint64_t request_id, const char *state,
                             const feb_agps_result_t *result)
{
    uint8_t result_buf[40];
    feb_status_payload_t status_payload = {0};
    size_t payload_len;

    status_payload.request_id = request_id;
    status_payload.state = state;
    status_payload.state_len = strlen(state);
    if (result != NULL) {
        size_t result_len = feb_agps_encode_result(result_buf, sizeof(result_buf), result);

        if (result_len == 0) {
            return false;
        }
        status_payload.result_span = result_buf;
        status_payload.result_span_len = result_len;
        status_payload.has_result = 1;
    }
    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf), &status_payload);
    return payload_len != 0 &&
           feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len);
}

void feb_handle_agps_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    feb_agps_load_args_t args;
    feb_agps_blob_status_t blob_status;
    feb_casic_status_t casic_status;
    uint32_t now_ms = agps_now_ms();
    uint32_t frame_count = 0;

    if (feb_agps_decode_load_args(cmd->arguments_span, cmd->arguments_span_len, &args) != FEB_CBOR_OK) {
        if (agps_state == AGPS_IDLE) {
            feb_agps_blob_reset(&agps_blob);
        }
        agps_reply_error(conn_handle, "invalid_command", cmd->request_id);
        return;
    }
    if (agps_state != AGPS_IDLE) {
        agps_reply_error(conn_handle, "busy", cmd->request_id);
        return;
    }

    blob_status = feb_agps_blob_chunk(&agps_blob, args.total, args.off, args.data, args.data_len, now_ms);
    if (blob_status == FEB_AGPS_BLOB_ERR_COMPLETE) {
        feb_agps_blob_reset(&agps_blob);
    }
    if (blob_status != FEB_AGPS_BLOB_MORE && blob_status != FEB_AGPS_BLOB_COMPLETE) {
        ESP_LOGW(TAG, "agps chunk rejected (status=%d, off=%u, total=%u)", (int)blob_status, (unsigned)args.off,
                 (unsigned)args.total);
        agps_reply_error(conn_handle, "invalid_command", cmd->request_id);
        return;
    }

    if (blob_status == FEB_AGPS_BLOB_COMPLETE) {
        casic_status = feb_casic_validate_frames(agps_blob.data, agps_blob.total, &frame_count);
        if (casic_status != FEB_CASIC_OK) {
            ESP_LOGW(TAG, "agps blob rejected before GPS: casic status=%d", (int)casic_status);
            feb_agps_blob_reset(&agps_blob);
            agps_reply_error(conn_handle, "invalid_command", cmd->request_id);
            return;
        }
        if (!location_agps_available()) {
            feb_agps_blob_reset(&agps_blob);
            agps_reply_error(conn_handle, "internal_error", cmd->request_id);
            return;
        }
        agps_utc_s = args.utc;
        agps_utc_rx_ms = now_ms;
        agps_position = args.position;
        agps_request_id = cmd->request_id;
        agps_orphaned = false;
        memset(&agps_result, 0, sizeof(agps_result));
        __atomic_store_n(&agps_state, AGPS_RUNNING, __ATOMIC_RELEASE);
        if (xTaskCreate(agps_feed_task, "agps_feed", AGPS_FEED_TASK_STACK, NULL, AGPS_FEED_TASK_PRIORITY, NULL) !=
            pdPASS) {
            __atomic_store_n(&agps_state, AGPS_IDLE, __ATOMIC_RELEASE);
            feb_agps_blob_reset(&agps_blob);
            agps_reply_error(conn_handle, "internal_error", cmd->request_id);
            return;
        }
        ESP_LOGI(TAG, "agps: %u-byte blob, %u frames validated, feed started (request_id=%llu)",
                 (unsigned)agps_blob.total, (unsigned)frame_count, (unsigned long long)cmd->request_id);
    }

    if (!agps_send_status(conn_handle, cmd->request_id, "partial", NULL)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void feb_agps_poll(void)
{
    if (feb_agps_blob_expire(&agps_blob, agps_now_ms())) {
        ESP_LOGW(TAG, "agps: partial blob timed out; discarded");
    }
    if (__atomic_load_n(&agps_state, __ATOMIC_ACQUIRE) != AGPS_DONE) {
        return;
    }
    if (!agps_orphaned && feb_connection_handle != BLE_HS_CONN_HANDLE_NONE &&
        feb_runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED) {
        ESP_LOGI(TAG, "agps: feed done sent=%u acked=%u nacked=%u timeout=%u late=%u in %ums",
                 (unsigned)agps_result.sent, (unsigned)agps_result.acked, (unsigned)agps_result.nacked,
                 (unsigned)agps_result.timeout, (unsigned)agps_result.late, (unsigned)agps_result.elapsed_ms);
        if (!agps_send_status(feb_connection_handle, agps_request_id, "complete", &agps_result)) {
            ble_gap_terminate(feb_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    feb_agps_blob_reset(&agps_blob);
    agps_orphaned = false;
    __atomic_store_n(&agps_state, AGPS_IDLE, __ATOMIC_RELEASE);
}

void feb_agps_on_disconnect(void)
{
    if (agps_state == AGPS_IDLE) {
        feb_agps_blob_reset(&agps_blob);
    } else {
        /* The feed task owns the blob until it finishes; drop only the completion. */
        agps_orphaned = true;
    }
}

#endif /* FEB_HAS_AGPS */

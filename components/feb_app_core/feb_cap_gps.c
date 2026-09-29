#include "feb_app_internal.h"

/* docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and real wardriving-record
   timestamps": `gps` is a poll-only, single-shot status query -- no scan-duration lifecycle,
   no busy/exclusivity concept (the UART read is a passive background task independent of the
   Wi-Fi/BLE radio), no request_id dedup cache (matches wifi_scan/ble_scan). `result` is
   present only when state == "fix", per docs/PROTOCOL.md's `gps` status table. */
void feb_handle_gps_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    size_t arg_count;
    feb_cbor_status_t status;
    feb_location_t fix;
    feb_location_state_t loc_state;
    feb_status_payload_t status_payload = {0};
    /* map(1) + lat_e7_offset key(1+13)+value(up to 5, values to ~1.8e9) +
       lon_e7_offset key(1+13)+value(5, up to ~3.6e9) + fix_quality key(1+11)+value(~1-2) +
       satellites key(1+10)+value(~1-2) + hdop_e1 key(1+7)+value(~1-2) + utc_timestamp_s
       key(1+15)+value(5, real Unix timestamps need the full 4-byte uint32 form) +
       altitude_dm_offset key(1+18)+value(5, offset keeps this > 65535) + speed_e1_kmh
       key(1+12)+value(~1-3) == ~133 bytes worst case with real (large) field values -- found
       (2026-09-21, hardware-verified) to exceed the previous 128-byte sizing in practice, not
       just in a theoretical worst case: real lat/lon/timestamp/altitude values are large
       enough to need their full uint encoding on every genuine fix, so
       feb_cbor_encode_gps_result_payload() failed on every single `gps` status reply once a
       real fix existed, silently downgrading every reply to an `internal_error` (result_len
       == 0 below) -- the GPS screen never actually decoded a bad payload, it just never
       received a `status` reply at all. Sized with real margin now, not shaved to the byte,
       matching this file's `cmd_payload_buf`/`FEB_CMD_PAYLOAD_MAX_LEN` precedent for the same
       failure class on the Flipper side. */
    uint8_t result_buf[192];
    size_t payload_len;

    if (feb_cbor_decode_map_header(cmd->arguments_span, cmd->arguments_span_len, &arg_count, &status) == 0 ||
        arg_count != 0) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"),
                                  1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    loc_state = location_get_fix(&fix);
    status_payload.request_id = cmd->request_id;
    switch (loc_state) {
    case FEB_LOCATION_FIX: status_payload.state = "fix"; break;
    case FEB_LOCATION_ACQUIRING: status_payload.state = "acquiring"; break;
    case FEB_LOCATION_NO_SIGNAL:
    default: status_payload.state = "no_signal"; break;
    }
    status_payload.state_len = strlen(status_payload.state);

    if (loc_state == FEB_LOCATION_FIX) {
        feb_gps_result_payload_t result = {0};
        size_t result_len;

        result.lat_e7_offset = (uint64_t)((int64_t)fix.lat_e7 + 900000000LL);
        result.lon_e7_offset = (uint64_t)((int64_t)fix.lon_e7 + 1800000000LL);
        result.fix_quality = fix.fix_quality;
        result.satellites = fix.satellites;
        result.hdop_e1 = fix.hdop_e1;
        result.utc_timestamp_s = fix.utc_timestamp_s;
        result.altitude_dm_offset = (uint64_t)((int64_t)fix.altitude_dm + FEB_GPS_ALTITUDE_DM_OFFSET);
        result.speed_e1_kmh = fix.speed_e1_kmh;

        result_len = feb_cbor_encode_gps_result_payload(result_buf, sizeof(result_buf), &result);
        if (result_len == 0) {
            if (!feb_send_protected_error(conn_handle, "internal_error", strlen("internal_error"),
                                      1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
        status_payload.result_span = result_buf;
        status_payload.result_span_len = result_len;
        status_payload.has_result = 1;
    }

    payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                 sizeof(feb_pairing_payload_encode_buf), &status_payload);
    if (payload_len == 0 ||
        !feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len)) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "gps status query answered (request_id=%llu, state=%s)",
             (unsigned long long)cmd->request_id, status_payload.state);
}

#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

/* ---- `wardriving` capability payloads ---- */

size_t feb_cbor_encode_wardriving_command_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wardriving_command_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->action == NULL) {
        return 0;
    }
    if(payload->has_sources && payload->source_count > FEB_WARDRIVING_MAX_SOURCES) {
        return 0;
    }
    size_t count = 1u;
    if(payload->has_sources) count++;
    if(payload->has_wifi_interval_ms) count++;
    if(payload->has_ble_params) count += 2u;

    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "action", sizeof("action") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->action, payload->action_len);
    if(n == 0) return 0;
    pos += n;

    if(payload->has_sources) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "sources", sizeof("sources") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->source_count);
        if(n == 0) return 0;
        pos += n;
        for(size_t i = 0; i < payload->source_count; i++) {
            n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->sources[i], payload->source_lens[i]);
            if(n == 0) return 0;
            pos += n;
        }
    }
    if(payload->has_wifi_interval_ms) {
        n = feb_cbor_encode_text(
            out + pos, out_cap - pos, "wifi_interval_ms", sizeof("wifi_interval_ms") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->wifi_interval_ms);
        if(n == 0) return 0;
        pos += n;
    }
    if(payload->has_ble_params) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "ble_window_ms", sizeof("ble_window_ms") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->ble_window_ms);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(
            out + pos, out_cap - pos, "ble_interval_ms", sizeof("ble_interval_ms") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->ble_interval_ms);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_wardriving_command_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_command_payload_t* payload) {
    if(in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    memset(payload, 0, sizeof(*payload));
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 1) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 5) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[5];
    size_t seen_lens[5];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "action", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->action, &payload->action_len, FEB_WARDRIVING_ACTION_MAX_LEN, &status);
    if(n == 0) return status;
    pos += n;

    if(count == 1) {
        return FEB_CBOR_OK;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "sources", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    size_t source_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &source_count, &status);
    if(n == 0) return status;
    if(source_count > FEB_WARDRIVING_MAX_SOURCES) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;
    for(size_t i = 0; i < source_count; i++) {
        n = feb_cbor_decode_text(
            in + pos, in_len - pos, &payload->sources[i], &payload->source_lens[i], FEB_WARDRIVING_SOURCE_MAX_LEN, &status);
        if(n == 0) return status;
        pos += n;
    }
    payload->source_count = source_count;
    payload->has_sources = 1;

    size_t remaining = count - 2;
    if(remaining > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    if(remaining == 1 || remaining == 3) {
        n = feb_cbor_i_decode_expected_key(
            in + pos, in_len - pos, "wifi_interval_ms", seen_ptrs, seen_lens, 2, &status);
        if(n == 0) return status;
        pos += n;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->wifi_interval_ms, &status);
        if(n == 0) return status;
        pos += n;
        payload->has_wifi_interval_ms = 1;
    }
    if(remaining == 2 || remaining == 3) {
        size_t idx = (remaining == 3) ? 3 : 2;
        n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "ble_window_ms", seen_ptrs, seen_lens, idx, &status);
        if(n == 0) return status;
        pos += n;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->ble_window_ms, &status);
        if(n == 0) return status;
        pos += n;
        n = feb_cbor_i_decode_expected_key(
            in + pos, in_len - pos, "ble_interval_ms", seen_ptrs, seen_lens, idx + 1, &status);
        if(n == 0) return status;
        pos += n;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->ble_interval_ms, &status);
        if(n == 0) return status;
        pos += n;
        payload->has_ble_params = 1;
    }

    if(pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    return FEB_CBOR_OK;
}

static size_t feb_cbor_encode_wardriving_wifi_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wardriving_wifi_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->auth == NULL) {
        return 0;
    }
    if(payload->ssid == NULL && payload->ssid_len > 0) {
        return 0;
    }
    if(payload->ssid_len > FEB_WARDRIVING_WIFI_SSID_MAX_LEN || payload->rssi_offset > 255u) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 5);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ssid", sizeof("ssid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, payload->ssid, payload->ssid_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "bssid", sizeof("bssid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, payload->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->rssi_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "channel", sizeof("channel") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->channel);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "auth", sizeof("auth") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->auth, payload->auth_len);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

static size_t feb_cbor_decode_wardriving_wifi_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_wifi_payload_t* payload,
    feb_cbor_status_t* status) {
    if(in == NULL || payload == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(payload, 0, sizeof(*payload));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 5) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 5) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[5];
    size_t seen_lens[5];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "ssid", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_bytes(
        in + pos, in_len - pos, &payload->ssid, &payload->ssid_len, FEB_WARDRIVING_WIFI_SSID_MAX_LEN, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "bssid", seen_ptrs, seen_lens, 1, status);
    if(n == 0) return 0;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_WIFI_SCAN_BSSID_LEN, status);
        if(n == 0) return 0;
        if(len != FEB_WIFI_SCAN_BSSID_LEN) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        memcpy(payload->bssid, data, FEB_WIFI_SCAN_BSSID_LEN);
        pos += n;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "rssi_offset", seen_ptrs, seen_lens, 2, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->rssi_offset, status);
    if(n == 0) return 0;
    if(payload->rssi_offset > 255u) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "channel", seen_ptrs, seen_lens, 3, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->channel, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "auth", seen_ptrs, seen_lens, 4, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->auth, &payload->auth_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

static size_t feb_cbor_encode_wardriving_ble_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wardriving_ble_payload_t* payload) {
    if(out == NULL || payload == NULL) {
        return 0;
    }
    if(payload->has_name && payload->name == NULL) {
        return 0;
    }
    if(payload->has_name && payload->name_len > FEB_WARDRIVING_BLE_NAME_MAX_LEN) {
        return 0;
    }
    if(payload->rssi_offset > 255u) {
        return 0;
    }
    size_t count = payload->has_name ? 3u : 2u;
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "address", sizeof("address") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, payload->address, FEB_WARDRIVING_BLE_ADDRESS_LEN);
    if(n == 0) return 0;
    pos += n;
    if(payload->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", sizeof("name") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->name, payload->name_len);
        if(n == 0) return 0;
        pos += n;
    }
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->rssi_offset);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

static size_t feb_cbor_decode_wardriving_ble_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_ble_payload_t* payload,
    feb_cbor_status_t* status) {
    if(in == NULL || payload == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(payload, 0, sizeof(*payload));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 2) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 3) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "address", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_WARDRIVING_BLE_ADDRESS_LEN, status);
        if(n == 0) return 0;
        if(len != FEB_WARDRIVING_BLE_ADDRESS_LEN) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        memcpy(payload->address, data, FEB_WARDRIVING_BLE_ADDRESS_LEN);
        pos += n;
    }

    size_t next_index = 1;
    if(count == 3) {
        n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "name", seen_ptrs, seen_lens, next_index, status);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_decode_text(
            in + pos, in_len - pos, &payload->name, &payload->name_len, FEB_WARDRIVING_BLE_NAME_MAX_LEN, status);
        if(n == 0) return 0;
        pos += n;
        payload->has_name = 1;
        next_index++;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "rssi_offset", seen_ptrs, seen_lens, next_index, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->rssi_offset, status);
    if(n == 0) return 0;
    if(payload->rssi_offset > 255u) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wardriving_record(uint8_t* out, size_t out_cap, const feb_wardriving_record_t* record) {
    if(out == NULL || record == NULL || record->source == NULL) {
        return 0;
    }
    if(record->payload_kind != FEB_WARDRIVING_PAYLOAD_WIFI &&
       record->payload_kind != FEB_WARDRIVING_PAYLOAD_BLE) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 6);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "timestamp_ms", sizeof("timestamp_ms") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->timestamp_ms);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "utc_timestamp_s", sizeof("utc_timestamp_s") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->utc_timestamp_s);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lat_e7_offset", sizeof("lat_e7_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->lat_e7_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lon_e7_offset", sizeof("lon_e7_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->lon_e7_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "source", sizeof("source") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->source, record->source_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", sizeof("payload") - 1);
    if(n == 0) return 0;
    pos += n;
    if(record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
        n = feb_cbor_encode_wardriving_wifi_payload(out + pos, out_cap - pos, &record->payload.wifi);
    } else {
        n = feb_cbor_encode_wardriving_ble_payload(out + pos, out_cap - pos, &record->payload.ble);
    }
    if(n == 0) return 0;
    pos += n;
    return pos;
}

size_t feb_cbor_decode_wardriving_record(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_record_t* record,
    feb_cbor_status_t* status) {
    if(in == NULL || record == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(record, 0, sizeof(*record));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 6) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 6) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[6];
    size_t seen_lens[6];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "timestamp_ms", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->timestamp_ms, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "utc_timestamp_s", seen_ptrs, seen_lens, 1, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->utc_timestamp_s, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "lat_e7_offset", seen_ptrs, seen_lens, 2, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->lat_e7_offset, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "lon_e7_offset", seen_ptrs, seen_lens, 3, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->lon_e7_offset, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "source", seen_ptrs, seen_lens, 4, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &record->source, &record->source_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "payload", seen_ptrs, seen_lens, 5, status);
    if(n == 0) return 0;
    pos += n;

    if(feb_cbor_i_text_matches(record->source, record->source_len, "wifi")) {
        record->payload_kind = FEB_WARDRIVING_PAYLOAD_WIFI;
        n = feb_cbor_decode_wardriving_wifi_payload(in + pos, in_len - pos, &record->payload.wifi, status);
    } else if(feb_cbor_i_text_matches(record->source, record->source_len, "ble")) {
        record->payload_kind = FEB_WARDRIVING_PAYLOAD_BLE;
        n = feb_cbor_decode_wardriving_ble_payload(in + pos, in_len - pos, &record->payload.ble, status);
    } else {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    if(n == 0) {
        return 0;
    }
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wardriving_status_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wardriving_status_result_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->record_count > FEB_WARDRIVING_MAX_RECORDS_PER_BATCH) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 2);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "records", sizeof("records") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->record_count);
    if(n == 0) return 0;
    pos += n;
    for(size_t i = 0; i < payload->record_count; i++) {
        n = feb_cbor_encode_wardriving_record(out + pos, out_cap - pos, &payload->records[i]);
        if(n == 0) return 0;
        pos += n;
    }
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "backlog_remaining", sizeof("backlog_remaining") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->backlog_remaining);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t feb_cbor_decode_wardriving_status_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_status_result_payload_t* payload) {
    if(in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    memset(payload, 0, sizeof(*payload));
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 2) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 2) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[2];
    size_t seen_lens[2];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "records", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;

    size_t array_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
    if(n == 0) return status;
    if(array_count > FEB_WARDRIVING_MAX_RECORDS_PER_BATCH) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;
    for(size_t i = 0; i < array_count; i++) {
        size_t item_len =
            feb_cbor_decode_wardriving_record(in + pos, in_len - pos, &payload->records[i], &status);
        if(item_len == 0) {
            return status;
        }
        pos += item_len;
    }
    payload->record_count = array_count;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "backlog_remaining", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->backlog_remaining, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

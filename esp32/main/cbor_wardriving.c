#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

size_t feb_cbor_encode_wardriving_command_payload(uint8_t *out, size_t out_cap, const feb_wardriving_command_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t count = 1;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->has_sources) {
        if (payload->source_count > FEB_WARDRIVING_MAX_SOURCES) {
            return 0;
        }
        count++;
    }
    if (payload->has_wifi_interval_ms) count++;
    if (payload->has_ble_params) count += 2;

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, count);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "action", FEB_CBOR_I_KLEN("action"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->action, payload->action_len);
    if (n == 0) return 0;
    pos += n;

    if (payload->has_sources) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "sources", FEB_CBOR_I_KLEN("sources"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->source_count);
        if (n == 0) return 0;
        pos += n;
        for (i = 0; i < payload->source_count; i++) {
            n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->sources[i], payload->source_lens[i]);
            if (n == 0) return 0;
            pos += n;
        }
    }

    if (payload->has_wifi_interval_ms) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "wifi_interval_ms", FEB_CBOR_I_KLEN("wifi_interval_ms"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->wifi_interval_ms);
        if (n == 0) return 0;
        pos += n;
    }

    if (payload->has_ble_params) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "ble_window_ms", FEB_CBOR_I_KLEN("ble_window_ms"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->ble_window_ms);
        if (n == 0) return 0;
        pos += n;

        n = feb_cbor_encode_text(out + pos, out_cap - pos, "ble_interval_ms", FEB_CBOR_I_KLEN("ble_interval_ms"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->ble_interval_ms);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
}

feb_cbor_status_t feb_cbor_decode_wardriving_command_payload(const uint8_t *in, size_t in_len, feb_wardriving_command_payload_t *payload)
{
    static const char *const names[5] = {"action", "sources", "wifi_interval_ms", "ble_window_ms", "ble_interval_ms"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[5] = {0, 0, 0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->has_sources = 0;
    payload->source_count = 0;
    payload->has_wifi_interval_ms = 0;
    payload->has_ble_params = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 5) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    if (count == 0) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    pos = consumed;

    for (i = 0; i < count; i++) {
        const char *key_data;
        size_t key_len;
        size_t key_consumed;
        int found;
        size_t j;
        size_t field_index;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);
        if (key_consumed == 0) {
            return status;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 5; j++) {
            if (key_len == strlen(names[j]) && memcmp(key_data, names[j], key_len) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        field_index = (size_t)found;
        if (seen[field_index]) {
            return FEB_CBOR_ERR_DUPLICATE_KEY;
        }
        if (field_index < next_min) {
            return FEB_CBOR_ERR_OUT_OF_ORDER;
        }

        switch (field_index) {
        case 0: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_WARDRIVING_ACTION_MAX_LEN, &status);

            if (n == 0) return status;
            payload->action = data;
            payload->action_len = len;
            pos += n;
            break;
        }
        case 1: {
            size_t arr_count;
            size_t arr_consumed;
            size_t k;

            arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
            if (arr_consumed == 0) return status;
            if (arr_count > FEB_WARDRIVING_MAX_SOURCES) {
                return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
            }
            pos += arr_consumed;
            for (k = 0; k < arr_count; k++) {
                const char *sdata;
                size_t slen;
                size_t sn = feb_cbor_decode_text(in + pos, in_len - pos, &sdata, &slen,
                                                  FEB_WARDRIVING_SOURCE_MAX_LEN, &status);

                if (sn == 0) return status;
                payload->sources[k] = sdata;
                payload->source_lens[k] = slen;
                pos += sn;
            }
            payload->source_count = arr_count;
            payload->has_sources = 1;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            payload->wifi_interval_ms = value;
            payload->has_wifi_interval_ms = 1;
            pos += n;
            break;
        }
        case 3: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            payload->ble_window_ms = value;
            pos += n;
            break;
        }
        case 4: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            payload->ble_interval_ms = value;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0]) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if (seen[3] != seen[4]) {
        /* ble_window_ms/ble_interval_ms must be present or absent together per
           docs/PROTOCOL.md -- seeing exactly one is a malformed record, not a
           caller-level action-dependent presence rule. */
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    payload->has_ble_params = seen[3];
    return FEB_CBOR_OK;
}

static size_t encode_wardriving_wifi_payload(uint8_t *out, size_t out_cap, const feb_wardriving_wifi_payload_t *wifi)
{
    size_t pos = 0;
    size_t n;

    if (wifi->ssid_len > FEB_WARDRIVING_WIFI_SSID_MAX_LEN || wifi->rssi_offset > 255u) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 5);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ssid", FEB_CBOR_I_KLEN("ssid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, wifi->ssid, wifi->ssid_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "bssid", FEB_CBOR_I_KLEN("bssid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, wifi->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", FEB_CBOR_I_KLEN("rssi_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, wifi->rssi_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "channel", FEB_CBOR_I_KLEN("channel"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, wifi->channel);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "auth", FEB_CBOR_I_KLEN("auth"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, wifi->auth, wifi->auth_len);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

static size_t decode_wardriving_wifi_payload(const uint8_t *in, size_t in_len, feb_wardriving_wifi_payload_t *wifi, feb_cbor_status_t *status)
{
    static const char *const names[5] = {"ssid", "bssid", "rssi_offset", "channel", "auth"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[5] = {0, 0, 0, 0, 0};
    feb_cbor_status_t local_status;
    size_t consumed;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &local_status);
    if (consumed == 0) {
        *status = local_status;
        return 0;
    }
    if (count > 5) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }
    pos = consumed;

    for (i = 0; i < count; i++) {
        const char *key_data;
        size_t key_len;
        size_t key_consumed;
        int found;
        size_t j;
        size_t field_index;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);
        if (key_consumed == 0) {
            *status = local_status;
            return 0;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 5; j++) {
            if (key_len == strlen(names[j]) && memcmp(key_data, names[j], key_len) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        field_index = (size_t)found;
        if (seen[field_index]) {
            *status = FEB_CBOR_ERR_DUPLICATE_KEY;
            return 0;
        }
        if (field_index < next_min) {
            *status = FEB_CBOR_ERR_OUT_OF_ORDER;
            return 0;
        }

        switch (field_index) {
        case 0: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_WARDRIVING_WIFI_SSID_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            wifi->ssid = data;
            wifi->ssid_len = len;
            pos += n;
            break;
        }
        case 1: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_WIFI_SCAN_BSSID_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (len != FEB_WIFI_SCAN_BSSID_LEN) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            memcpy(wifi->bssid, data, FEB_WIFI_SCAN_BSSID_LEN);
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (value > 255u) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            wifi->rssi_offset = value;
            pos += n;
            break;
        }
        case 3: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            wifi->channel = value;
            pos += n;
            break;
        }
        case 4: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            wifi->auth = data;
            wifi->auth_len = len;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 5; i++) {
        if (!seen[i]) {
            *status = FEB_CBOR_ERR_MISSING_FIELD;
            return 0;
        }
    }
    *status = FEB_CBOR_OK;
    return pos;
}

static size_t encode_wardriving_ble_payload(uint8_t *out, size_t out_cap, const feb_wardriving_ble_payload_t *ble)
{
    size_t pos = 0;
    size_t n;
    size_t count = 2;

    if (ble->rssi_offset > 255u) {
        return 0;
    }
    if (ble->has_name) count++;

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, count);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "address", FEB_CBOR_I_KLEN("address"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ble->address, FEB_WARDRIVING_BLE_ADDRESS_LEN);
    if (n == 0) return 0;
    pos += n;

    if (ble->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", FEB_CBOR_I_KLEN("name"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, ble->name, ble->name_len);
        if (n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", FEB_CBOR_I_KLEN("rssi_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ble->rssi_offset);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

static size_t decode_wardriving_ble_payload(const uint8_t *in, size_t in_len, feb_wardriving_ble_payload_t *ble, feb_cbor_status_t *status)
{
    static const char *const names[3] = {"address", "name", "rssi_offset"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[3] = {0, 0, 0};
    feb_cbor_status_t local_status;
    size_t consumed;

    ble->has_name = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &local_status);
    if (consumed == 0) {
        *status = local_status;
        return 0;
    }
    if (count > 3) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }
    pos = consumed;

    for (i = 0; i < count; i++) {
        const char *key_data;
        size_t key_len;
        size_t key_consumed;
        int found;
        size_t j;
        size_t field_index;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);
        if (key_consumed == 0) {
            *status = local_status;
            return 0;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 3; j++) {
            if (key_len == strlen(names[j]) && memcmp(key_data, names[j], key_len) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        field_index = (size_t)found;
        if (seen[field_index]) {
            *status = FEB_CBOR_ERR_DUPLICATE_KEY;
            return 0;
        }
        if (field_index < next_min) {
            *status = FEB_CBOR_ERR_OUT_OF_ORDER;
            return 0;
        }

        switch (field_index) {
        case 0: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_WARDRIVING_BLE_ADDRESS_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (len != FEB_WARDRIVING_BLE_ADDRESS_LEN) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            memcpy(ble->address, data, FEB_WARDRIVING_BLE_ADDRESS_LEN);
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_WARDRIVING_BLE_NAME_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ble->name = data;
            ble->name_len = len;
            ble->has_name = 1;
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (value > 255u) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            ble->rssi_offset = value;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0] || !seen[2]) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wardriving_record(uint8_t *out, size_t out_cap, const feb_wardriving_record_t *record)
{
    size_t pos = 0;
    size_t n;
    const char *source_text;
    size_t source_text_len;

    if (out == NULL || record == NULL) {
        return 0;
    }
    if (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
        source_text = "wifi";
        source_text_len = FEB_CBOR_I_KLEN("wifi");
    } else if (record->payload_kind == FEB_WARDRIVING_PAYLOAD_BLE) {
        source_text = "ble";
        source_text_len = FEB_CBOR_I_KLEN("ble");
    } else {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 6);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "timestamp_ms", FEB_CBOR_I_KLEN("timestamp_ms"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->timestamp_ms);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "utc_timestamp_s", FEB_CBOR_I_KLEN("utc_timestamp_s"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->utc_timestamp_s);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lat_e7_offset", FEB_CBOR_I_KLEN("lat_e7_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->lat_e7_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lon_e7_offset", FEB_CBOR_I_KLEN("lon_e7_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->lon_e7_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "source", FEB_CBOR_I_KLEN("source"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, source_text, source_text_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", FEB_CBOR_I_KLEN("payload"));
    if (n == 0) return 0;
    pos += n;
    if (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
        n = encode_wardriving_wifi_payload(out + pos, out_cap - pos, &record->payload.wifi);
    } else {
        n = encode_wardriving_ble_payload(out + pos, out_cap - pos, &record->payload.ble);
    }
    if (n == 0) return 0;
    pos += n;

    return pos;
}

size_t feb_cbor_decode_wardriving_record(const uint8_t *in, size_t in_len, feb_wardriving_record_t *record, feb_cbor_status_t *status)
{
    static const char *const names[6] = {"timestamp_ms", "utc_timestamp_s", "lat_e7_offset",
                                          "lon_e7_offset", "source", "payload"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[6] = {0, 0, 0, 0, 0, 0};
    feb_cbor_status_t local_status;
    size_t consumed;

    if (status == NULL) {
        return 0;
    }
    if (in == NULL || record == NULL) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &local_status);
    if (consumed == 0) {
        *status = local_status;
        return 0;
    }
    if (count > 6) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }
    pos = consumed;

    for (i = 0; i < count; i++) {
        const char *key_data;
        size_t key_len;
        size_t key_consumed;
        int found;
        size_t j;
        size_t field_index;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);
        if (key_consumed == 0) {
            *status = local_status;
            return 0;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 6; j++) {
            if (key_len == strlen(names[j]) && memcmp(key_data, names[j], key_len) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        field_index = (size_t)found;
        if (seen[field_index]) {
            *status = FEB_CBOR_ERR_DUPLICATE_KEY;
            return 0;
        }
        if (field_index < next_min) {
            *status = FEB_CBOR_ERR_OUT_OF_ORDER;
            return 0;
        }

        switch (field_index) {
        case 0: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            record->timestamp_ms = value;
            pos += n;
            break;
        }
        case 1: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            record->utc_timestamp_s = value;
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            record->lat_e7_offset = value;
            pos += n;
            break;
        }
        case 3: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            record->lon_e7_offset = value;
            pos += n;
            break;
        }
        case 4: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            record->source = data;
            record->source_len = len;
            if (len == FEB_CBOR_I_KLEN("wifi") && memcmp(data, "wifi", len) == 0) {
                record->payload_kind = FEB_WARDRIVING_PAYLOAD_WIFI;
            } else if (len == FEB_CBOR_I_KLEN("ble") && memcmp(data, "ble", len) == 0) {
                record->payload_kind = FEB_WARDRIVING_PAYLOAD_BLE;
            } else {
                *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
                return 0;
            }
            pos += n;
            break;
        }
        case 5: {
            size_t n;

            if (!seen[4]) {
                /* `payload`'s shape depends on `payload_kind`, which is only set while
                   decoding `source` (case 4 above) -- this guards against a map that
                   includes `payload` without `source` ever having appeared at all, which
                   the fixed field-order check above does not itself catch since indices
                   only need to be non-decreasing, not contiguous. Without this guard,
                   record->payload_kind would be read uninitialized. The "source missing"
                   case is also caught by the all-fields-present check below, but only
                   after this would already have used garbage. */
                *status = FEB_CBOR_ERR_MISSING_FIELD;
                return 0;
            }
            if (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
                n = decode_wardriving_wifi_payload(in + pos, in_len - pos, &record->payload.wifi, &local_status);
            } else {
                n = decode_wardriving_ble_payload(in + pos, in_len - pos, &record->payload.ble, &local_status);
            }
            if (n == 0) { *status = local_status; return 0; }
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 6; i++) {
        if (!seen[i]) {
            *status = FEB_CBOR_ERR_MISSING_FIELD;
            return 0;
        }
    }
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wardriving_status_result_payload(uint8_t *out, size_t out_cap, const feb_wardriving_status_result_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->record_count > FEB_WARDRIVING_MAX_RECORDS_PER_BATCH) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 2);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "records", FEB_CBOR_I_KLEN("records"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->record_count);
    if (n == 0) return 0;
    pos += n;
    for (i = 0; i < payload->record_count; i++) {
        n = feb_cbor_encode_wardriving_record(out + pos, out_cap - pos, &payload->records[i]);
        if (n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "backlog_remaining", FEB_CBOR_I_KLEN("backlog_remaining"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->backlog_remaining);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_wardriving_status_result_payload(const uint8_t *in, size_t in_len, feb_wardriving_status_result_payload_t *payload)
{
    size_t count;
    size_t pos;
    feb_cbor_status_t status;
    size_t consumed;
    const char *key_data;
    size_t key_len;
    size_t key_consumed;
    size_t arr_count;
    size_t arr_consumed;
    size_t i;
    uint64_t value;
    size_t n;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->record_count = 0;
    payload->backlog_remaining = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count != 2) {
        return count > 2 ? FEB_CBOR_ERR_TOO_MANY_ENTRIES : FEB_CBOR_ERR_MISSING_FIELD;
    }
    pos = consumed;

    key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                         FEB_CBOR_MAX_TEXT_LEN, &status);
    if (key_consumed == 0) {
        return status;
    }
    if (key_len != strlen("records") || memcmp(key_data, "records", key_len) != 0) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    pos += key_consumed;

    arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
    if (arr_consumed == 0) {
        return status;
    }
    if (arr_count > FEB_WARDRIVING_MAX_RECORDS_PER_BATCH) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += arr_consumed;

    for (i = 0; i < arr_count; i++) {
        n = feb_cbor_decode_wardriving_record(in + pos, in_len - pos, &payload->records[i], &status);
        if (n == 0) {
            return status;
        }
        pos += n;
    }
    payload->record_count = arr_count;

    key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                         FEB_CBOR_MAX_TEXT_LEN, &status);
    if (key_consumed == 0) {
        return status;
    }
    if (key_len != strlen("backlog_remaining") || memcmp(key_data, "backlog_remaining", key_len) != 0) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    pos += key_consumed;

    n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);
    if (n == 0) {
        return status;
    }
    payload->backlog_remaining = value;

    return FEB_CBOR_OK;
}

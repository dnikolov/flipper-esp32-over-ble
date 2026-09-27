#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

size_t feb_cbor_encode_mesh_log_record(uint8_t *out, size_t out_cap, const feb_mesh_log_record_t *record)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || record == NULL) {
        return 0;
    }
    if (record->node_id_len == 0 || record->node_id_len > FEB_MESH_LOG_NODE_ID_MAX_LEN ||
        record->network_len == 0 || record->network_len > FEB_MESH_LOG_NETWORK_MAX_LEN) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 4);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "node_id", FEB_CBOR_I_KLEN("node_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->node_id, record->node_id_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "network", FEB_CBOR_I_KLEN("network"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->network, record->network_len);
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

    return pos;
}

size_t feb_cbor_decode_mesh_log_record(const uint8_t *in, size_t in_len, feb_mesh_log_record_t *record, feb_cbor_status_t *status)
{
    static const char *const names[4] = {"node_id", "network", "lat_e7_offset", "lon_e7_offset"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[4] = {0, 0, 0, 0};
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
    if (count > 4) {
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
        for (j = 0; j < 4; j++) {
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
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_MESH_LOG_NODE_ID_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (len == 0) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            record->node_id = data;
            record->node_id_len = len;
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_MESH_LOG_NETWORK_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (len == 0) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            record->network = data;
            record->network_len = len;
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
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0] || !seen[1] || !seen[2] || !seen[3]) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_mesh_log_status_result_payload(uint8_t *out, size_t out_cap, const feb_mesh_log_status_result_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->record_count > FEB_MESH_LOG_MAX_RECORDS_PER_BATCH) {
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
        n = feb_cbor_encode_mesh_log_record(out + pos, out_cap - pos, &payload->records[i]);
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

feb_cbor_status_t feb_cbor_decode_mesh_log_status_result_payload(const uint8_t *in, size_t in_len, feb_mesh_log_status_result_payload_t *payload)
{
    static const char *const names[2] = {"records", "backlog_remaining"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[2] = {0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->record_count = 0;
    payload->backlog_remaining = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 2) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
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
        for (j = 0; j < 2; j++) {
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

        if (field_index == 0) {
            size_t arr_count;
            size_t arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
            size_t k;

            if (arr_consumed == 0) {
                return status;
            }
            if (arr_count > FEB_MESH_LOG_MAX_RECORDS_PER_BATCH) {
                return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
            }
            pos += arr_consumed;

            for (k = 0; k < arr_count; k++) {
                size_t record_consumed = feb_cbor_decode_mesh_log_record(in + pos, in_len - pos, &payload->records[k], &status);

                if (record_consumed == 0) {
                    return status;
                }
                pos += record_consumed;
            }
            payload->record_count = arr_count;
        } else {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) {
                return status;
            }
            payload->backlog_remaining = value;
            pos += n;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0] || !seen[1]) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    return FEB_CBOR_OK;
}

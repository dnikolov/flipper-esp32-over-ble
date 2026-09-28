/* See cbor_mesh_log.h for the format/scope note. */
#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

/* ---- `mesh_log` capability payloads ---- */

size_t feb_cbor_encode_mesh_log_record(uint8_t* out, size_t out_cap, const feb_mesh_log_record_t* record) {
    if(out == NULL || record == NULL) {
        return 0;
    }
    if(record->node_id_len == 0 || record->node_id_len > FEB_MESH_LOG_NODE_ID_MAX_LEN ||
       record->network_len == 0 || record->network_len > FEB_MESH_LOG_NETWORK_MAX_LEN) {
        return 0;
    }

    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 4);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "node_id", sizeof("node_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->node_id, record->node_id_len);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "network", sizeof("network") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->network, record->network_len);
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

    return pos;
}

size_t feb_cbor_decode_mesh_log_record(
    const uint8_t* in,
    size_t in_len,
    feb_mesh_log_record_t* record,
    feb_cbor_status_t* status) {
    static const char* const names[4] = {"node_id", "network", "lat_e7_offset", "lon_e7_offset"};
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
    if(count > 4) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    uint32_t seen = 0;
    size_t next_min = 0;
    for(size_t i = 0; i < count; i++) {
        size_t field;
        size_t n = feb_cbor_i_decode_table_key(
            in + pos, in_len - pos, names, 4, seen, next_min, &field, status);
        if(n == 0) return 0;
        pos += n;
        switch(field) {
        case 0:
        case 1: {
            const char* data;
            size_t len;
            n = feb_cbor_decode_text(
                in + pos,
                in_len - pos,
                &data,
                &len,
                field == 0 ? FEB_MESH_LOG_NODE_ID_MAX_LEN : FEB_MESH_LOG_NETWORK_MAX_LEN,
                status);
            if(n == 0) return 0;
            if(len == 0) {
                *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
                return 0;
            }
            if(field == 0) {
                record->node_id = data;
                record->node_id_len = len;
            } else {
                record->network = data;
                record->network_len = len;
            }
            break;
        }
        default:
            n = feb_cbor_decode_uint(
                in + pos,
                in_len - pos,
                field == 2 ? &record->lat_e7_offset : &record->lon_e7_offset,
                status);
            if(n == 0) return 0;
            break;
        }
        pos += n;
        seen |= 1u << field;
        next_min = field + 1;
    }

    if(seen != 0xFu) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_mesh_log_status_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_mesh_log_status_result_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->record_count > FEB_MESH_LOG_MAX_RECORDS_PER_BATCH) {
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
        n = feb_cbor_encode_mesh_log_record(out + pos, out_cap - pos, &payload->records[i]);
        if(n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "backlog_remaining", sizeof("backlog_remaining") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->backlog_remaining);
    if(n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_mesh_log_status_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_mesh_log_status_result_payload_t* payload) {
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
    if(array_count > FEB_MESH_LOG_MAX_RECORDS_PER_BATCH) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;
    for(size_t i = 0; i < array_count; i++) {
        size_t item_len =
            feb_cbor_decode_mesh_log_record(in + pos, in_len - pos, &payload->records[i], &status);
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

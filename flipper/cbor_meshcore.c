#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

/* ---- `meshcore_scan` capability payloads ---- */

size_t feb_cbor_encode_meshcore_node(uint8_t* out, size_t out_cap, const feb_meshcore_node_t* node) {
    if(out == NULL || node == NULL) {
        return 0;
    }
    if(node->node_id_len != FEB_MESHCORE_NODE_ID_LEN) {
        return 0;
    }
    if(node->has_name && node->name_len > FEB_MESHCORE_NAME_MAX_LEN) {
        return 0;
    }
    if(node->rssi_offset > 255u) {
        return 0;
    }
    size_t count = 4u;
    if(node->has_name) count++;
    if(node->has_location) count += 2u;

    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "node_id", sizeof("node_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, node->node_id, node->node_id_len);
    if(n == 0) return 0;
    pos += n;

    if(node->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", sizeof("name") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, node->name, node->name_len);
        if(n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "role", sizeof("role") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, node->role, node->role_len);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, node->rssi_offset);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "last_seen_ms", sizeof("last_seen_ms") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, node->last_seen_ms);
    if(n == 0) return 0;
    pos += n;

    if(node->has_location) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "lat_e7_offset", sizeof("lat_e7_offset") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, node->lat_e7_offset);
        if(n == 0) return 0;
        pos += n;

        n = feb_cbor_encode_text(out + pos, out_cap - pos, "lon_e7_offset", sizeof("lon_e7_offset") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, node->lon_e7_offset);
        if(n == 0) return 0;
        pos += n;
    }

    return pos;
}

size_t feb_cbor_decode_meshcore_node(
    const uint8_t* in,
    size_t in_len,
    feb_meshcore_node_t* node,
    feb_cbor_status_t* status) {
    static const char* const names[7] = {
        "node_id", "name", "role", "rssi_offset", "last_seen_ms", "lat_e7_offset", "lon_e7_offset"};
    if(in == NULL || node == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(node, 0, sizeof(*node));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count > 7) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    uint32_t seen = 0;
    size_t next_min = 0;
    for(size_t i = 0; i < count; i++) {
        size_t field;
        size_t n = feb_cbor_i_decode_table_key(
            in + pos, in_len - pos, names, 7, seen, next_min, &field, status);
        if(n == 0) return 0;
        pos += n;
        switch(field) {
        case 0: {
            const char* data;
            size_t len;
            n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len, FEB_MESHCORE_NODE_ID_LEN, status);
            if(n == 0) return 0;
            if(len != FEB_MESHCORE_NODE_ID_LEN) {
                *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
                return 0;
            }
            node->node_id = data;
            node->node_id_len = len;
            break;
        }
        case 1:
            n = feb_cbor_decode_text(
                in + pos, in_len - pos, &node->name, &node->name_len, FEB_MESHCORE_NAME_MAX_LEN, status);
            if(n == 0) return 0;
            node->has_name = 1;
            break;
        case 2:
            n = feb_cbor_decode_text(
                in + pos, in_len - pos, &node->role, &node->role_len, FEB_CBOR_MAX_TEXT_LEN, status);
            if(n == 0) return 0;
            break;
        case 3:
            n = feb_cbor_decode_uint(in + pos, in_len - pos, &node->rssi_offset, status);
            if(n == 0) return 0;
            if(node->rssi_offset > 255u) {
                *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
                return 0;
            }
            break;
        case 4:
            n = feb_cbor_decode_uint(in + pos, in_len - pos, &node->last_seen_ms, status);
            if(n == 0) return 0;
            break;
        default:
            n = feb_cbor_decode_uint(
                in + pos,
                in_len - pos,
                field == 5 ? &node->lat_e7_offset : &node->lon_e7_offset,
                status);
            if(n == 0) return 0;
            break;
        }
        pos += n;
        seen |= 1u << field;
        next_min = field + 1;
    }

    /* node_id, role, rssi_offset, last_seen_ms are required; lat/lon only as a pair. */
    if((seen & 0x1Du) != 0x1Du || ((seen >> 5) & 1u) != ((seen >> 6) & 1u)) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    node->has_location = (seen & 0x20u) ? 1 : 0;
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_meshcore_status_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_meshcore_status_result_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->node_count > FEB_MESHCORE_MAX_NODES_PER_RESULT) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 2);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "nodes", sizeof("nodes") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->node_count);
    if(n == 0) return 0;
    pos += n;
    for(size_t i = 0; i < payload->node_count; i++) {
        n = feb_cbor_encode_meshcore_node(out + pos, out_cap - pos, &payload->nodes[i]);
        if(n == 0) return 0;
        pos += n;
    }
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "total_known_nodes", sizeof("total_known_nodes") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->total_known_nodes);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t feb_cbor_decode_meshcore_status_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_meshcore_status_result_payload_t* payload) {
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

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "nodes", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;

    size_t array_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
    if(n == 0) return status;
    if(array_count > FEB_MESHCORE_MAX_NODES_PER_RESULT) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;
    for(size_t i = 0; i < array_count; i++) {
        size_t item_len =
            feb_cbor_decode_meshcore_node(in + pos, in_len - pos, &payload->nodes[i], &status);
        if(item_len == 0) {
            return status;
        }
        pos += item_len;
    }
    payload->node_count = array_count;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "total_known_nodes", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->total_known_nodes, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

size_t feb_cbor_encode_meshcore_node(uint8_t *out, size_t out_cap, const feb_meshcore_node_t *node)
{
    size_t pos = 0;
    size_t n;
    size_t count = 4; /* node_id, role, rssi_offset, last_seen_ms always present */

    if (out == NULL || node == NULL) {
        return 0;
    }
    if (node->node_id_len != FEB_MESHCORE_NODE_ID_LEN ||
        (node->has_name && node->name_len > FEB_MESHCORE_NAME_MAX_LEN) ||
        node->rssi_offset > 255u) {
        return 0;
    }
    if (node->has_name) count++;
    if (node->has_location) count += 2;

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, count);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "node_id", FEB_CBOR_I_KLEN("node_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, node->node_id, node->node_id_len);
    if (n == 0) return 0;
    pos += n;

    if (node->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", FEB_CBOR_I_KLEN("name"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, node->name, node->name_len);
        if (n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "role", FEB_CBOR_I_KLEN("role"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, node->role, node->role_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", FEB_CBOR_I_KLEN("rssi_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, node->rssi_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "last_seen_ms", FEB_CBOR_I_KLEN("last_seen_ms"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, node->last_seen_ms);
    if (n == 0) return 0;
    pos += n;

    if (node->has_location) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "lat_e7_offset", FEB_CBOR_I_KLEN("lat_e7_offset"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, node->lat_e7_offset);
        if (n == 0) return 0;
        pos += n;

        n = feb_cbor_encode_text(out + pos, out_cap - pos, "lon_e7_offset", FEB_CBOR_I_KLEN("lon_e7_offset"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, node->lon_e7_offset);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
}

size_t feb_cbor_decode_meshcore_node(const uint8_t *in, size_t in_len, feb_meshcore_node_t *node, feb_cbor_status_t *status)
{
    static const char *const names[7] = {"node_id", "name", "role", "rssi_offset",
                                          "last_seen_ms", "lat_e7_offset", "lon_e7_offset"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[7] = {0, 0, 0, 0, 0, 0, 0};
    feb_cbor_status_t local_status;
    size_t consumed;

    if (status == NULL) {
        return 0;
    }
    if (in == NULL || node == NULL) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    node->has_name = 0;
    node->has_location = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &local_status);
    if (consumed == 0) {
        *status = local_status;
        return 0;
    }
    if (count > 7) {
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
        for (j = 0; j < 7; j++) {
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
                                             FEB_MESHCORE_NODE_ID_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (len != FEB_MESHCORE_NODE_ID_LEN) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            node->node_id = data;
            node->node_id_len = len;
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_MESHCORE_NAME_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            node->name = data;
            node->name_len = len;
            node->has_name = 1;
            pos += n;
            break;
        }
        case 2: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            node->role = data;
            node->role_len = len;
            pos += n;
            break;
        }
        case 3: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (value > 255u) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            node->rssi_offset = value;
            pos += n;
            break;
        }
        case 4: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            node->last_seen_ms = value;
            pos += n;
            break;
        }
        case 5: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            node->lat_e7_offset = value;
            pos += n;
            break;
        }
        case 6: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            node->lon_e7_offset = value;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0] || !seen[2] || !seen[3] || !seen[4]) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if (seen[5] != seen[6]) {
        /* lat_e7_offset/lon_e7_offset must be present or absent together, same enforcement
           as cbor_wardriving.c's ble_window_ms/ble_interval_ms pairing. */
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    node->has_location = seen[5];
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_meshcore_status_result_payload(uint8_t *out, size_t out_cap, const feb_meshcore_status_result_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->node_count > FEB_MESHCORE_MAX_NODES_PER_RESULT) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 2);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "nodes", FEB_CBOR_I_KLEN("nodes"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->node_count);
    if (n == 0) return 0;
    pos += n;
    for (i = 0; i < payload->node_count; i++) {
        n = feb_cbor_encode_meshcore_node(out + pos, out_cap - pos, &payload->nodes[i]);
        if (n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "total_known_nodes", FEB_CBOR_I_KLEN("total_known_nodes"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->total_known_nodes);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_meshcore_status_result_payload(const uint8_t *in, size_t in_len, feb_meshcore_status_result_payload_t *payload)
{
    static const char *const names[2] = {"nodes", "total_known_nodes"};
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
    payload->node_count = 0;
    payload->total_known_nodes = 0;

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
            if (arr_count > FEB_MESHCORE_MAX_NODES_PER_RESULT) {
                return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
            }
            pos += arr_consumed;

            for (k = 0; k < arr_count; k++) {
                size_t node_consumed = feb_cbor_decode_meshcore_node(in + pos, in_len - pos, &payload->nodes[k], &status);

                if (node_consumed == 0) {
                    return status;
                }
                pos += node_consumed;
            }
            payload->node_count = arr_count;
        } else {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) {
                return status;
            }
            payload->total_known_nodes = value;
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

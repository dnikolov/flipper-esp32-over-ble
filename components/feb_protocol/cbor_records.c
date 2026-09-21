#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

size_t feb_cbor_encode_unencrypted(uint8_t *out, size_t out_cap, const feb_unencrypted_record_t *record)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || record == NULL) {
        return 0;
    }
    if (record->payload_span_len == 0 || record->payload_span_len > FEB_CBOR_MAX_PAYLOAD) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 5);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", FEB_CBOR_I_KLEN("version"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", FEB_CBOR_I_KLEN("type"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", FEB_CBOR_I_KLEN("session_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->session_id, FEB_SESSION_ID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", FEB_CBOR_I_KLEN("board_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", FEB_CBOR_I_KLEN("payload"));
    if (n == 0) return 0;
    pos += n;
    if (out_cap - pos < record->payload_span_len) {
        return 0;
    }
    memcpy(out + pos, record->payload_span, record->payload_span_len);
    pos += record->payload_span_len;

    return pos;
}

size_t feb_cbor_encode_protected(uint8_t *out, size_t out_cap, const feb_protected_record_t *record)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || record == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 7);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", FEB_CBOR_I_KLEN("version"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", FEB_CBOR_I_KLEN("type"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", FEB_CBOR_I_KLEN("session_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->session_id, FEB_SESSION_ID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", FEB_CBOR_I_KLEN("board_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "sequence", FEB_CBOR_I_KLEN("sequence"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->sequence);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ciphertext", FEB_CBOR_I_KLEN("ciphertext"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->ciphertext, record->ciphertext_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "tag", FEB_CBOR_I_KLEN("tag"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->tag, FEB_GCM_TAG_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_unencrypted(const uint8_t *in, size_t in_len, feb_unencrypted_record_t *record)
{
    static const char *const names[5] = {"version", "type", "session_id", "board_id", "payload"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[5] = {0, 0, 0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || record == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 5) {
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
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            record->version = (uint32_t)value;
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            record->type = data;
            record->type_len = len;
            pos += n;
            break;
        }
        case 2: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_SESSION_ID_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_SESSION_ID_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(record->session_id, data, FEB_SESSION_ID_LEN);
            pos += n;
            break;
        }
        case 3: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            record->board_id = data;
            record->board_id_len = len;
            pos += n;
            break;
        }
        case 4: {
            const uint8_t *span;
            size_t span_len;
            size_t n = feb_cbor_skip_value(in + pos, in_len - pos, 2, &span, &span_len, &status);

            if (n == 0) return status;
            if (span_len > FEB_CBOR_MAX_PAYLOAD) return FEB_CBOR_ERR_TOO_LARGE;
            record->payload_span = span;
            record->payload_span_len = span_len;
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
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    if (pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    return FEB_CBOR_OK;
}

feb_cbor_status_t feb_cbor_decode_protected(const uint8_t *in, size_t in_len, feb_protected_record_t *record)
{
    static const char *const names[7] = {"version", "type", "session_id", "board_id",
                                          "sequence", "ciphertext", "tag"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[7] = {0, 0, 0, 0, 0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || record == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 7) {
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
        for (j = 0; j < 7; j++) {
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
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            record->version = (uint32_t)value;
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            record->type = data;
            record->type_len = len;
            pos += n;
            break;
        }
        case 2: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_SESSION_ID_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_SESSION_ID_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(record->session_id, data, FEB_SESSION_ID_LEN);
            pos += n;
            break;
        }
        case 3: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            record->board_id = data;
            record->board_id_len = len;
            pos += n;
            break;
        }
        case 4: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            record->sequence = value;
            pos += n;
            break;
        }
        case 5: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_CBOR_MAX_BYTES_LEN, &status);

            if (n == 0) return status;
            record->ciphertext = data;
            record->ciphertext_len = len;
            pos += n;
            break;
        }
        case 6: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_GCM_TAG_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_GCM_TAG_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(record->tag, data, FEB_GCM_TAG_LEN);
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 7; i++) {
        if (!seen[i]) {
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    if (pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_error_payload(uint8_t *out, size_t out_cap, const feb_error_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t count = 1;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->has_message) count++;
    if (payload->has_request_id) count++;

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, count);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "code", FEB_CBOR_I_KLEN("code"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->code, payload->code_len);
    if (n == 0) return 0;
    pos += n;

    if (payload->has_message) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "message", FEB_CBOR_I_KLEN("message"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->message, payload->message_len);
        if (n == 0) return 0;
        pos += n;
    }

    if (payload->has_request_id) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", FEB_CBOR_I_KLEN("request_id"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
}

feb_cbor_status_t feb_cbor_decode_error_payload(const uint8_t *in, size_t in_len, feb_error_payload_t *payload)
{
    static const char *const names[3] = {"code", "message", "request_id"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[3] = {0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->has_message = 0;
    payload->has_request_id = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 3) {
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
        for (j = 0; j < 3; j++) {
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
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            payload->code = data;
            payload->code_len = len;
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            payload->message = data;
            payload->message_len = len;
            payload->has_message = 1;
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            payload->request_id = value;
            payload->has_request_id = 1;
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
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_capability_query_payload(uint8_t *out, size_t out_cap, const feb_capability_query_payload_t *payload)
{
    /* Neither firmware ever encodes a populated `requested` today (docs/PROTOCOL.md's
       "Notes on capability discovery": the Flipper always omits it) -- this always emits
       the empty-map wire form regardless of payload->has_requested. */
    (void)payload;
    return feb_cbor_encode_map_header(out, out_cap, 0);
}

feb_cbor_status_t feb_cbor_decode_capability_query_payload(const uint8_t *in, size_t in_len, feb_capability_query_payload_t *payload)
{
    size_t count;
    size_t pos;
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->has_requested = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos = consumed;

    if (count == 1) {
        const char *key_data;
        size_t key_len;
        size_t key_consumed;
        size_t arr_count;
        size_t arr_consumed;
        size_t i;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);
        if (key_consumed == 0) {
            return status;
        }
        if (key_len != strlen("requested") || memcmp(key_data, "requested", key_len) != 0) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        pos += key_consumed;

        arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
        if (arr_consumed == 0) {
            return status;
        }
        if (arr_count > FEB_CBOR_MAX_ARRAY_ENTRIES) {
            return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        }
        pos += arr_consumed;

        for (i = 0; i < arr_count; i++) {
            const char *elem_data;
            size_t elem_len;
            size_t elem_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &elem_data, &elem_len,
                                                        FEB_CBOR_MAX_TEXT_LEN, &status);

            if (elem_consumed == 0) {
                return status;
            }
            pos += elem_consumed;
        }
        payload->has_requested = 1;
    }

    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_capability_response_payload(uint8_t *out, size_t out_cap, const feb_capability_response_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->feature_count > FEB_CAPABILITY_MAX_FEATURES) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 3);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board", FEB_CBOR_I_KLEN("board"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->board, payload->board_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "firmware", FEB_CBOR_I_KLEN("firmware"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->firmware, payload->firmware_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "features", FEB_CBOR_I_KLEN("features"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->feature_count);
    if (n == 0) return 0;
    pos += n;
    for (i = 0; i < payload->feature_count; i++) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->features[i], payload->feature_lens[i]);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
}

feb_cbor_status_t feb_cbor_decode_capability_response_payload(const uint8_t *in, size_t in_len, feb_capability_response_payload_t *payload)
{
    static const char *const names[3] = {"board", "firmware", "features"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[3] = {0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->feature_count = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 3) {
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
        for (j = 0; j < 3; j++) {
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
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            payload->board = data;
            payload->board_len = len;
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            payload->firmware = data;
            payload->firmware_len = len;
            pos += n;
            break;
        }
        case 2: {
            size_t arr_count;
            size_t arr_consumed;
            size_t k;

            arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
            if (arr_consumed == 0) return status;
            if (arr_count > FEB_CAPABILITY_MAX_FEATURES) {
                return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
            }
            pos += arr_consumed;
            for (k = 0; k < arr_count; k++) {
                const char *data;
                size_t len;
                size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                                 FEB_CBOR_MAX_TEXT_LEN, &status);

                if (n == 0) return status;
                payload->features[k] = data;
                payload->feature_lens[k] = len;
                pos += n;
            }
            payload->feature_count = arr_count;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 3; i++) {
        if (!seen[i]) {
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_command_payload(uint8_t *out, size_t out_cap, const feb_command_payload_t *payload)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || payload == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 3);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "capability", FEB_CBOR_I_KLEN("capability"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->capability, payload->capability_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", FEB_CBOR_I_KLEN("request_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "arguments", FEB_CBOR_I_KLEN("arguments"));
    if (n == 0) return 0;
    pos += n;
    if (out_cap - pos < payload->arguments_span_len) {
        return 0;
    }
    memcpy(out + pos, payload->arguments_span, payload->arguments_span_len);
    pos += payload->arguments_span_len;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_command_payload(const uint8_t *in, size_t in_len, feb_command_payload_t *payload)
{
    static const char *const names[3] = {"capability", "request_id", "arguments"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[3] = {0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 3) {
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
        for (j = 0; j < 3; j++) {
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
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            payload->capability = data;
            payload->capability_len = len;
            pos += n;
            break;
        }
        case 1: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            payload->request_id = value;
            pos += n;
            break;
        }
        case 2: {
            const uint8_t *span;
            size_t span_len;
            size_t n;

            if (pos >= in_len || (uint8_t)(in[pos] >> 5) != 5) {
                return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            }
            /* Fresh nesting budget (depth 0), not depth 2 -- see cbor_records.h's
               `command`/`status` comment for why: `arguments` is its own opaque
               "second-level payload," not additional depth accumulated from wherever the
               outer `payload` field itself was positioned. */
            n = feb_cbor_skip_value(in + pos, in_len - pos, 0, &span, &span_len, &status);
            if (n == 0) return status;
            payload->arguments_span = span;
            payload->arguments_span_len = span_len;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 3; i++) {
        if (!seen[i]) {
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_status_payload(uint8_t *out, size_t out_cap, const feb_status_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t count = 2;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->has_result) count++;

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, count);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", FEB_CBOR_I_KLEN("request_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "state", FEB_CBOR_I_KLEN("state"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->state, payload->state_len);
    if (n == 0) return 0;
    pos += n;

    if (payload->has_result) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "result", FEB_CBOR_I_KLEN("result"));
        if (n == 0) return 0;
        pos += n;
        if (out_cap - pos < payload->result_span_len) {
            return 0;
        }
        memcpy(out + pos, payload->result_span, payload->result_span_len);
        pos += payload->result_span_len;
    }

    return pos;
}

feb_cbor_status_t feb_cbor_decode_status_payload(const uint8_t *in, size_t in_len, feb_status_payload_t *payload)
{
    static const char *const names[3] = {"request_id", "state", "result"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[3] = {0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->has_result = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 3) {
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
        for (j = 0; j < 3; j++) {
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
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            payload->request_id = value;
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            payload->state = data;
            payload->state_len = len;
            pos += n;
            break;
        }
        case 2: {
            const uint8_t *span;
            size_t span_len;
            size_t n;

            if (pos >= in_len || (uint8_t)(in[pos] >> 5) != 5) {
                return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            }
            /* Fresh nesting budget (depth 0) -- see cbor_records.h's `command`/`status`
               comment: wifi_scan's own `result` (result -> aps array -> ap-result map ->
               its scalar fields) needs it, and reusing `payload`'s depth=2 convention here
               would reject that frozen, spec-mandated shape as FEB_CBOR_ERR_TOO_DEEP. */
            n = feb_cbor_skip_value(in + pos, in_len - pos, 0, &span, &span_len, &status);
            if (n == 0) return status;
            payload->result_span = span;
            payload->result_span_len = span_len;
            payload->has_result = 1;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0] || !seen[1]) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    return FEB_CBOR_OK;
}

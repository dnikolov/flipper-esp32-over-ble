#include "cbor_codec.h"

#include <string.h>

#define KLEN(literal) (sizeof(literal) - 1u)

static size_t encode_head(uint8_t *out, size_t out_cap, uint8_t major, uint64_t value)
{
    uint8_t prefix = (uint8_t)(major << 5);

    if (out == NULL) {
        return 0;
    }
    if (value < 24u) {
        if (out_cap < 1) {
            return 0;
        }
        out[0] = (uint8_t)(prefix | value);
        return 1;
    }
    if (value < 256u) {
        if (out_cap < 2) {
            return 0;
        }
        out[0] = (uint8_t)(prefix | 24);
        out[1] = (uint8_t)value;
        return 2;
    }
    if (value < 65536u) {
        if (out_cap < 3) {
            return 0;
        }
        out[0] = (uint8_t)(prefix | 25);
        out[1] = (uint8_t)(value >> 8);
        out[2] = (uint8_t)value;
        return 3;
    }
    if (value < 4294967296ULL) {
        if (out_cap < 5) {
            return 0;
        }
        out[0] = (uint8_t)(prefix | 26);
        out[1] = (uint8_t)(value >> 24);
        out[2] = (uint8_t)(value >> 16);
        out[3] = (uint8_t)(value >> 8);
        out[4] = (uint8_t)value;
        return 5;
    }
    if (out_cap < 9) {
        return 0;
    }
    out[0] = (uint8_t)(prefix | 27);
    out[1] = (uint8_t)(value >> 56);
    out[2] = (uint8_t)(value >> 48);
    out[3] = (uint8_t)(value >> 40);
    out[4] = (uint8_t)(value >> 32);
    out[5] = (uint8_t)(value >> 24);
    out[6] = (uint8_t)(value >> 16);
    out[7] = (uint8_t)(value >> 8);
    out[8] = (uint8_t)value;
    return 9;
}

/* Reads one CBOR head (major type + length/value) at in[0], validating shortest-form
   canonical encoding. `expected_major` gates the major type; ai 28-30 are reserved (not
   well-formed for any major type this protocol uses) and are rejected the same as an
   unexpected type, since no dedicated status exists for "not well-formed". */
static size_t decode_head(const uint8_t *in, size_t in_len, uint8_t expected_major,
                           uint64_t *value, feb_cbor_status_t *status)
{
    uint8_t initial;
    uint8_t major;
    uint8_t ai;
    uint64_t v;

    if (in == NULL || in_len < 1) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    initial = in[0];
    major = (uint8_t)(initial >> 5);
    ai = (uint8_t)(initial & 0x1Fu);
    if (major != expected_major) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    if (ai < 24) {
        *value = ai;
        *status = FEB_CBOR_OK;
        return 1;
    }
    if (ai == 24) {
        if (in_len < 2) {
            *status = FEB_CBOR_ERR_TRUNCATED;
            return 0;
        }
        v = in[1];
        if (v < 24) {
            *status = FEB_CBOR_ERR_NON_CANONICAL;
            return 0;
        }
        *value = v;
        *status = FEB_CBOR_OK;
        return 2;
    }
    if (ai == 25) {
        if (in_len < 3) {
            *status = FEB_CBOR_ERR_TRUNCATED;
            return 0;
        }
        v = ((uint64_t)in[1] << 8) | in[2];
        if (v < 256) {
            *status = FEB_CBOR_ERR_NON_CANONICAL;
            return 0;
        }
        *value = v;
        *status = FEB_CBOR_OK;
        return 3;
    }
    if (ai == 26) {
        if (in_len < 5) {
            *status = FEB_CBOR_ERR_TRUNCATED;
            return 0;
        }
        v = ((uint64_t)in[1] << 24) | ((uint64_t)in[2] << 16) | ((uint64_t)in[3] << 8) | in[4];
        if (v < 65536u) {
            *status = FEB_CBOR_ERR_NON_CANONICAL;
            return 0;
        }
        *value = v;
        *status = FEB_CBOR_OK;
        return 5;
    }
    if (ai == 27) {
        if (in_len < 9) {
            *status = FEB_CBOR_ERR_TRUNCATED;
            return 0;
        }
        v = ((uint64_t)in[1] << 56) | ((uint64_t)in[2] << 48) | ((uint64_t)in[3] << 40) |
            ((uint64_t)in[4] << 32) | ((uint64_t)in[5] << 24) | ((uint64_t)in[6] << 16) |
            ((uint64_t)in[7] << 8) | in[8];
        if (v < 4294967296ULL) {
            *status = FEB_CBOR_ERR_NON_CANONICAL;
            return 0;
        }
        *value = v;
        *status = FEB_CBOR_OK;
        return 9;
    }
    if (ai == 31) {
        *status = FEB_CBOR_ERR_INDEFINITE_LENGTH;
        return 0;
    }
    *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
    return 0;
}

size_t feb_cbor_encode_uint(uint8_t *out, size_t out_cap, uint64_t value)
{
    return encode_head(out, out_cap, 0, value);
}

size_t feb_cbor_encode_bytes(uint8_t *out, size_t out_cap, const uint8_t *data, size_t len)
{
    size_t header_len;

    if (data == NULL && len > 0) {
        return 0;
    }
    header_len = encode_head(out, out_cap, 2, (uint64_t)len);
    if (header_len == 0) {
        return 0;
    }
    if (out_cap - header_len < len) {
        return 0;
    }
    if (len > 0) {
        memcpy(out + header_len, data, len);
    }
    return header_len + len;
}

size_t feb_cbor_encode_text(uint8_t *out, size_t out_cap, const char *data, size_t len)
{
    size_t header_len;

    if (data == NULL && len > 0) {
        return 0;
    }
    header_len = encode_head(out, out_cap, 3, (uint64_t)len);
    if (header_len == 0) {
        return 0;
    }
    if (out_cap - header_len < len) {
        return 0;
    }
    if (len > 0) {
        memcpy(out + header_len, data, len);
    }
    return header_len + len;
}

size_t feb_cbor_encode_map_header(uint8_t *out, size_t out_cap, size_t count)
{
    return encode_head(out, out_cap, 5, (uint64_t)count);
}

size_t feb_cbor_encode_array_header(uint8_t *out, size_t out_cap, size_t count)
{
    return encode_head(out, out_cap, 4, (uint64_t)count);
}

size_t feb_cbor_decode_uint(const uint8_t *in, size_t in_len, uint64_t *value,
                             feb_cbor_status_t *status)
{
    feb_cbor_status_t local_status;
    size_t consumed;

    if (value == NULL) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TRUNCATED;
        }
        return 0;
    }
    consumed = decode_head(in, in_len, 0, value, &local_status);
    if (status != NULL) {
        *status = local_status;
    }
    return consumed;
}

size_t feb_cbor_decode_bytes(const uint8_t *in, size_t in_len, const uint8_t **data,
                              size_t *len, size_t max_len, feb_cbor_status_t *status)
{
    uint64_t length;
    feb_cbor_status_t local_status;
    size_t header_len;

    if (data == NULL || len == NULL) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TRUNCATED;
        }
        return 0;
    }
    header_len = decode_head(in, in_len, 2, &length, &local_status);
    if (header_len == 0) {
        if (status != NULL) {
            *status = local_status;
        }
        return 0;
    }
    if (length > (uint64_t)max_len) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TOO_LARGE;
        }
        return 0;
    }
    if (in_len - header_len < length) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TRUNCATED;
        }
        return 0;
    }
    *data = in + header_len;
    *len = (size_t)length;
    if (status != NULL) {
        *status = FEB_CBOR_OK;
    }
    return header_len + (size_t)length;
}

size_t feb_cbor_decode_text(const uint8_t *in, size_t in_len, const char **data,
                             size_t *len, size_t max_len, feb_cbor_status_t *status)
{
    uint64_t length;
    feb_cbor_status_t local_status;
    size_t header_len;

    if (data == NULL || len == NULL) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TRUNCATED;
        }
        return 0;
    }
    header_len = decode_head(in, in_len, 3, &length, &local_status);
    if (header_len == 0) {
        if (status != NULL) {
            *status = local_status;
        }
        return 0;
    }
    if (length > (uint64_t)max_len) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TOO_LARGE;
        }
        return 0;
    }
    if (in_len - header_len < length) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TRUNCATED;
        }
        return 0;
    }
    *data = (const char *)(in + header_len);
    *len = (size_t)length;
    if (status != NULL) {
        *status = FEB_CBOR_OK;
    }
    return header_len + (size_t)length;
}

size_t feb_cbor_decode_map_header(const uint8_t *in, size_t in_len, size_t *count,
                                   feb_cbor_status_t *status)
{
    uint64_t value;
    feb_cbor_status_t local_status;
    size_t consumed;

    if (count == NULL) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TRUNCATED;
        }
        return 0;
    }
    consumed = decode_head(in, in_len, 5, &value, &local_status);
    if (consumed == 0) {
        if (status != NULL) {
            *status = local_status;
        }
        return 0;
    }
    *count = (size_t)value;
    if (status != NULL) {
        *status = FEB_CBOR_OK;
    }
    return consumed;
}

size_t feb_cbor_decode_array_header(const uint8_t *in, size_t in_len, size_t *count,
                                     feb_cbor_status_t *status)
{
    uint64_t value;
    feb_cbor_status_t local_status;
    size_t consumed;

    if (count == NULL) {
        if (status != NULL) {
            *status = FEB_CBOR_ERR_TRUNCATED;
        }
        return 0;
    }
    consumed = decode_head(in, in_len, 4, &value, &local_status);
    if (consumed == 0) {
        if (status != NULL) {
            *status = local_status;
        }
        return 0;
    }
    *count = (size_t)value;
    if (status != NULL) {
        *status = FEB_CBOR_OK;
    }
    return consumed;
}

size_t feb_cbor_skip_value(
    const uint8_t *in,
    size_t in_len,
    size_t depth,
    const uint8_t **out_span,
    size_t *out_span_len,
    feb_cbor_status_t *status)
{
    uint8_t major;
    feb_cbor_status_t local_status;
    size_t consumed;

    if (status == NULL) {
        return 0;
    }
    if (in == NULL || in_len == 0) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    if (depth > FEB_CBOR_MAX_NESTING) {
        *status = FEB_CBOR_ERR_TOO_DEEP;
        return 0;
    }

    major = (uint8_t)(in[0] >> 5);

    switch (major) {
    case 0:
    case 1: {
        uint64_t value;

        consumed = decode_head(in, in_len, major, &value, &local_status);
        if (consumed == 0) {
            *status = local_status;
            return 0;
        }
        break;
    }
    case 2: {
        const uint8_t *data;
        size_t len;

        consumed = feb_cbor_decode_bytes(in, in_len, &data, &len, FEB_CBOR_MAX_BYTES_LEN,
                                          &local_status);
        if (consumed == 0) {
            *status = local_status;
            return 0;
        }
        break;
    }
    case 3: {
        const char *data;
        size_t len;

        consumed = feb_cbor_decode_text(in, in_len, &data, &len, FEB_CBOR_MAX_TEXT_LEN,
                                         &local_status);
        if (consumed == 0) {
            *status = local_status;
            return 0;
        }
        break;
    }
    case 4: {
        size_t count;
        size_t i;
        size_t pos;

        consumed = feb_cbor_decode_array_header(in, in_len, &count, &local_status);
        if (consumed == 0) {
            *status = local_status;
            return 0;
        }
        if (count > FEB_CBOR_MAX_ARRAY_ENTRIES) {
            *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
            return 0;
        }
        pos = consumed;
        for (i = 0; i < count; i++) {
            const uint8_t *elem_span;
            size_t elem_len;
            size_t elem_consumed;

            elem_consumed = feb_cbor_skip_value(in + pos, in_len - pos, depth + 1,
                                                 &elem_span, &elem_len, &local_status);
            if (elem_consumed == 0) {
                *status = local_status;
                return 0;
            }
            pos += elem_consumed;
        }
        consumed = pos;
        break;
    }
    case 5: {
        size_t count;
        size_t i;
        size_t pos;
        const uint8_t *seen_keys[FEB_CBOR_MAX_MAP_ENTRIES];
        size_t seen_lens[FEB_CBOR_MAX_MAP_ENTRIES];

        consumed = feb_cbor_decode_map_header(in, in_len, &count, &local_status);
        if (consumed == 0) {
            *status = local_status;
            return 0;
        }
        if (count > FEB_CBOR_MAX_MAP_ENTRIES) {
            *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
            return 0;
        }
        pos = consumed;
        for (i = 0; i < count; i++) {
            const uint8_t *key_span;
            size_t key_len;
            size_t key_consumed;
            const uint8_t *val_span;
            size_t val_len;
            size_t val_consumed;
            size_t j;

            if (pos >= in_len || (uint8_t)(in[pos] >> 5) != 3) {
                *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
                return 0;
            }
            key_consumed = feb_cbor_skip_value(in + pos, in_len - pos, depth + 1,
                                                &key_span, &key_len, &local_status);
            if (key_consumed == 0) {
                *status = local_status;
                return 0;
            }
            for (j = 0; j < i; j++) {
                if (seen_lens[j] == key_len && memcmp(seen_keys[j], key_span, key_len) == 0) {
                    *status = FEB_CBOR_ERR_DUPLICATE_KEY;
                    return 0;
                }
            }
            seen_keys[i] = key_span;
            seen_lens[i] = key_len;
            pos += key_consumed;

            val_consumed = feb_cbor_skip_value(in + pos, in_len - pos, depth + 1,
                                                &val_span, &val_len, &local_status);
            if (val_consumed == 0) {
                *status = local_status;
                return 0;
            }
            pos += val_consumed;
        }
        consumed = pos;
        break;
    }
    default:
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }

    if (out_span != NULL) {
        *out_span = in;
    }
    if (out_span_len != NULL) {
        *out_span_len = consumed;
    }
    *status = FEB_CBOR_OK;
    return consumed;
}

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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", KLEN("version"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", KLEN("type"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", KLEN("session_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->session_id, FEB_SESSION_ID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", KLEN("board_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", KLEN("payload"));
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", KLEN("version"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", KLEN("type"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", KLEN("session_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->session_id, FEB_SESSION_ID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", KLEN("board_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "sequence", KLEN("sequence"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->sequence);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ciphertext", KLEN("ciphertext"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->ciphertext, record->ciphertext_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "tag", KLEN("tag"));
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "code", KLEN("code"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->code, payload->code_len);
    if (n == 0) return 0;
    pos += n;

    if (payload->has_message) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "message", KLEN("message"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->message, payload->message_len);
        if (n == 0) return 0;
        pos += n;
    }

    if (payload->has_request_id) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", KLEN("request_id"));
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

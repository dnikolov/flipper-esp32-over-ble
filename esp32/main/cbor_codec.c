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
    case 0: {
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
        /* major 1 (negative int), major 6 (tag), and major 7 (true/false/null) are not
           used anywhere in this protocol. */
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board", KLEN("board"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->board, payload->board_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "firmware", KLEN("firmware"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->firmware, payload->firmware_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "features", KLEN("features"));
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "capability", KLEN("capability"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->capability, payload->capability_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", KLEN("request_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "arguments", KLEN("arguments"));
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
            /* Fresh nesting budget (depth 0), not depth 2 -- see cbor_codec.h's
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", KLEN("request_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "state", KLEN("state"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->state, payload->state_len);
    if (n == 0) return 0;
    pos += n;

    if (payload->has_result) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "result", KLEN("result"));
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
            /* Fresh nesting budget (depth 0) -- see cbor_codec.h's `command`/`status`
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

size_t feb_cbor_encode_wifi_scan_ap(uint8_t *out, size_t out_cap, const feb_wifi_scan_ap_t *ap)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || ap == NULL) {
        return 0;
    }
    if (ap->ssid_len > FEB_WIFI_SCAN_SSID_MAX_LEN || ap->rssi_offset > 255u) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 6);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ssid", KLEN("ssid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ap->ssid, ap->ssid_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "bssid", KLEN("bssid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ap->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", KLEN("rssi_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ap->rssi_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "channel", KLEN("channel"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ap->channel);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "phy", KLEN("phy"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, ap->phy, ap->phy_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "auth", KLEN("auth"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, ap->auth, ap->auth_len);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

size_t feb_cbor_decode_wifi_scan_ap(const uint8_t *in, size_t in_len, feb_wifi_scan_ap_t *ap, feb_cbor_status_t *status)
{
    static const char *const names[6] = {"ssid", "bssid", "rssi_offset", "channel", "phy", "auth"};
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
    if (in == NULL || ap == NULL) {
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
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_WIFI_SCAN_SSID_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ap->ssid = data;
            ap->ssid_len = len;
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
            memcpy(ap->bssid, data, FEB_WIFI_SCAN_BSSID_LEN);
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (value > 255u) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            ap->rssi_offset = value;
            pos += n;
            break;
        }
        case 3: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ap->channel = value;
            pos += n;
            break;
        }
        case 4: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ap->phy = data;
            ap->phy_len = len;
            pos += n;
            break;
        }
        case 5: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ap->auth = data;
            ap->auth_len = len;
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

size_t feb_cbor_encode_wifi_scan_result_payload(uint8_t *out, size_t out_cap, const feb_wifi_scan_result_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->ap_count > FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "aps", KLEN("aps"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->ap_count);
    if (n == 0) return 0;
    pos += n;

    for (i = 0; i < payload->ap_count; i++) {
        n = feb_cbor_encode_wifi_scan_ap(out + pos, out_cap - pos, &payload->aps[i]);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
}

feb_cbor_status_t feb_cbor_decode_wifi_scan_result_payload(const uint8_t *in, size_t in_len, feb_wifi_scan_result_payload_t *payload)
{
    size_t count;
    size_t pos;
    size_t i;
    feb_cbor_status_t status;
    size_t consumed;
    const char *key_data;
    size_t key_len;
    size_t key_consumed;
    size_t arr_count;
    size_t arr_consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->ap_count = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    if (count == 0) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    pos = consumed;

    key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                         FEB_CBOR_MAX_TEXT_LEN, &status);
    if (key_consumed == 0) {
        return status;
    }
    if (key_len != strlen("aps") || memcmp(key_data, "aps", key_len) != 0) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    pos += key_consumed;

    arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
    if (arr_consumed == 0) {
        return status;
    }
    if (arr_count > FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += arr_consumed;

    for (i = 0; i < arr_count; i++) {
        size_t ap_consumed = feb_cbor_decode_wifi_scan_ap(in + pos, in_len - pos, &payload->aps[i], &status);

        if (ap_consumed == 0) {
            return status;
        }
        pos += ap_consumed;
    }
    payload->ap_count = arr_count;

    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_ble_scan_device(uint8_t *out, size_t out_cap, const feb_ble_scan_device_t *device)
{
    size_t pos = 0;
    size_t n;
    size_t count = 3;

    if (out == NULL || device == NULL) {
        return 0;
    }
    if (device->rssi_offset > 255u) {
        return 0;
    }
    if (device->has_name) count++;

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, count);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "address", KLEN("address"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, device->address, FEB_BLE_SCAN_ADDRESS_LEN);
    if (n == 0) return 0;
    pos += n;

    if (device->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", KLEN("name"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, device->name, device->name_len);
        if (n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", KLEN("rssi_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, device->rssi_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "addr_type", KLEN("addr_type"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, device->addr_type, device->addr_type_len);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

size_t feb_cbor_decode_ble_scan_device(const uint8_t *in, size_t in_len, feb_ble_scan_device_t *device, feb_cbor_status_t *status)
{
    static const char *const names[4] = {"address", "name", "rssi_offset", "addr_type"};
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
    if (in == NULL || device == NULL) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    device->has_name = 0;

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
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_BLE_SCAN_ADDRESS_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (len != FEB_BLE_SCAN_ADDRESS_LEN) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            memcpy(device->address, data, FEB_BLE_SCAN_ADDRESS_LEN);
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_BLE_SCAN_NAME_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            device->name = data;
            device->name_len = len;
            device->has_name = 1;
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (value > 255u) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            device->rssi_offset = value;
            pos += n;
            break;
        }
        case 3: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            device->addr_type = data;
            device->addr_type_len = len;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0] || !seen[2] || !seen[3]) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_ble_scan_result_payload(uint8_t *out, size_t out_cap, const feb_ble_scan_result_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->device_count > FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "devices", KLEN("devices"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->device_count);
    if (n == 0) return 0;
    pos += n;

    for (i = 0; i < payload->device_count; i++) {
        n = feb_cbor_encode_ble_scan_device(out + pos, out_cap - pos, &payload->devices[i]);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
}

feb_cbor_status_t feb_cbor_decode_ble_scan_result_payload(const uint8_t *in, size_t in_len, feb_ble_scan_result_payload_t *payload)
{
    size_t count;
    size_t pos;
    size_t i;
    feb_cbor_status_t status;
    size_t consumed;
    const char *key_data;
    size_t key_len;
    size_t key_consumed;
    size_t arr_count;
    size_t arr_consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->device_count = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    if (count == 0) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    pos = consumed;

    key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                         FEB_CBOR_MAX_TEXT_LEN, &status);
    if (key_consumed == 0) {
        return status;
    }
    if (key_len != strlen("devices") || memcmp(key_data, "devices", key_len) != 0) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    pos += key_consumed;

    arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
    if (arr_consumed == 0) {
        return status;
    }
    if (arr_count > FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += arr_consumed;

    for (i = 0; i < arr_count; i++) {
        size_t device_consumed = feb_cbor_decode_ble_scan_device(in + pos, in_len - pos, &payload->devices[i], &status);

        if (device_consumed == 0) {
            return status;
        }
        pos += device_consumed;
    }
    payload->device_count = arr_count;

    return FEB_CBOR_OK;
}

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
    if (payload->has_ble_window_ms) count++;
    if (payload->has_ble_interval_ms) count++;

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, count);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "action", KLEN("action"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->action, payload->action_len);
    if (n == 0) return 0;
    pos += n;

    if (payload->has_sources) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "sources", KLEN("sources"));
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
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "wifi_interval_ms", KLEN("wifi_interval_ms"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->wifi_interval_ms);
        if (n == 0) return 0;
        pos += n;
    }

    if (payload->has_ble_window_ms) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "ble_window_ms", KLEN("ble_window_ms"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->ble_window_ms);
        if (n == 0) return 0;
        pos += n;
    }

    if (payload->has_ble_interval_ms) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "ble_interval_ms", KLEN("ble_interval_ms"));
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
    payload->has_ble_window_ms = 0;
    payload->has_ble_interval_ms = 0;

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
            payload->has_ble_window_ms = 1;
            pos += n;
            break;
        }
        case 4: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            payload->ble_interval_ms = value;
            payload->has_ble_interval_ms = 1;
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ssid", KLEN("ssid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, wifi->ssid, wifi->ssid_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "bssid", KLEN("bssid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, wifi->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", KLEN("rssi_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, wifi->rssi_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "channel", KLEN("channel"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, wifi->channel);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "auth", KLEN("auth"));
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "address", KLEN("address"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ble->address, FEB_WARDRIVING_BLE_ADDRESS_LEN);
    if (n == 0) return 0;
    pos += n;

    if (ble->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", KLEN("name"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, ble->name, ble->name_len);
        if (n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", KLEN("rssi_offset"));
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
        source_text_len = KLEN("wifi");
    } else if (record->payload_kind == FEB_WARDRIVING_PAYLOAD_BLE) {
        source_text = "ble";
        source_text_len = KLEN("ble");
    } else {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 5);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "timestamp_ms", KLEN("timestamp_ms"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->timestamp_ms);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lat_e7_offset", KLEN("lat_e7_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->lat_e7_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lon_e7_offset", KLEN("lon_e7_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->lon_e7_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "source", KLEN("source"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, source_text, source_text_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", KLEN("payload"));
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
    static const char *const names[5] = {"timestamp_ms", "lat_e7_offset", "lon_e7_offset", "source", "payload"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[5] = {0, 0, 0, 0, 0};
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
            record->lat_e7_offset = value;
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            record->lon_e7_offset = value;
            pos += n;
            break;
        }
        case 3: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            record->source = data;
            record->source_len = len;
            if (len == KLEN("wifi") && memcmp(data, "wifi", len) == 0) {
                record->payload_kind = FEB_WARDRIVING_PAYLOAD_WIFI;
            } else if (len == KLEN("ble") && memcmp(data, "ble", len) == 0) {
                record->payload_kind = FEB_WARDRIVING_PAYLOAD_BLE;
            } else {
                *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
                return 0;
            }
            pos += n;
            break;
        }
        case 4: {
            size_t n;

            if (!seen[3]) {
                /* `payload`'s shape depends on `payload_kind`, which is only set while
                   decoding `source` (case 3 above) -- this guards against a map that
                   includes `payload` without `source` ever having appeared at all (e.g.
                   {timestamp_ms, lat_e7_offset, lon_e7_offset, payload}, skipping
                   `source`), which the fixed field-order check above does not itself catch
                   since indices only need to be non-decreasing, not contiguous. Without
                   this guard, record->payload_kind would be read uninitialized. The
                   "source missing" case is also caught by the all-fields-present check
                   below, but only after this would already have used garbage. */
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

    for (i = 0; i < 5; i++) {
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "records", KLEN("records"));
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

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "backlog_remaining", KLEN("backlog_remaining"));
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

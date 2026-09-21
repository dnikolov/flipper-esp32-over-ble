#include "cbor_codec.h"

#include <string.h>

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

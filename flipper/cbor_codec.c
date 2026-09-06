#include "cbor_codec.h"

#include <string.h>

/* ---- shared header encode/decode (major type + shortest-form length/value) ---- */

static size_t encode_head(uint8_t* out, size_t out_cap, uint8_t major, uint64_t n) {
    if(n < 24) {
        if(out_cap < 1) {
            return 0;
        }
        out[0] = (uint8_t)((major << 5) | (uint8_t)n);
        return 1;
    }
    if(n < 256) {
        if(out_cap < 2) {
            return 0;
        }
        out[0] = (uint8_t)((major << 5) | 24);
        out[1] = (uint8_t)n;
        return 2;
    }
    if(n < 65536) {
        if(out_cap < 3) {
            return 0;
        }
        out[0] = (uint8_t)((major << 5) | 25);
        out[1] = (uint8_t)(n >> 8);
        out[2] = (uint8_t)n;
        return 3;
    }
    if(n < 4294967296ULL) {
        if(out_cap < 5) {
            return 0;
        }
        out[0] = (uint8_t)((major << 5) | 26);
        out[1] = (uint8_t)(n >> 24);
        out[2] = (uint8_t)(n >> 16);
        out[3] = (uint8_t)(n >> 8);
        out[4] = (uint8_t)n;
        return 5;
    }
    if(out_cap < 9) {
        return 0;
    }
    out[0] = (uint8_t)((major << 5) | 27);
    for(int i = 0; i < 8; i++) {
        out[1 + i] = (uint8_t)(n >> (8 * (7 - i)));
    }
    return 9;
}

/* Reads one major-type-tagged head (int value, or byte/text/array/map length) starting at
   in[0]. Rejects a major-type mismatch, indefinite length (ai == 31), non-shortest-form
   encoding, and truncation. Returns bytes consumed, or 0 with *status set on failure. */
static size_t decode_head(
    const uint8_t* in,
    size_t in_len,
    uint8_t expected_major,
    uint64_t* value,
    feb_cbor_status_t* status) {
    if(in_len < 1) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    uint8_t first = in[0];
    uint8_t major = (uint8_t)(first >> 5);
    uint8_t ai = (uint8_t)(first & 0x1F);
    if(major != expected_major) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    if(ai == 31) {
        *status = FEB_CBOR_ERR_INDEFINITE_LENGTH;
        return 0;
    }
    if(ai < 24) {
        *value = ai;
        return 1;
    }
    if(ai == 24) {
        if(in_len < 2) {
            *status = FEB_CBOR_ERR_TRUNCATED;
            return 0;
        }
        uint8_t v = in[1];
        if(v < 24) {
            *status = FEB_CBOR_ERR_NON_CANONICAL;
            return 0;
        }
        *value = v;
        return 2;
    }
    if(ai == 25) {
        if(in_len < 3) {
            *status = FEB_CBOR_ERR_TRUNCATED;
            return 0;
        }
        uint16_t v = (uint16_t)(((uint16_t)in[1] << 8) | in[2]);
        if(v < 256) {
            *status = FEB_CBOR_ERR_NON_CANONICAL;
            return 0;
        }
        *value = v;
        return 3;
    }
    if(ai == 26) {
        if(in_len < 5) {
            *status = FEB_CBOR_ERR_TRUNCATED;
            return 0;
        }
        uint32_t v = ((uint32_t)in[1] << 24) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 8) |
                     (uint32_t)in[4];
        if(v < 65536u) {
            *status = FEB_CBOR_ERR_NON_CANONICAL;
            return 0;
        }
        *value = v;
        return 5;
    }
    if(ai == 27) {
        if(in_len < 9) {
            *status = FEB_CBOR_ERR_TRUNCATED;
            return 0;
        }
        uint64_t v = 0;
        for(int i = 1; i <= 8; i++) {
            v = (v << 8) | in[i];
        }
        if(v < 4294967296ULL) {
            *status = FEB_CBOR_ERR_NON_CANONICAL;
            return 0;
        }
        *value = v;
        return 9;
    }
    /* ai in [28, 30]: reserved, not well-formed. */
    *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
    return 0;
}

/* ---- Primitives: canonical encode ---- */

size_t feb_cbor_encode_uint(uint8_t* out, size_t out_cap, uint64_t value) {
    if(out == NULL) {
        return 0;
    }
    return encode_head(out, out_cap, 0, value);
}

size_t feb_cbor_encode_bytes(uint8_t* out, size_t out_cap, const uint8_t* data, size_t len) {
    if(out == NULL || (data == NULL && len > 0)) {
        return 0;
    }
    size_t head_len = encode_head(out, out_cap, 2, len);
    if(head_len == 0) {
        return 0;
    }
    if(out_cap - head_len < len) {
        return 0;
    }
    if(len > 0) {
        memcpy(out + head_len, data, len);
    }
    return head_len + len;
}

size_t feb_cbor_encode_text(uint8_t* out, size_t out_cap, const char* data, size_t len) {
    if(out == NULL || (data == NULL && len > 0)) {
        return 0;
    }
    size_t head_len = encode_head(out, out_cap, 3, len);
    if(head_len == 0) {
        return 0;
    }
    if(out_cap - head_len < len) {
        return 0;
    }
    if(len > 0) {
        memcpy(out + head_len, data, len);
    }
    return head_len + len;
}

size_t feb_cbor_encode_map_header(uint8_t* out, size_t out_cap, size_t count) {
    if(out == NULL) {
        return 0;
    }
    return encode_head(out, out_cap, 5, count);
}

size_t feb_cbor_encode_array_header(uint8_t* out, size_t out_cap, size_t count) {
    if(out == NULL) {
        return 0;
    }
    return encode_head(out, out_cap, 4, count);
}

/* ---- Primitives: canonical decode ---- */

size_t feb_cbor_decode_uint(
    const uint8_t* in,
    size_t in_len,
    uint64_t* value,
    feb_cbor_status_t* status) {
    return decode_head(in, in_len, 0, value, status);
}

size_t feb_cbor_decode_bytes(
    const uint8_t* in,
    size_t in_len,
    const uint8_t** data,
    size_t* len,
    size_t max_len,
    feb_cbor_status_t* status) {
    uint64_t raw_len = 0;
    size_t head_len = decode_head(in, in_len, 2, &raw_len, status);
    if(head_len == 0) {
        return 0;
    }
    if(raw_len > (uint64_t)max_len) {
        *status = FEB_CBOR_ERR_TOO_LARGE;
        return 0;
    }
    size_t body_len = (size_t)raw_len;
    if(in_len - head_len < body_len) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    *data = in + head_len;
    *len = body_len;
    *status = FEB_CBOR_OK;
    return head_len + body_len;
}

size_t feb_cbor_decode_text(
    const uint8_t* in,
    size_t in_len,
    const char** data,
    size_t* len,
    size_t max_len,
    feb_cbor_status_t* status) {
    uint64_t raw_len = 0;
    size_t head_len = decode_head(in, in_len, 3, &raw_len, status);
    if(head_len == 0) {
        return 0;
    }
    if(raw_len > (uint64_t)max_len) {
        *status = FEB_CBOR_ERR_TOO_LARGE;
        return 0;
    }
    size_t body_len = (size_t)raw_len;
    if(in_len - head_len < body_len) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    *data = (const char*)(in + head_len);
    *len = body_len;
    *status = FEB_CBOR_OK;
    return head_len + body_len;
}

size_t
    feb_cbor_decode_map_header(const uint8_t* in, size_t in_len, size_t* count, feb_cbor_status_t* status) {
    uint64_t raw_count = 0;
    size_t head_len = decode_head(in, in_len, 5, &raw_count, status);
    if(head_len == 0) {
        return 0;
    }
    *count = (size_t)raw_count;
    *status = FEB_CBOR_OK;
    return head_len;
}

size_t
    feb_cbor_decode_array_header(const uint8_t* in, size_t in_len, size_t* count, feb_cbor_status_t* status) {
    uint64_t raw_count = 0;
    size_t head_len = decode_head(in, in_len, 4, &raw_count, status);
    if(head_len == 0) {
        return 0;
    }
    *count = (size_t)raw_count;
    *status = FEB_CBOR_OK;
    return head_len;
}

/* ---- Generic structural validator for an opaque value (docs/PROTOCOL.md's payload maps
   use only unsigned integers, byte strings, text strings, arrays of text strings, maps,
   and (in future capability/command payloads) simple true/false/null values). ---- */

size_t feb_cbor_skip_value(
    const uint8_t* in,
    size_t in_len,
    size_t depth,
    const uint8_t** out_span,
    size_t* out_span_len,
    feb_cbor_status_t* status) {
    if(depth > FEB_CBOR_MAX_NESTING) {
        *status = FEB_CBOR_ERR_TOO_DEEP;
        return 0;
    }
    if(in_len < 1) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    uint8_t major = (uint8_t)(in[0] >> 5);
    uint8_t ai = (uint8_t)(in[0] & 0x1F);
    size_t consumed = 0;

    switch(major) {
    case 0: {
        uint64_t v;
        consumed = feb_cbor_decode_uint(in, in_len, &v, status);
        if(consumed == 0) {
            return 0;
        }
        break;
    }
    case 2: {
        const uint8_t* data;
        size_t len;
        consumed = feb_cbor_decode_bytes(in, in_len, &data, &len, FEB_CBOR_MAX_BYTES_LEN, status);
        if(consumed == 0) {
            return 0;
        }
        break;
    }
    case 3: {
        const char* data;
        size_t len;
        consumed = feb_cbor_decode_text(in, in_len, &data, &len, FEB_CBOR_MAX_TEXT_LEN, status);
        if(consumed == 0) {
            return 0;
        }
        break;
    }
    case 4: {
        size_t count = 0;
        size_t head_len = feb_cbor_decode_array_header(in, in_len, &count, status);
        if(head_len == 0) {
            return 0;
        }
        if(count > FEB_CBOR_MAX_ARRAY_ENTRIES) {
            *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
            return 0;
        }
        size_t pos = head_len;
        for(size_t i = 0; i < count; i++) {
            size_t item_len =
                feb_cbor_skip_value(in + pos, in_len - pos, depth + 1, NULL, NULL, status);
            if(item_len == 0) {
                return 0;
            }
            pos += item_len;
        }
        consumed = pos;
        break;
    }
    case 5: {
        size_t count = 0;
        size_t head_len = feb_cbor_decode_map_header(in, in_len, &count, status);
        if(head_len == 0) {
            return 0;
        }
        if(count > FEB_CBOR_MAX_MAP_ENTRIES) {
            *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
            return 0;
        }
        const uint8_t* key_ptrs[FEB_CBOR_MAX_MAP_ENTRIES];
        size_t key_lens[FEB_CBOR_MAX_MAP_ENTRIES];
        size_t pos = head_len;
        for(size_t i = 0; i < count; i++) {
            if((uint8_t)(in[pos] >> 5) != 3) {
                *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
                return 0;
            }
            const char* key_data;
            size_t key_len;
            size_t key_head_len = feb_cbor_decode_text(
                in + pos, in_len - pos, &key_data, &key_len, FEB_CBOR_MAX_TEXT_LEN, status);
            if(key_head_len == 0) {
                return 0;
            }
            for(size_t j = 0; j < i; j++) {
                if(key_lens[j] == key_len &&
                   memcmp(key_ptrs[j], key_data, key_len) == 0) {
                    *status = FEB_CBOR_ERR_DUPLICATE_KEY;
                    return 0;
                }
            }
            key_ptrs[i] = (const uint8_t*)key_data;
            key_lens[i] = key_len;
            pos += key_head_len;

            size_t value_len =
                feb_cbor_skip_value(in + pos, in_len - pos, depth + 1, NULL, NULL, status);
            if(value_len == 0) {
                return 0;
            }
            pos += value_len;
        }
        consumed = pos;
        break;
    }
    case 7: {
        if(ai == 20 || ai == 21 || ai == 22 || ai == 23) {
            consumed = 1;
        } else {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        break;
    }
    default:
        /* major 1 (negative int) and major 6 (tag) are not used anywhere in this protocol. */
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }

    if(out_span != NULL) {
        *out_span = in;
    }
    if(out_span_len != NULL) {
        *out_span_len = consumed;
    }
    *status = FEB_CBOR_OK;
    return consumed;
}

/* ---- Fixed-order field helpers shared by the envelope/error-payload decoders ---- */

static int text_matches(const char* data, size_t len, const char* literal) {
    size_t literal_len = strlen(literal);
    return len == literal_len && memcmp(data, literal, literal_len) == 0;
}

/* Decodes one map key (text) at *pos, rejecting a key that duplicates one already seen in
   `seen_ptrs`/`seen_lens` (first `seen_count` entries) or that does not match `expected`. */
static size_t decode_expected_key(
    const uint8_t* in,
    size_t in_len,
    const char* expected,
    const uint8_t** seen_ptrs,
    size_t* seen_lens,
    size_t seen_count,
    feb_cbor_status_t* status) {
    const char* key_data;
    size_t key_len;
    size_t n =
        feb_cbor_decode_text(in, in_len, &key_data, &key_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) {
        return 0;
    }
    for(size_t j = 0; j < seen_count; j++) {
        if(seen_lens[j] == key_len && memcmp(seen_ptrs[j], key_data, key_len) == 0) {
            *status = FEB_CBOR_ERR_DUPLICATE_KEY;
            return 0;
        }
    }
    if(!text_matches(key_data, key_len, expected)) {
        *status = FEB_CBOR_ERR_OUT_OF_ORDER;
        return 0;
    }
    seen_ptrs[seen_count] = (const uint8_t*)key_data;
    seen_lens[seen_count] = key_len;
    *status = FEB_CBOR_OK;
    return n;
}

/* ---- Envelope shapes ---- */

size_t feb_cbor_encode_unencrypted(
    uint8_t* out,
    size_t out_cap,
    const feb_unencrypted_record_t* record) {
    if(out == NULL || record == NULL) {
        return 0;
    }
    if(record->payload_span_len > FEB_CBOR_MAX_PAYLOAD) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 5);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", sizeof("version") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", sizeof("type") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", sizeof("session_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->session_id, FEB_SESSION_ID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", sizeof("board_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", sizeof("payload") - 1);
    if(n == 0) return 0;
    pos += n;
    if(out_cap - pos < record->payload_span_len) {
        return 0;
    }
    if(record->payload_span_len > 0) {
        memcpy(out + pos, record->payload_span, record->payload_span_len);
    }
    pos += record->payload_span_len;
    return pos;
}

size_t
    feb_cbor_encode_protected(uint8_t* out, size_t out_cap, const feb_protected_record_t* record) {
    if(out == NULL || record == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 7);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", sizeof("version") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", sizeof("type") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", sizeof("session_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->session_id, FEB_SESSION_ID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", sizeof("board_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "sequence", sizeof("sequence") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->sequence);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ciphertext", sizeof("ciphertext") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->ciphertext, record->ciphertext_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "tag", sizeof("tag") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->tag, FEB_GCM_TAG_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_unencrypted(const uint8_t* in, size_t in_len, feb_unencrypted_record_t* record) {
    if(in == NULL || record == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 5) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 5) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[5];
    size_t seen_lens[5];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "version", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    {
        uint64_t v;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &v, &status);
        if(n == 0) return status;
        record->version = (uint32_t)v;
        pos += n;
    }

    n = decode_expected_key(in + pos, in_len - pos, "type", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &record->type, &record->type_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "session_id", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(
            in + pos, in_len - pos, &data, &len, FEB_SESSION_ID_LEN, &status);
        if(n == 0) return status;
        if(len != FEB_SESSION_ID_LEN) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        memcpy(record->session_id, data, FEB_SESSION_ID_LEN);
        pos += n;
    }

    n = decode_expected_key(in + pos, in_len - pos, "board_id", seen_ptrs, seen_lens, 3, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos,
        in_len - pos,
        &record->board_id,
        &record->board_id_len,
        FEB_CBOR_MAX_TEXT_LEN,
        &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "payload", seen_ptrs, seen_lens, 4, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_skip_value(
        in + pos, in_len - pos, 1, &record->payload_span, &record->payload_span_len, &status);
    if(n == 0) return status;
    if(record->payload_span_len > FEB_CBOR_MAX_PAYLOAD) {
        return FEB_CBOR_ERR_TOO_LARGE;
    }
    pos += n;

    return FEB_CBOR_OK;
}

feb_cbor_status_t
    feb_cbor_decode_protected(const uint8_t* in, size_t in_len, feb_protected_record_t* record) {
    if(in == NULL || record == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 7) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 7) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[7];
    size_t seen_lens[7];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "version", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    {
        uint64_t v;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &v, &status);
        if(n == 0) return status;
        record->version = (uint32_t)v;
        pos += n;
    }

    n = decode_expected_key(in + pos, in_len - pos, "type", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &record->type, &record->type_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "session_id", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(
            in + pos, in_len - pos, &data, &len, FEB_SESSION_ID_LEN, &status);
        if(n == 0) return status;
        if(len != FEB_SESSION_ID_LEN) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        memcpy(record->session_id, data, FEB_SESSION_ID_LEN);
        pos += n;
    }

    n = decode_expected_key(in + pos, in_len - pos, "board_id", seen_ptrs, seen_lens, 3, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos,
        in_len - pos,
        &record->board_id,
        &record->board_id_len,
        FEB_CBOR_MAX_TEXT_LEN,
        &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "sequence", seen_ptrs, seen_lens, 4, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->sequence, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "ciphertext", seen_ptrs, seen_lens, 5, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_bytes(
        in + pos,
        in_len - pos,
        &record->ciphertext,
        &record->ciphertext_len,
        FEB_CBOR_MAX_BYTES_LEN,
        &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "tag", seen_ptrs, seen_lens, 6, &status);
    if(n == 0) return status;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_GCM_TAG_LEN, &status);
        if(n == 0) return status;
        if(len != FEB_GCM_TAG_LEN) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        memcpy(record->tag, data, FEB_GCM_TAG_LEN);
        pos += n;
    }

    return FEB_CBOR_OK;
}

/* ---- `error` payload ---- */

size_t feb_cbor_encode_error_payload(uint8_t* out, size_t out_cap, const feb_error_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->code == NULL) {
        return 0;
    }
    size_t count = 1;
    if(payload->has_message) count++;
    if(payload->has_request_id) count++;

    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "code", sizeof("code") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->code, payload->code_len);
    if(n == 0) return 0;
    pos += n;

    if(payload->has_message) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "message", sizeof("message") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->message, payload->message_len);
        if(n == 0) return 0;
        pos += n;
    }
    if(payload->has_request_id) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", sizeof("request_id") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_error_payload(const uint8_t* in, size_t in_len, feb_error_payload_t* payload) {
    if(in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 1) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    memset(payload, 0, sizeof(*payload));

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "code", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->code, &payload->code_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    int seen_message = 0;
    int seen_request_id = 0;
    for(size_t i = 1; i < count; i++) {
        const char* key_data;
        size_t key_len;
        n = feb_cbor_decode_text(
            in + pos, in_len - pos, &key_data, &key_len, FEB_CBOR_MAX_TEXT_LEN, &status);
        if(n == 0) return status;
        for(size_t j = 0; j < i; j++) {
            if(seen_lens[j] == key_len && memcmp(seen_ptrs[j], key_data, key_len) == 0) {
                return FEB_CBOR_ERR_DUPLICATE_KEY;
            }
        }
        seen_ptrs[i] = (const uint8_t*)key_data;
        seen_lens[i] = key_len;
        pos += n;

        if(text_matches(key_data, key_len, "message")) {
            if(seen_request_id) {
                return FEB_CBOR_ERR_OUT_OF_ORDER;
            }
            n = feb_cbor_decode_text(
                in + pos,
                in_len - pos,
                &payload->message,
                &payload->message_len,
                FEB_CBOR_MAX_TEXT_LEN,
                &status);
            if(n == 0) return status;
            pos += n;
            payload->has_message = 1;
            seen_message = 1;
            (void)seen_message;
        } else if(text_matches(key_data, key_len, "request_id")) {
            n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->request_id, &status);
            if(n == 0) return status;
            pos += n;
            payload->has_request_id = 1;
            seen_request_id = 1;
        } else {
            return FEB_CBOR_ERR_OUT_OF_ORDER;
        }
    }

    return FEB_CBOR_OK;
}

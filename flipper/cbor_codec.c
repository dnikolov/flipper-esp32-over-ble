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

/* Per-recursion-depth duplicate-key scratch for feb_cbor_skip_value()'s map case, indexed by
   `depth` (0..FEB_CBOR_MAX_NESTING, the only values the map case can execute at -- depth >
   FEB_CBOR_MAX_NESTING returns before reaching this code). Stack-local key_ptrs/key_lens
   arrays here would cost FEB_CBOR_MAX_MAP_ENTRIES*(sizeof(ptr)+sizeof(size_t)) bytes *per
   recursion level*, and this function is reachable from the Flipper's 1280-byte
   BleEventWorker stack (docs/SESSION_MEMORY.md/PLAN.md 2026-09-07 wifi_scan stack-usage
   measurement: a fresh depth-0 opaque-field budget lets an authenticated peer force this
   function 6 levels deep, i.e. depths 0-5, before the depth check rejects the innermost
   call). Indexing by depth (rather than one flat static) is required for correctness, not
   just style: a map nested inside an array nested inside a map needs each depth's own
   duplicate-key state to survive across its own call while the deeper recursive call runs
   and returns -- a single shared static would be silently clobbered by the nested call.
   Safe as static/depth-indexed for the same reason every other static in this codebase's
   BLE-thread-reachable code is: BLE events dispatch single-threaded and sequentially, one in
   flight at a time, so recursive calls to the same depth are always sequential (siblings),
   never concurrent, and each depth's slot is fully consumed (read) before that same slot is
   reused by the next sibling call at that depth. */
static const uint8_t* skip_value_key_ptrs[FEB_CBOR_MAX_NESTING + 1][FEB_CBOR_MAX_MAP_ENTRIES];
static size_t skip_value_key_lens[FEB_CBOR_MAX_NESTING + 1][FEB_CBOR_MAX_MAP_ENTRIES];

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
        const uint8_t** key_ptrs = skip_value_key_ptrs[depth];
        size_t* key_lens = skip_value_key_lens[depth];
        size_t pos = head_len;
        for(size_t i = 0; i < count; i++) {
            if(pos >= in_len || (uint8_t)(in[pos] >> 5) != 3) {
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
    default:
        /* major 1 (negative int), major 6 (tag), and major 7 (true/false/null) are not
           used anywhere in this protocol. */
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
        in + pos, in_len - pos, 2, &record->payload_span, &record->payload_span_len, &status);
    if(n == 0) return status;
    if(record->payload_span_len > FEB_CBOR_MAX_PAYLOAD) {
        return FEB_CBOR_ERR_TOO_LARGE;
    }
    pos += n;

    if(pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
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

    if(pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
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

/* ---- `capability_query` / `capability_response` payloads ---- */

size_t feb_cbor_encode_capability_query_payload(
    uint8_t* out, size_t out_cap, const feb_capability_query_payload_t* payload) {
    if(out == NULL || payload == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, payload->has_requested ? 1u : 0u);
    if(n == 0) return 0;
    pos += n;
    if(payload->has_requested) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "requested", sizeof("requested") - 1);
        if(n == 0) return 0;
        pos += n;
        /* Content intentionally not captured/emitted by this firmware -- see the header
           comment. This firmware never sets has_requested, so this path is unused today. */
        n = feb_cbor_encode_array_header(out + pos, out_cap - pos, 0);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_capability_query_payload(
    const uint8_t* in, size_t in_len, feb_capability_query_payload_t* payload) {
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
    if(count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    if(count == 1) {
        const char* key_data;
        size_t key_len;
        size_t n =
            feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len, FEB_CBOR_MAX_TEXT_LEN, &status);
        if(n == 0) return status;
        if(!text_matches(key_data, key_len, "requested")) {
            return FEB_CBOR_ERR_OUT_OF_ORDER;
        }
        pos += n;

        size_t array_count = 0;
        n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
        if(n == 0) return status;
        if(array_count > FEB_CBOR_MAX_ARRAY_ENTRIES) {
            return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        }
        pos += n;
        for(size_t i = 0; i < array_count; i++) {
            const char* item_data;
            size_t item_len;
            n = feb_cbor_decode_text(
                in + pos, in_len - pos, &item_data, &item_len, FEB_CBOR_MAX_TEXT_LEN, &status);
            if(n == 0) return status;
            pos += n;
        }
        payload->has_requested = 1;
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_capability_response_payload(
    uint8_t* out, size_t out_cap, const feb_capability_response_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->board == NULL || payload->firmware == NULL) {
        return 0;
    }
    if(payload->feature_count > FEB_CAPABILITY_MAX_FEATURES) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 3);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board", sizeof("board") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->board, payload->board_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "firmware", sizeof("firmware") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->firmware, payload->firmware_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "features", sizeof("features") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->feature_count);
    if(n == 0) return 0;
    pos += n;
    for(size_t i = 0; i < payload->feature_count; i++) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->features[i], payload->feature_lens[i]);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_capability_response_payload(
    const uint8_t* in, size_t in_len, feb_capability_response_payload_t* payload) {
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
    if(count < 3) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "board", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->board, &payload->board_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "firmware", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->firmware, &payload->firmware_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "features", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    size_t feature_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &feature_count, &status);
    if(n == 0) return status;
    if(feature_count > FEB_CAPABILITY_MAX_FEATURES) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;
    for(size_t i = 0; i < feature_count; i++) {
        n = feb_cbor_decode_text(
            in + pos,
            in_len - pos,
            &payload->features[i],
            &payload->feature_lens[i],
            FEB_CBOR_MAX_TEXT_LEN,
            &status);
        if(n == 0) return status;
        pos += n;
    }
    payload->feature_count = feature_count;

    return FEB_CBOR_OK;
}

/* ---- `command` / `status` payloads (generic; see header comment) ---- */

size_t feb_cbor_encode_command_payload(uint8_t* out, size_t out_cap, const feb_command_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->capability == NULL || payload->arguments_span == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 3);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "capability", sizeof("capability") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->capability, payload->capability_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", sizeof("request_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "arguments", sizeof("arguments") - 1);
    if(n == 0) return 0;
    pos += n;
    if(out_cap - pos < payload->arguments_span_len) {
        return 0;
    }
    if(payload->arguments_span_len > 0) {
        memcpy(out + pos, payload->arguments_span, payload->arguments_span_len);
    }
    pos += payload->arguments_span_len;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_command_payload(const uint8_t* in, size_t in_len, feb_command_payload_t* payload) {
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
    if(count < 3) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "capability", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->capability, &payload->capability_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "request_id", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->request_id, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "arguments", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    /* Fresh depth-0 budget, not payload_span's depth-2 convention -- per docs/PROTOCOL.md's
       "Nesting depth" section (updated for this step): a field with its own dedicated,
       schema-aware decoder (this one) is its own self-contained span and gets a fresh depth
       budget starting at 0 when recursing into feb_cbor_skip_value() for a still-generically-
       validated sub-piece, rather than inheriting the depth already spent positioning the
       outer payload. This is a decoder-internal bookkeeping convention with no wire
       representation, but both firmwares must apply it identically or one will accept a
       record the other rejects -- see the matching `result` comment in
       feb_cbor_decode_status_payload() below for the full accounting. */
    n = feb_cbor_skip_value(
        in + pos, in_len - pos, 0, &payload->arguments_span, &payload->arguments_span_len, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_status_payload(uint8_t* out, size_t out_cap, const feb_status_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->state == NULL) {
        return 0;
    }
    if(payload->has_result && payload->result_span == NULL) {
        return 0;
    }
    size_t count = payload->has_result ? 3u : 2u;
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", sizeof("request_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "state", sizeof("state") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->state, payload->state_len);
    if(n == 0) return 0;
    pos += n;
    if(payload->has_result) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "result", sizeof("result") - 1);
        if(n == 0) return 0;
        pos += n;
        if(out_cap - pos < payload->result_span_len) {
            return 0;
        }
        if(payload->result_span_len > 0) {
            memcpy(out + pos, payload->result_span, payload->result_span_len);
        }
        pos += payload->result_span_len;
    }
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_status_payload(const uint8_t* in, size_t in_len, feb_status_payload_t* payload) {
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
    if(count > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "request_id", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->request_id, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "state", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->state, &payload->state_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    if(count == 3) {
        n = decode_expected_key(in + pos, in_len - pos, "result", seen_ptrs, seen_lens, 2, &status);
        if(n == 0) return status;
        pos += n;
        /* Fresh depth-0 budget -- per docs/PROTOCOL.md's "Nesting depth" section (updated
           for this step, after this exact gap was found independently on both firmwares
           while implementing against the frozen FEB_VEC_WIFI_SCAN_STATUS_PARTIAL/COMPLETE_
           PAYLOAD vectors): wifi_scan's actual `result` shape (result-map -> aps-array ->
           6-field ap-result-map -> scalar field) is three containers deep in its own right,
           which exceeds FEB_CBOR_MAX_NESTING if it inherits the depth already spent
           positioning `status` itself as "the payload" (payload_span's depth-2 convention,
           or a naive "one level deeper", depth-3). Since this decoder is schema-aware down
           to the <ap-result> field level (unlike a truly opaque field, e.g. `payload` itself
           or capability_query's unused `requested`), `result` is validated as its own
           self-contained span starting fresh at depth 0, not as continued generic descent
           from `status`. This is a decoder-internal bookkeeping convention with no wire
           representation, but must match the ESP32 side's decoder exactly -- confirmed
           against PROTOCOL.md's now-updated wording rather than invented independently. */
        n = feb_cbor_skip_value(
            in + pos, in_len - pos, 0, &payload->result_span, &payload->result_span_len, &status);
        if(n == 0) return status;
        pos += n;
        payload->has_result = 1;
    }

    return FEB_CBOR_OK;
}

/* ---- `wifi_scan` capability payloads ---- */

size_t feb_cbor_encode_wifi_scan_ap(uint8_t* out, size_t out_cap, const feb_wifi_scan_ap_t* ap) {
    if(out == NULL || ap == NULL || ap->phy == NULL || ap->auth == NULL) {
        return 0;
    }
    if(ap->ssid == NULL && ap->ssid_len > 0) {
        return 0;
    }
    if(ap->ssid_len > FEB_WIFI_SCAN_SSID_MAX_LEN || ap->rssi_offset > 255u) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 6);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ssid", sizeof("ssid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ap->ssid, ap->ssid_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "bssid", sizeof("bssid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ap->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ap->rssi_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "channel", sizeof("channel") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ap->channel);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "phy", sizeof("phy") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, ap->phy, ap->phy_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "auth", sizeof("auth") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, ap->auth, ap->auth_len);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

size_t feb_cbor_decode_wifi_scan_ap(
    const uint8_t* in,
    size_t in_len,
    feb_wifi_scan_ap_t* ap,
    feb_cbor_status_t* status) {
    if(in == NULL || ap == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(ap, 0, sizeof(*ap));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 6) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 6) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[6];
    size_t seen_lens[6];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "ssid", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_bytes(
        in + pos, in_len - pos, &ap->ssid, &ap->ssid_len, FEB_WIFI_SCAN_SSID_MAX_LEN, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "bssid", seen_ptrs, seen_lens, 1, status);
    if(n == 0) return 0;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_WIFI_SCAN_BSSID_LEN, status);
        if(n == 0) return 0;
        if(len != FEB_WIFI_SCAN_BSSID_LEN) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        memcpy(ap->bssid, data, FEB_WIFI_SCAN_BSSID_LEN);
        pos += n;
    }

    n = decode_expected_key(in + pos, in_len - pos, "rssi_offset", seen_ptrs, seen_lens, 2, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &ap->rssi_offset, status);
    if(n == 0) return 0;
    if(ap->rssi_offset > 255u) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "channel", seen_ptrs, seen_lens, 3, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &ap->channel, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "phy", seen_ptrs, seen_lens, 4, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(in + pos, in_len - pos, &ap->phy, &ap->phy_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "auth", seen_ptrs, seen_lens, 5, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(in + pos, in_len - pos, &ap->auth, &ap->auth_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wifi_scan_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wifi_scan_result_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->ap_count > FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "aps", sizeof("aps") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->ap_count);
    if(n == 0) return 0;
    pos += n;
    for(size_t i = 0; i < payload->ap_count; i++) {
        n = feb_cbor_encode_wifi_scan_ap(out + pos, out_cap - pos, &payload->aps[i]);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_wifi_scan_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wifi_scan_result_payload_t* payload) {
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
    if(count < 1) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[1];
    size_t seen_lens[1];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "aps", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;

    size_t array_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
    if(n == 0) return status;
    if(array_count > FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;

    for(size_t i = 0; i < array_count; i++) {
        size_t item_len = feb_cbor_decode_wifi_scan_ap(in + pos, in_len - pos, &payload->aps[i], &status);
        if(item_len == 0) {
            return status;
        }
        pos += item_len;
    }
    payload->ap_count = array_count;

    return FEB_CBOR_OK;
}

/* ---- `ble_scan` capability payloads ---- */

size_t feb_cbor_encode_ble_scan_device(uint8_t* out, size_t out_cap, const feb_ble_scan_device_t* device) {
    if(out == NULL || device == NULL || device->addr_type == NULL) {
        return 0;
    }
    if(device->has_name && device->name == NULL) {
        return 0;
    }
    if(device->has_name && device->name_len > FEB_BLE_SCAN_NAME_MAX_LEN) {
        return 0;
    }
    if(device->rssi_offset > 255u) {
        return 0;
    }
    size_t count = device->has_name ? 4u : 3u;
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "address", sizeof("address") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, device->address, FEB_BLE_SCAN_ADDRESS_LEN);
    if(n == 0) return 0;
    pos += n;
    if(device->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", sizeof("name") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, device->name, device->name_len);
        if(n == 0) return 0;
        pos += n;
    }
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, device->rssi_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "addr_type", sizeof("addr_type") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, device->addr_type, device->addr_type_len);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

size_t feb_cbor_decode_ble_scan_device(
    const uint8_t* in,
    size_t in_len,
    feb_ble_scan_device_t* device,
    feb_cbor_status_t* status) {
    if(in == NULL || device == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(device, 0, sizeof(*device));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 3) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 4) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[4];
    size_t seen_lens[4];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "address", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_BLE_SCAN_ADDRESS_LEN, status);
        if(n == 0) return 0;
        if(len != FEB_BLE_SCAN_ADDRESS_LEN) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        memcpy(device->address, data, FEB_BLE_SCAN_ADDRESS_LEN);
        pos += n;
    }

    size_t next_index = 1;
    if(count == 4) {
        n = decode_expected_key(in + pos, in_len - pos, "name", seen_ptrs, seen_lens, next_index, status);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_decode_text(
            in + pos, in_len - pos, &device->name, &device->name_len, FEB_BLE_SCAN_NAME_MAX_LEN, status);
        if(n == 0) return 0;
        pos += n;
        device->has_name = 1;
        next_index++;
    }

    n = decode_expected_key(in + pos, in_len - pos, "rssi_offset", seen_ptrs, seen_lens, next_index, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &device->rssi_offset, status);
    if(n == 0) return 0;
    if(device->rssi_offset > 255u) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    pos += n;
    next_index++;

    n = decode_expected_key(in + pos, in_len - pos, "addr_type", seen_ptrs, seen_lens, next_index, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &device->addr_type, &device->addr_type_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_ble_scan_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_ble_scan_result_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->device_count > FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "devices", sizeof("devices") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->device_count);
    if(n == 0) return 0;
    pos += n;
    for(size_t i = 0; i < payload->device_count; i++) {
        n = feb_cbor_encode_ble_scan_device(out + pos, out_cap - pos, &payload->devices[i]);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_ble_scan_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_ble_scan_result_payload_t* payload) {
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
    if(count < 1) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[1];
    size_t seen_lens[1];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "devices", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;

    size_t array_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
    if(n == 0) return status;
    if(array_count > FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;

    for(size_t i = 0; i < array_count; i++) {
        size_t item_len =
            feb_cbor_decode_ble_scan_device(in + pos, in_len - pos, &payload->devices[i], &status);
        if(item_len == 0) {
            return status;
        }
        pos += item_len;
    }
    payload->device_count = array_count;

    return FEB_CBOR_OK;
}

/* ---- `wardriving` capability payloads ---- */

size_t feb_cbor_encode_wardriving_command_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wardriving_command_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->action == NULL) {
        return 0;
    }
    if(payload->has_sources && payload->source_count > FEB_WARDRIVING_MAX_SOURCES) {
        return 0;
    }
    size_t count = 1u;
    if(payload->has_sources) count++;
    if(payload->has_wifi_interval_ms) count++;
    if(payload->has_ble_params) count += 2u;

    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "action", sizeof("action") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->action, payload->action_len);
    if(n == 0) return 0;
    pos += n;

    if(payload->has_sources) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "sources", sizeof("sources") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->source_count);
        if(n == 0) return 0;
        pos += n;
        for(size_t i = 0; i < payload->source_count; i++) {
            n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->sources[i], payload->source_lens[i]);
            if(n == 0) return 0;
            pos += n;
        }
    }
    if(payload->has_wifi_interval_ms) {
        n = feb_cbor_encode_text(
            out + pos, out_cap - pos, "wifi_interval_ms", sizeof("wifi_interval_ms") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->wifi_interval_ms);
        if(n == 0) return 0;
        pos += n;
    }
    if(payload->has_ble_params) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "ble_window_ms", sizeof("ble_window_ms") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->ble_window_ms);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(
            out + pos, out_cap - pos, "ble_interval_ms", sizeof("ble_interval_ms") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->ble_interval_ms);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_wardriving_command_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_command_payload_t* payload) {
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
    if(count < 1) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 5) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[5];
    size_t seen_lens[5];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "action", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->action, &payload->action_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    if(count == 1) {
        return FEB_CBOR_OK;
    }

    n = decode_expected_key(in + pos, in_len - pos, "sources", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    size_t source_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &source_count, &status);
    if(n == 0) return status;
    if(source_count > FEB_WARDRIVING_MAX_SOURCES) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;
    for(size_t i = 0; i < source_count; i++) {
        n = feb_cbor_decode_text(
            in + pos, in_len - pos, &payload->sources[i], &payload->source_lens[i], FEB_CBOR_MAX_TEXT_LEN, &status);
        if(n == 0) return status;
        pos += n;
    }
    payload->source_count = source_count;
    payload->has_sources = 1;

    size_t remaining = count - 2;
    if(remaining > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    if(remaining == 1 || remaining == 3) {
        n = decode_expected_key(
            in + pos, in_len - pos, "wifi_interval_ms", seen_ptrs, seen_lens, 2, &status);
        if(n == 0) return status;
        pos += n;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->wifi_interval_ms, &status);
        if(n == 0) return status;
        pos += n;
        payload->has_wifi_interval_ms = 1;
    }
    if(remaining == 2 || remaining == 3) {
        size_t idx = (remaining == 3) ? 3 : 2;
        n = decode_expected_key(in + pos, in_len - pos, "ble_window_ms", seen_ptrs, seen_lens, idx, &status);
        if(n == 0) return status;
        pos += n;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->ble_window_ms, &status);
        if(n == 0) return status;
        pos += n;
        n = decode_expected_key(
            in + pos, in_len - pos, "ble_interval_ms", seen_ptrs, seen_lens, idx + 1, &status);
        if(n == 0) return status;
        pos += n;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->ble_interval_ms, &status);
        if(n == 0) return status;
        pos += n;
        payload->has_ble_params = 1;
    }

    if(pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_wardriving_wifi_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wardriving_wifi_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->auth == NULL) {
        return 0;
    }
    if(payload->ssid == NULL && payload->ssid_len > 0) {
        return 0;
    }
    if(payload->ssid_len > FEB_WIFI_SCAN_SSID_MAX_LEN || payload->rssi_offset > 255u) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 5);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ssid", sizeof("ssid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, payload->ssid, payload->ssid_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "bssid", sizeof("bssid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, payload->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->rssi_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "channel", sizeof("channel") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->channel);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "auth", sizeof("auth") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->auth, payload->auth_len);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

size_t feb_cbor_decode_wardriving_wifi_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_wifi_payload_t* payload,
    feb_cbor_status_t* status) {
    if(in == NULL || payload == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(payload, 0, sizeof(*payload));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 5) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 5) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[5];
    size_t seen_lens[5];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "ssid", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_bytes(
        in + pos, in_len - pos, &payload->ssid, &payload->ssid_len, FEB_WIFI_SCAN_SSID_MAX_LEN, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "bssid", seen_ptrs, seen_lens, 1, status);
    if(n == 0) return 0;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_WIFI_SCAN_BSSID_LEN, status);
        if(n == 0) return 0;
        if(len != FEB_WIFI_SCAN_BSSID_LEN) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        memcpy(payload->bssid, data, FEB_WIFI_SCAN_BSSID_LEN);
        pos += n;
    }

    n = decode_expected_key(in + pos, in_len - pos, "rssi_offset", seen_ptrs, seen_lens, 2, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->rssi_offset, status);
    if(n == 0) return 0;
    if(payload->rssi_offset > 255u) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "channel", seen_ptrs, seen_lens, 3, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->channel, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "auth", seen_ptrs, seen_lens, 4, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->auth, &payload->auth_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wardriving_ble_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wardriving_ble_payload_t* payload) {
    if(out == NULL || payload == NULL) {
        return 0;
    }
    if(payload->has_name && payload->name == NULL) {
        return 0;
    }
    if(payload->has_name && payload->name_len > FEB_BLE_SCAN_NAME_MAX_LEN) {
        return 0;
    }
    if(payload->rssi_offset > 255u) {
        return 0;
    }
    size_t count = payload->has_name ? 3u : 2u;
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "address", sizeof("address") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, payload->address, FEB_BLE_SCAN_ADDRESS_LEN);
    if(n == 0) return 0;
    pos += n;
    if(payload->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", sizeof("name") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->name, payload->name_len);
        if(n == 0) return 0;
        pos += n;
    }
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->rssi_offset);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

size_t feb_cbor_decode_wardriving_ble_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_ble_payload_t* payload,
    feb_cbor_status_t* status) {
    if(in == NULL || payload == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(payload, 0, sizeof(*payload));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 2) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 3) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "address", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_BLE_SCAN_ADDRESS_LEN, status);
        if(n == 0) return 0;
        if(len != FEB_BLE_SCAN_ADDRESS_LEN) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        memcpy(payload->address, data, FEB_BLE_SCAN_ADDRESS_LEN);
        pos += n;
    }

    size_t next_index = 1;
    if(count == 3) {
        n = decode_expected_key(in + pos, in_len - pos, "name", seen_ptrs, seen_lens, next_index, status);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_decode_text(
            in + pos, in_len - pos, &payload->name, &payload->name_len, FEB_BLE_SCAN_NAME_MAX_LEN, status);
        if(n == 0) return 0;
        pos += n;
        payload->has_name = 1;
        next_index++;
    }

    n = decode_expected_key(in + pos, in_len - pos, "rssi_offset", seen_ptrs, seen_lens, next_index, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->rssi_offset, status);
    if(n == 0) return 0;
    if(payload->rssi_offset > 255u) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wardriving_record(uint8_t* out, size_t out_cap, const feb_wardriving_record_t* record) {
    if(out == NULL || record == NULL || record->source == NULL) {
        return 0;
    }
    int is_wifi = text_matches(record->source, record->source_len, "wifi");
    int is_ble = text_matches(record->source, record->source_len, "ble");
    if(!is_wifi && !is_ble) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 5);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "timestamp_ms", sizeof("timestamp_ms") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->timestamp_ms);
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
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "source", sizeof("source") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->source, record->source_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", sizeof("payload") - 1);
    if(n == 0) return 0;
    pos += n;
    if(is_wifi) {
        n = feb_cbor_encode_wardriving_wifi_payload(out + pos, out_cap - pos, &record->wifi_payload);
    } else {
        n = feb_cbor_encode_wardriving_ble_payload(out + pos, out_cap - pos, &record->ble_payload);
    }
    if(n == 0) return 0;
    pos += n;
    return pos;
}

size_t feb_cbor_decode_wardriving_record(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_record_t* record,
    feb_cbor_status_t* status) {
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
    if(count < 5) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 5) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[5];
    size_t seen_lens[5];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "timestamp_ms", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->timestamp_ms, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "lat_e7_offset", seen_ptrs, seen_lens, 1, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->lat_e7_offset, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "lon_e7_offset", seen_ptrs, seen_lens, 2, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->lon_e7_offset, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "source", seen_ptrs, seen_lens, 3, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &record->source, &record->source_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "payload", seen_ptrs, seen_lens, 4, status);
    if(n == 0) return 0;
    pos += n;

    if(text_matches(record->source, record->source_len, "wifi")) {
        n = feb_cbor_decode_wardriving_wifi_payload(in + pos, in_len - pos, &record->wifi_payload, status);
    } else if(text_matches(record->source, record->source_len, "ble")) {
        n = feb_cbor_decode_wardriving_ble_payload(in + pos, in_len - pos, &record->ble_payload, status);
    } else {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    if(n == 0) {
        return 0;
    }
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wardriving_status_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wardriving_status_result_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->record_count > FEB_WARDRIVING_MAX_RECORDS_PER_BATCH) {
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
        n = feb_cbor_encode_wardriving_record(out + pos, out_cap - pos, &payload->records[i]);
        if(n == 0) return 0;
        pos += n;
    }
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "backlog_remaining", sizeof("backlog_remaining") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->backlog_remaining);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t feb_cbor_decode_wardriving_status_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wardriving_status_result_payload_t* payload) {
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

    n = decode_expected_key(in + pos, in_len - pos, "records", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;

    size_t array_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
    if(n == 0) return status;
    if(array_count > FEB_WARDRIVING_MAX_RECORDS_PER_BATCH) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;
    for(size_t i = 0; i < array_count; i++) {
        size_t item_len =
            feb_cbor_decode_wardriving_record(in + pos, in_len - pos, &payload->records[i], &status);
        if(item_len == 0) {
            return status;
        }
        pos += item_len;
    }
    payload->record_count = array_count;

    n = decode_expected_key(in + pos, in_len - pos, "backlog_remaining", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->backlog_remaining, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

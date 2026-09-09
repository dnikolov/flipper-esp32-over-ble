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

/* Internal codec plumbing shared across the split cbor_*.c translation units (primitives,
   records, wifi_scan, ble_scan, wardriving). NOT part of the cross-firmware shared-header
   contract that tools/check_shared_headers.py diffs against the ESP32 copy -- this is purely
   a local implementation-sharing seam, so it does not need to match the ESP32 side
   byte-for-byte. Functions are `static inline` so this header can be included by any of the
   split .c files without a separate cbor_internal.c/build-system entry. */
#ifndef FEB_CBOR_INTERNAL_H
#define FEB_CBOR_INTERNAL_H

#include "cbor_codec.h"

#include <string.h>

/* Returns nonzero if `data` (length `len`) matches `literal` byte-for-byte. */
static inline int feb_cbor_i_text_matches(const char* data, size_t len, const char* literal) {
    size_t literal_len = strlen(literal);
    return len == literal_len && memcmp(data, literal, literal_len) == 0;
}

/* Decodes one map key (text) at *pos, rejecting a key that duplicates one already seen in
   `seen_ptrs`/`seen_lens` (first `seen_count` entries) or that does not match `expected`.
   Shared by every fixed-field-order map decoder across the split translation units. */
static inline size_t feb_cbor_i_decode_expected_key(
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
    if(!feb_cbor_i_text_matches(key_data, key_len, expected)) {
        *status = FEB_CBOR_ERR_OUT_OF_ORDER;
        return 0;
    }
    seen_ptrs[seen_count] = (const uint8_t*)key_data;
    seen_lens[seen_count] = key_len;
    *status = FEB_CBOR_OK;
    return n;
}

#endif /* FEB_CBOR_INTERNAL_H */

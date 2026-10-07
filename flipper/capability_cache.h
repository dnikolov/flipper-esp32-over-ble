/* Capability-cache lookup (docs/CAPABILITIES.md "Storage and persistence"), kept as a pure
   header so tests/flipper can exercise the exact decision capability_bootstrap() makes.

   A cache file is only "a cache" if it loads AND decodes. Missing, empty, unreadable, oversized
   or malformed all mean "no cache" -> the caller re-sends capability_query, whose response
   overwrites the bad file. (Before 2026-10-07 an existing-but-unusable file made
   capability_bootstrap() return silently, so no capability menu rows ever appeared again.) */
#ifndef FEB_CAPABILITY_CACHE_H
#define FEB_CAPABILITY_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cbor_codec.h"

typedef bool (*feb_capability_cache_load_fn)(void* ctx, uint8_t* out, size_t out_cap, size_t* out_len);

/* Returns true only when `out` holds a successfully decoded cached record. `buf` is the scratch
   the loader fills (callers pass FEB_CBOR_MAX_PAYLOAD bytes). */
static inline bool feb_capability_cache_resolve(
    feb_capability_cache_load_fn load,
    void* ctx,
    uint8_t* buf,
    size_t buf_cap,
    feb_capability_response_payload_t* out) {
    size_t len = 0;
    if(!load(ctx, buf, buf_cap, &len) || len == 0 || len > buf_cap) {
        return false;
    }
    return feb_cbor_decode_capability_response_payload(buf, len, out) == FEB_CBOR_OK;
}

#endif

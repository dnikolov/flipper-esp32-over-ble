/* Shared contract, mirrored byte-for-byte in flipper/cbor_codec.h. Changes here must be
   mirrored there and in docs/PROTOCOL.md, or the two firmwares diverge.

   Umbrella header (docs/OPTIMIZATION.md item 1, split 2026-09-08; cbor_gps.h added
   2026-09-12; cbor_meshcore.h added 2026-09-26, Heltec-only `meshcore_scan` capability): the
   codec implementation is split per capability into cbor_primitives.c/.h, cbor_records.c/.h,
   cbor_wifi_scan.c/.h, cbor_ble_scan.c/.h, cbor_wardriving.c/.h, cbor_gps.c/.h,
   cbor_meshcore.c/.h -- this file now holds only the macros/typedef genuinely shared across
   every one of those (the CBOR status enum and the generic length/nesting bounds), then
   #includes the six split headers so every declaration is still reachable through
   `#include "cbor_codec.h"` exactly as before the split. See each split header's own top
   comment for its scope; the split headers' comments preserve the original
   section-boundary documentation (nesting-depth rationale, field-order notes, etc.)
   verbatim from before the split.

   Scope note (docs/PLAN.md step 3): cbor_primitives.h implements the generic canonical-CBOR
   primitives; cbor_records.h implements the two fixed outer envelope shapes from
   docs/PROTOCOL.md#record-format, the self-contained `error` payload (used as the on-device
   smoke-test payload for step 3), the capability_query/capability_response payloads (step
   7), and the generic command/status payloads (Phase 3 wifi_scan-command step).
   cbor_wifi_scan.h/cbor_ble_scan.h/cbor_wardriving.h/cbor_gps.h implement their respective
   capability-specific payload shapes. None of these implement hello/pair_* (those live in
   session.h/pairing.h). */
#ifndef FEB_CBOR_CODEC_H
#define FEB_CBOR_CODEC_H

#include <stdint.h>
#include <stddef.h>

#define FEB_CBOR_MAX_PAYLOAD 512u   /* plaintext `payload` map, its own CBOR encoding */
#define FEB_CBOR_MAX_NESTING 4u     /* outer map -> payload map -> array -> element */
#define FEB_CBOR_MAX_MAP_ENTRIES 8u /* largest fixed map defined in PROTOCOL.md today */
#define FEB_CBOR_MAX_ARRAY_ENTRIES 32u
#define FEB_CBOR_MAX_TEXT_LEN 64u
#define FEB_CBOR_MAX_BYTES_LEN 512u /* bounds `ciphertext`; must be >= FEB_CBOR_MAX_PAYLOAD
                                       since GCM ciphertext is plaintext-length regardless
                                       of key size; exact-length fields (nonces/keys/tags)
                                       override */

#define FEB_SESSION_ID_LEN 8u
#define FEB_GCM_TAG_LEN 16u

typedef enum {
    FEB_CBOR_OK = 0,
    FEB_CBOR_ERR_TOO_LARGE,        /* -> error.code "payload_too_large" */
    FEB_CBOR_ERR_DUPLICATE_KEY,    /* -> error.code "malformed_record" */
    FEB_CBOR_ERR_OUT_OF_ORDER,     /* -> error.code "malformed_record" */
    FEB_CBOR_ERR_MISSING_FIELD,    /* -> error.code "malformed_record" */
    FEB_CBOR_ERR_UNEXPECTED_TYPE,  /* -> error.code "malformed_record" */
    FEB_CBOR_ERR_INDEFINITE_LENGTH,/* -> error.code "malformed_record" */
    FEB_CBOR_ERR_TRUNCATED,        /* -> error.code "malformed_record" */
    FEB_CBOR_ERR_TOO_DEEP,         /* -> error.code "malformed_record" */
    FEB_CBOR_ERR_TOO_MANY_ENTRIES, /* -> error.code "malformed_record" */
    FEB_CBOR_ERR_NON_CANONICAL,    /* not shortest-form integer/length encoding */
    FEB_CBOR_ERR_AUTH_FAILED,      /* AES-256-GCM tag did not verify (session.h, step 6) --
                                       fatal per docs/PROTOCOL.md: discard the record and
                                       close the BLE connection without replying; never a
                                       wire error.code, never exposed to the peer */
} feb_cbor_status_t;

#include "cbor_primitives.h"
#include "cbor_records.h"
#include "cbor_wifi_scan.h"
#include "cbor_ble_scan.h"
#include "cbor_wardriving.h"
#include "cbor_gps.h"
#include "cbor_meshcore.h"

#endif /* FEB_CBOR_CODEC_H */

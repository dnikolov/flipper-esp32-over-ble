/* Shared contract, mirrored byte-for-byte in flipper/cbor_codec.h. Changes here must be
   mirrored there and in docs/PROTOCOL.md, or the two firmwares diverge.

   Scope note (docs/PLAN.md step 3): this module implements the generic canonical-CBOR
   primitives and the two fixed outer envelope shapes from docs/PROTOCOL.md#record-format,
   plus the self-contained `error` payload (docs/PROTOCOL.md#message-payloads) — which
   needs no pairing or session state and is used as the on-device smoke-test payload for
   this step. Step 7 (docs/PLAN.md) added the `capability_query`/`capability_response`
   payload schemas below. The Phase 3 `wifi_scan`-command step (docs/PLAN.md) added the
   generic `command`/`status` payload schemas (capability-agnostic; `arguments`/`result`
   stay opaque CBOR-map spans captured via feb_cbor_skip_value(), same treatment
   `capability_query`'s `requested` field got) plus the `wifi_scan`-specific `<ap-result>`
   element and `result` map shapes. The `ble_scan`/`wardriving` step (docs/PROTOCOL.md
   "`ble_scan` command and status payloads" / "`wardriving` command and status payloads",
   frozen wire spec) added `<device-result>`/`ble_scan` result shapes and the `wardriving`
   command/status/`<wardriving-record>` shapes -- wire-format codec layer only, no
   dispatch/UI. The `gps` capability (docs/PROTOCOL.md "`gps` command and status payloads",
   frozen 2026-09-12) added the flat `status.result` shape in cbor_gps.h, and
   `<wardriving-record>` gained a new `utc_timestamp_s` field the same day (see
   cbor_wardriving.h). It does NOT implement hello/pair_* (those live in session.h/pairing.h).

   This is a thin umbrella header (split 2026-09-08 per docs/OPTIMIZATION.md item 1): the
   generic primitives/envelope/payload declarations live in the five included headers below,
   one per capability/section. Nothing that includes this header needs to change -- every
   declaration that used to live here directly is still visible transitively. Only the macros
   and typedefs genuinely shared across every section (not owned by any one capability) stay
   defined directly in this file. */
#ifndef FEB_CBOR_CODEC_H
#define FEB_CBOR_CODEC_H

#include <stdint.h>
#include <stddef.h>

#define FEB_CBOR_MAX_PAYLOAD 512u   /* plaintext `payload` map, its own CBOR encoding */
#define FEB_CBOR_MAX_NESTING 4u     /* outer map -> payload map -> array -> element */
#define FEB_CBOR_MAX_MAP_ENTRIES 8u /* largest fixed map defined in PROTOCOL.md today */
#define FEB_CBOR_MAX_ARRAY_ENTRIES 32u
#define FEB_CBOR_MAX_TEXT_LEN 64u
#define FEB_CBOR_MAX_BYTES_LEN 512u /* bounds `ciphertext`; GCM ciphertext is
                                       plaintext-length regardless of key size, so this
                                       must match FEB_CBOR_MAX_PAYLOAD, not be
                                       independently sized; exact-length fields override */

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

/* Include order matters: wifi_scan/ble_scan must precede wardriving, since wardriving.h's
   own length-limit macros alias the FEB_WIFI_SCAN_ / FEB_BLE_SCAN_ ones defined there. */
#include "cbor_primitives.h"
#include "cbor_records.h"
#include "cbor_wifi_scan.h"
#include "cbor_ble_scan.h"
#include "cbor_wardriving.h"
#include "cbor_gps.h"

#endif /* FEB_CBOR_CODEC_H */

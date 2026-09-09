/* Split out of cbor_codec.h (docs/OPTIMIZATION.md item 1) -- shared contract, mirrored
   byte-for-byte in flipper/cbor_records.h. Changes here must be mirrored there and in
   docs/PROTOCOL.md, or the two firmwares diverge. Included transitively via cbor_codec.h;
   nothing outside the codec split should need to include this directly.

   Scope: the two fixed outer envelope shapes from docs/PROTOCOL.md#record-format, the
   self-contained `error` payload, the `capability_query`/`capability_response` payloads
   (docs/PLAN.md step 7), and the generic capability-agnostic `command`/`status` payloads
   (docs/PLAN.md's Phase 3 wifi_scan-command step; `arguments`/`result` stay opaque CBOR-map
   spans captured via feb_cbor_skip_value(), same treatment `capability_query`'s `requested`
   field got). Does NOT implement hello/pair_* (those live in session.h/pairing.h), and does
   NOT implement any capability-specific payload shape (those live in cbor_wifi_scan.h/
   cbor_ble_scan.h/cbor_wardriving.h). */
#ifndef FEB_CBOR_RECORDS_H
#define FEB_CBOR_RECORDS_H

#include "cbor_codec.h"

/* ---- Envelope shapes (docs/PROTOCOL.md#record-format) ----
   Field order below is the canonical order — see
   docs/PROTOCOL.md#canonical-cbor-encoding-definition. Encoders emit exactly this order;
   decoders reject any other order in addition to duplicate keys and indefinite length. */

typedef struct {
    uint32_t version;
    const char *type;
    size_t type_len;
    uint8_t session_id[FEB_SESSION_ID_LEN];
    const char *board_id;
    size_t board_id_len;
    const uint8_t *payload_span; /* raw CBOR bytes of the payload map, opaque at this layer */
    size_t payload_span_len;
} feb_unencrypted_record_t;

typedef struct {
    uint32_t version;
    const char *type;
    size_t type_len;
    uint8_t session_id[FEB_SESSION_ID_LEN];
    const char *board_id;
    size_t board_id_len;
    uint64_t sequence;
    const uint8_t *ciphertext;
    size_t ciphertext_len;
    uint8_t tag[FEB_GCM_TAG_LEN];
} feb_protected_record_t;

/* Encodes into `out` (capacity `out_cap`); returns bytes written, or 0 on failure
   (oversized output, or record->payload_span_len > FEB_CBOR_MAX_PAYLOAD). */
size_t feb_cbor_encode_unencrypted(uint8_t *out, size_t out_cap, const feb_unencrypted_record_t *record);
size_t feb_cbor_encode_protected(uint8_t *out, size_t out_cap, const feb_protected_record_t *record);

/* Decodes a full reassembled record buffer (as produced by framing.h) into `record`,
   whose bytes/text/payload_span pointers alias `in` (valid only as long as `in` is).
   Returns FEB_CBOR_OK, or a feb_cbor_status_t failure. Rejects a payload_span_len
   exceeding FEB_CBOR_MAX_PAYLOAD with FEB_CBOR_ERR_TOO_LARGE. */
feb_cbor_status_t feb_cbor_decode_unencrypted(const uint8_t *in, size_t in_len, feb_unencrypted_record_t *record);
feb_cbor_status_t feb_cbor_decode_protected(const uint8_t *in, size_t in_len, feb_protected_record_t *record);

/* ---- `error` payload (docs/PROTOCOL.md#message-payloads) ----
   Self-contained; the only payload type this module knows the specific fields of. Used
   as the payload of a feb_unencrypted_record_t with type "error" for this step's
   on-device smoke test. Field order: code, message, request_id (message/request_id
   optional — set has_message/has_request_id to include them). */
typedef struct {
    const char *code;
    size_t code_len;
    const char *message;   /* NULL if absent */
    size_t message_len;
    int has_message;
    uint64_t request_id;
    int has_request_id;
} feb_error_payload_t;

size_t feb_cbor_encode_error_payload(uint8_t *out, size_t out_cap, const feb_error_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_error_payload(const uint8_t *in, size_t in_len, feb_error_payload_t *payload);

/* ---- `capability_query` / `capability_response` payloads
   (docs/PROTOCOL.md#message-payloads, docs/PLAN.md step 7) ----
   `capability_query` (Flipper -> ESP32): `requested` is optional and, per docs/PROTOCOL.md's
   "Notes on capability discovery", its content is deliberately never used by either
   firmware today -- the ESP32 always returns the full registry regardless of what's sent
   or omitted. Decoding still validates the field's shape (array of text strings) when
   present and rejects anything else; the array's own content is discarded, not captured.
   `capability_response` (ESP32 -> Flipper): `board`/`firmware` are opaque hand-maintained
   constant strings, never validated. `features` is bounded by FEB_CAPABILITY_MAX_FEATURES
   (== FEB_CBOR_MAX_ARRAY_ENTRIES, the same generic array bound used elsewhere in this
   file). Field order: board, firmware, features. */
#define FEB_CAPABILITY_MAX_FEATURES FEB_CBOR_MAX_ARRAY_ENTRIES

typedef struct {
    int has_requested; /* content intentionally not captured; see comment above */
} feb_capability_query_payload_t;

size_t feb_cbor_encode_capability_query_payload(uint8_t *out, size_t out_cap, const feb_capability_query_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_capability_query_payload(const uint8_t *in, size_t in_len, feb_capability_query_payload_t *payload);

typedef struct {
    const char *board;
    size_t board_len;
    const char *firmware;
    size_t firmware_len;
    const char *features[FEB_CAPABILITY_MAX_FEATURES];
    size_t feature_lens[FEB_CAPABILITY_MAX_FEATURES];
    size_t feature_count;
} feb_capability_response_payload_t;

size_t feb_cbor_encode_capability_response_payload(uint8_t *out, size_t out_cap, const feb_capability_response_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_capability_response_payload(const uint8_t *in, size_t in_len, feb_capability_response_payload_t *payload);

/* ---- `command` / `status` payloads (docs/PROTOCOL.md#message-payloads, docs/PLAN.md
   "Wi-Fi scan capability" step) ----
   Generic across every capability, not wifi_scan-specific: `arguments` (command) and
   `result` (status, optional) are opaque CBOR-map spans, validated for well-formedness and
   captured via feb_cbor_skip_value() -- this layer only confirms each is a map (major type
   5) and structurally sound; the capability-specific schema inside is a caller (main.c)
   concern, same split as capability_query's `requested` field above.

   Depth budget (real bug found and fixed while implementing the wifi_scan status.result
   shape against this header, 2026-09-07): `arguments`/`result` are each validated with
   their OWN fresh feb_cbor_skip_value() nesting budget (depth starts at 0), not the depth=2
   `feb_cbor_decode_unencrypted` passes for the top-level `payload` field. Reusing depth=2
   here (mirroring `payload`'s own call site literally) makes wifi_scan's own frozen
   `status` shape undecodable: `result` -> `aps` (array) -> `<ap-result>` (map) -> its own
   scalar fields is 3 real container levels below `result` itself, and starting from depth=2
   leaves only 2 levels of FEB_CBOR_MAX_NESTING (4) headroom -- the innermost ap-result
   fields get checked at depth 5 and are rejected as FEB_CBOR_ERR_TOO_DEEP, confirmed by a
   failing host-native test against FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD /
   STATUS_COMPLETE_PAYLOAD before this fix. Since the depth counter is a pure internal
   recursion-budget implementation detail (it has no wire representation), each opaque
   "second-level payload" field (`arguments`/`result`) getting its own fresh
   FEB_CBOR_MAX_NESTING budget -- the same policy `payload` itself gets relative to the
   outer record -- is the correct generalization, not a special case for wifi_scan. This
   should be promoted into docs/PROTOCOL.md's "Nesting depth" section (currently only
   describes the single outer-record/payload relationship) and confirmed identically on the
   Flipper side, since an unmodified depth=2 call there would hit the same rejection against
   the same frozen vectors. Field order: command =
   capability, request_id, arguments (arguments always present, itself may be an empty map);
   status = request_id, state, result (result optional per docs/PROTOCOL.md's message-payload
   table -- every wifi_scan status sets it, so has_result mirrors error payload's optional-field
   pattern). */
typedef struct {
    const char *capability;
    size_t capability_len;
    uint64_t request_id;
    const uint8_t *arguments_span; /* raw CBOR bytes of the arguments map, opaque at this layer */
    size_t arguments_span_len;
} feb_command_payload_t;

size_t feb_cbor_encode_command_payload(uint8_t *out, size_t out_cap, const feb_command_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_command_payload(const uint8_t *in, size_t in_len, feb_command_payload_t *payload);

typedef struct {
    uint64_t request_id;
    const char *state;
    size_t state_len;
    const uint8_t *result_span; /* raw CBOR bytes of the result map, opaque at this layer;
                                    only meaningful when has_result is set */
    size_t result_span_len;
    int has_result;
} feb_status_payload_t;

size_t feb_cbor_encode_status_payload(uint8_t *out, size_t out_cap, const feb_status_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_status_payload(const uint8_t *in, size_t in_len, feb_status_payload_t *payload);

#endif /* FEB_CBOR_RECORDS_H */

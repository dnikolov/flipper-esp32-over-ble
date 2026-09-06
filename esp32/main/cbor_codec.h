/* Shared contract, mirrored byte-for-byte in flipper/cbor_codec.h. Changes here must be
   mirrored there and in docs/PROTOCOL.md, or the two firmwares diverge.

   Scope note (docs/PLAN.md step 3): this module implements the generic canonical-CBOR
   primitives and the two fixed outer envelope shapes from docs/PROTOCOL.md#record-format,
   plus the self-contained `error` payload (docs/PROTOCOL.md#message-payloads) — which
   needs no pairing or session state and is used as the on-device smoke-test payload for
   this step. It does NOT implement hello, pair_*, capability_*, command, or status payload
   schemas; those are defined and implemented in steps 5, 6, and 7 respectively, reusing
   feb_cbor_skip_value() below to validate/capture the opaque `payload` map generically
   until each step adds its own field-order table for the payload's specific type. */
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

/* ---- Primitives: canonical encode ----
   All encoders write shortest-form CBOR (definite length, minimal-size integers) and
   return the number of bytes written, or 0 if `out` (size `out_cap`) is too small. */
size_t feb_cbor_encode_uint(uint8_t *out, size_t out_cap, uint64_t value);
size_t feb_cbor_encode_bytes(uint8_t *out, size_t out_cap, const uint8_t *data, size_t len);
size_t feb_cbor_encode_text(uint8_t *out, size_t out_cap, const char *data, size_t len);
/* Writes only the definite-length map/array header (major type 5 / 4); caller then
   writes exactly `count` map-entries-worth (2*count items) or `count` array items. */
size_t feb_cbor_encode_map_header(uint8_t *out, size_t out_cap, size_t count);
size_t feb_cbor_encode_array_header(uint8_t *out, size_t out_cap, size_t count);

/* ---- Primitives: canonical decode ----
   Each decoder reads one value starting at `in[0]`, validates it is definite-length
   canonical CBOR, and returns the number of bytes consumed, or 0 on any
   feb_cbor_status_t failure (written to *status). */
size_t feb_cbor_decode_uint(const uint8_t *in, size_t in_len, uint64_t *value, feb_cbor_status_t *status);
size_t feb_cbor_decode_bytes(const uint8_t *in, size_t in_len, const uint8_t **data, size_t *len, size_t max_len, feb_cbor_status_t *status);
size_t feb_cbor_decode_text(const uint8_t *in, size_t in_len, const char **data, size_t *len, size_t max_len, feb_cbor_status_t *status);
size_t feb_cbor_decode_map_header(const uint8_t *in, size_t in_len, size_t *count, feb_cbor_status_t *status);
size_t feb_cbor_decode_array_header(const uint8_t *in, size_t in_len, size_t *count, feb_cbor_status_t *status);

/* Validates one well-formed canonical CBOR value of any type at `in[0]` — used to
   structurally validate (and, via out_span/out_span_len, capture the raw bytes of) an
   opaque `payload` map whose per-type field schema isn't known at this layer: definite
   lengths only, map keys are unique text strings (compared as raw bytes, order not
   checked since the schema is unknown here), nesting bounded by FEB_CBOR_MAX_NESTING,
   map/array entry counts bounded by FEB_CBOR_MAX_MAP_ENTRIES/FEB_CBOR_MAX_ARRAY_ENTRIES.
   Returns bytes consumed, or 0 on failure (written to *status). */
size_t feb_cbor_skip_value(
    const uint8_t *in,
    size_t in_len,
    size_t depth,
    const uint8_t **out_span,
    size_t *out_span_len,
    feb_cbor_status_t *status);

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

#endif /* FEB_CBOR_CODEC_H */

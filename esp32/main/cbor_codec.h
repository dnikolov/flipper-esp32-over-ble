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
   element and `result` map shapes. It does NOT implement hello/pair_* (those live in
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

/* ---- `wifi_scan`-specific `<ap-result>` element and `result` map
   (docs/PROTOCOL.md "`wifi_scan` command and status payloads") ----
   Field order per `<ap-result>`: ssid, bssid, rssi_offset, channel, phy, auth -- matches
   PROTOCOL.md's table exactly. `rssi_offset` is `rssi_dbm + 128` (an unsigned 0-255 value)
   per PROTOCOL.md's canonical-CBOR rule against negative integers in payload maps; callers
   convert to/from a real signed dBm value themselves. `phy`/`auth` are caller-owned text
   (main.c holds the board-specific wifi_auth_mode_t / PHY-generation string tables; this
   module only encodes/decodes whatever text it's given). `ssid` aliases the input on decode,
   same convention as every other variable-length byte/text field in this header; `bssid` is
   copied since it's exact-length, matching `session_id`'s treatment above.

   `feb_wifi_scan_ap_t` is encoded/decoded as an array *element* (like `features` above), not
   a standalone payload -- feb_cbor_decode_wifi_scan_ap() therefore follows the primitive
   decoder convention (returns bytes consumed via feb_cbor_decode_uint/_bytes/_text above,
   not the feb_cbor_status_t-returning "whole exact payload span" convention used by
   `command`/`status`/`capability_*` above), since the caller must know how far to advance
   within the `aps` array. */
#define FEB_WIFI_SCAN_SSID_MAX_LEN 32u
#define FEB_WIFI_SCAN_BSSID_LEN 6u
/* Per-status-record `aps[]` bound; reuses the same generic array cap used elsewhere in this
   file. PROTOCOL.md's 32-total-APs-per-scan cap is a separate ESP32-side scan-result-selection
   concern (main.c), not a codec-layer bound -- it happens to be the same number today. */
#define FEB_WIFI_SCAN_MAX_APS_PER_RECORD FEB_CBOR_MAX_ARRAY_ENTRIES

typedef struct {
    const uint8_t *ssid; /* 0..FEB_WIFI_SCAN_SSID_MAX_LEN bytes; not guaranteed valid UTF-8 */
    size_t ssid_len;
    uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN];
    uint64_t rssi_offset; /* rssi_dbm + 128; encoder/decoder reject a value > 255 */
    uint64_t channel;
    const char *phy;
    size_t phy_len;
    const char *auth;
    size_t auth_len;
} feb_wifi_scan_ap_t;

size_t feb_cbor_encode_wifi_scan_ap(uint8_t *out, size_t out_cap, const feb_wifi_scan_ap_t *ap);
size_t feb_cbor_decode_wifi_scan_ap(const uint8_t *in, size_t in_len, feb_wifi_scan_ap_t *ap, feb_cbor_status_t *status);

typedef struct {
    feb_wifi_scan_ap_t aps[FEB_WIFI_SCAN_MAX_APS_PER_RECORD];
    size_t ap_count;
} feb_wifi_scan_result_payload_t;

size_t feb_cbor_encode_wifi_scan_result_payload(uint8_t *out, size_t out_cap, const feb_wifi_scan_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_wifi_scan_result_payload(const uint8_t *in, size_t in_len, feb_wifi_scan_result_payload_t *payload);

#endif /* FEB_CBOR_CODEC_H */

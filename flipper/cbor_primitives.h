/* Split out of cbor_codec.h 2026-09-08 (docs/OPTIMIZATION.md item 1) -- included by the
   umbrella cbor_codec.h, which must be included first (directly or transitively) so that
   feb_cbor_status_t and the FEB_CBOR_MAX_* macros this header uses are already visible; not
   meant to be included standalone. Not part of the cross-firmware shared-header contract
   check (tools/check_shared_headers.py diffs cbor_codec.h itself, which still transitively
   provides everything declared here).

   Canonical-CBOR primitives: uint/bytes/text/map-header/array-header encode+decode, plus the
   generic structural validator feb_cbor_skip_value(). No knowledge of any specific payload
   shape lives here -- every other split header builds on these. */
#ifndef FEB_CBOR_PRIMITIVES_H
#define FEB_CBOR_PRIMITIVES_H

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

#endif /* FEB_CBOR_PRIMITIVES_H */

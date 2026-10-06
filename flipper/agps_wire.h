#pragma once

/* `agps.load` wire shape -- the ONLY place that knows its CBOR field names/tags, so aligning
   with docs/PROTOCOL.md's agps section is a one-file edit. Header-only/static: included into
   the unity build via app_internal.h and compiled directly by tests/flipper/test_agps.c
   (which links cbor_primitives.c). Requires cbor_codec.h to have been included first.

   docs/PROTOCOL.md "`agps` command and status payloads". Request `arguments`, fixed field
   order: {"action": "load", "utc": uint, "total": uint, "off": uint, "data": bytes}, one chunk
   per command (position triple omitted in v1). Every accepted chunk is answered with `status`
   state "partial" (no result) and the next chunk is sent only after it; after the final
   chunk's partial one unsolicited `status` state "complete" carries
   result {"sent","acked","nacked","timeout": uint}, same request_id as the final chunk. */

#define AGPS_WIRE_CAPABILITY "agps"
#define AGPS_WIRE_STATE_PARTIAL "partial"
#define AGPS_WIRE_STATE_COMPLETE "complete"
/* PROTOCOL.md: UTC must be in [1700000000, 4000000000); an unset RTC fails there. */
#define AGPS_WIRE_UTC_MIN 1700000000u
#define AGPS_WIRE_UTC_MAX 4000000000u
#define AGPS_WIRE_CHUNK_MAX 128u

typedef struct {
    uint16_t sent;
    uint16_t acked;
    uint16_t nacked;
    uint16_t timeout;
} AgpsWireResult;

static size_t agps_wire_encode_chunk_arguments(
    uint8_t* out,
    size_t out_cap,
    uint32_t utc,
    uint16_t total,
    uint16_t off,
    const uint8_t* data,
    size_t data_len) {
    size_t pos = 0;
    size_t n = feb_cbor_encode_map_header(out, out_cap, 5);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "action", 6);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "load", 4);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "utc", 3);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, utc);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "total", 5);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, total);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "off", 3);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, off);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "data", 4);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, data, data_len);
    if(n == 0) return 0;
    return pos + n;
}

/* Unknown keys are skipped; a missing counter is an error (never guessed as 0). */
static bool agps_wire_decode_result(const uint8_t* in, size_t in_len, AgpsWireResult* out) {
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) return false;
    uint8_t seen = 0;
    memset(out, 0, sizeof(*out));
    for(size_t i = 0; i < count; i++) {
        const char* key = NULL;
        size_t key_len = 0;
        size_t n = feb_cbor_decode_text(
            in + pos, in_len - pos, &key, &key_len, FEB_CBOR_MAX_TEXT_LEN, &status);
        if(n == 0) return false;
        pos += n;
        uint16_t* target = NULL;
        uint8_t bit = 0;
        if(key_len == 4 && memcmp(key, "sent", 4) == 0) {
            target = &out->sent;
            bit = 1;
        } else if(key_len == 5 && memcmp(key, "acked", 5) == 0) {
            target = &out->acked;
            bit = 2;
        } else if(key_len == 6 && memcmp(key, "nacked", 6) == 0) {
            target = &out->nacked;
            bit = 4;
        } else if(key_len == 7 && memcmp(key, "timeout", 7) == 0) {
            target = &out->timeout;
            bit = 8;
        }
        if(target != NULL) {
            uint64_t value = 0;
            n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);
            if(n == 0 || value > 0xFFFFu || (seen & bit) != 0) return false;
            *target = (uint16_t)value;
            seen |= bit;
        } else {
            n = feb_cbor_skip_value(in + pos, in_len - pos, 0, NULL, NULL, &status);
            if(n == 0) return false;
        }
        pos += n;
    }
    return seen == 0x0Fu && pos == in_len;
}

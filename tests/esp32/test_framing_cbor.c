/* Host-native test driver for esp32/main/framing.c + esp32/main/cbor_codec.c, compiled
   directly (not copies) against the shared vectors in tests/vectors/vectors.h. See
   docs/PLAN.md step 3 "Validation gate is host-native, not on-device". */
#include <stdio.h>
#include <string.h>

#include "framing.h"
#include "cbor_codec.h"
#include "vectors.h"

static int g_failures = 0;

static void check(int condition, const char *name)
{
    if (condition) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        g_failures++;
    }
}

static int bytes_eq(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len)
{
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

#define FRAG_CAPTURE_MAX_FRAGS 32
#define FRAG_CAPTURE_MAX_LEN 256

typedef struct {
    uint8_t data[FRAG_CAPTURE_MAX_FRAGS][FRAG_CAPTURE_MAX_LEN];
    size_t len[FRAG_CAPTURE_MAX_FRAGS];
    size_t count;
} frag_capture_t;

static void capture_emit(const uint8_t *fragment, size_t fragment_len, void *ctx)
{
    frag_capture_t *fc = (frag_capture_t *)ctx;

    if (fc->count < FRAG_CAPTURE_MAX_FRAGS && fragment_len <= FRAG_CAPTURE_MAX_LEN) {
        memcpy(fc->data[fc->count], fragment, fragment_len);
        fc->len[fc->count] = fragment_len;
        fc->count++;
    }
}

static void test_fragment_record(uint16_t mtu, const uint8_t *const *expected_frags,
                                  const size_t *expected_lens, size_t expected_count,
                                  const char *name)
{
    frag_capture_t fc;
    size_t capacity = feb_fragment_capacity(mtu);
    uint8_t count;
    int ok;
    size_t i;

    memset(&fc, 0, sizeof(fc));
    count = feb_fragment_record(FEB_VEC_RECORD, FEB_VEC_RECORD_LEN, capacity, 7,
                                 capture_emit, &fc);
    ok = (count == expected_count) && (fc.count == expected_count);
    if (ok) {
        for (i = 0; i < fc.count; i++) {
            if (!bytes_eq(fc.data[i], fc.len[i], expected_frags[i], expected_lens[i])) {
                ok = 0;
            }
        }
    }
    check(ok, name);
}

static void test_reassemble(const uint8_t *const *frags, const size_t *lens, size_t count,
                             const char *name)
{
    feb_reassembly_t r;
    feb_frame_status_t st = FEB_FRAME_OK;
    const uint8_t *out_record = NULL;
    size_t out_len = 0;
    size_t i;

    feb_reassembly_reset(&r);
    for (i = 0; i < count; i++) {
        st = feb_reassembly_feed(&r, frags[i], lens[i], 0, &out_record, &out_len);
    }
    check(st == FEB_FRAME_MESSAGE_COMPLETE &&
              bytes_eq(out_record, out_len, FEB_VEC_RECORD, FEB_VEC_RECORD_LEN),
          name);
}

static int run_malformed_fragments(const uint8_t *const *frags, const size_t *lens,
                                    size_t count, feb_frame_status_t expected)
{
    feb_reassembly_t r;
    feb_frame_status_t st = FEB_FRAME_OK;
    const uint8_t *out_record = NULL;
    size_t out_len = 0;
    size_t i;

    feb_reassembly_reset(&r);
    for (i = 0; i < count; i++) {
        st = feb_reassembly_feed(&r, frags[i], lens[i], 0, &out_record, &out_len);
        if (st != FEB_FRAME_OK) {
            /* Stop at the first non-OK status, matching real usage: a caller does not
               keep force-feeding a message's remaining fragments into a reassembly
               buffer that feed() already rejected and reset (drop-and-continue per
               docs/PLAN.md step 3), since any later fragment for that same
               already-dropped message_id would itself just be evaluated as the start
               of a new/unrelated message. */
            break;
        }
    }
    return (st == expected) && (r.in_progress == 0);
}

static void test_decode_error_record(void)
{
    feb_unencrypted_record_t rec;
    feb_cbor_status_t st;
    int ok;
    static const uint8_t expected_session[FEB_SESSION_ID_LEN] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

    st = feb_cbor_decode_unencrypted(FEB_VEC_RECORD, FEB_VEC_RECORD_LEN, &rec);
    ok = (st == FEB_CBOR_OK);
    if (ok) {
        ok = ok && rec.version == 2;
        ok = ok && rec.type_len == 5 && memcmp(rec.type, "error", 5) == 0;
        ok = ok && memcmp(rec.session_id, expected_session, FEB_SESSION_ID_LEN) == 0;
        ok = ok && rec.board_id_len == 15 &&
             memcmp(rec.board_id, "esp32-c6-test01", 15) == 0;
    }
    check(ok, "decode_unencrypted: envelope fields match FEB_VEC_RECORD");

    if (ok) {
        feb_error_payload_t payload;
        feb_cbor_status_t pst = feb_cbor_decode_error_payload(rec.payload_span,
                                                               rec.payload_span_len,
                                                               &payload);
        int pok = (pst == FEB_CBOR_OK);

        pok = pok && payload.code_len == 14 &&
              memcmp(payload.code, "internal_error", 14) == 0;
        pok = pok && payload.has_message && payload.message_len == 11 &&
              memcmp(payload.message, "test vector", 11) == 0;
        pok = pok && payload.has_request_id && payload.request_id == 42;
        check(pok, "decode_error_payload: code/message/request_id match FEB_VEC_RECORD");
    } else {
        check(0, "decode_error_payload: code/message/request_id match FEB_VEC_RECORD");
    }
}

static void test_encode_roundtrip(void)
{
    uint8_t payload_buf[64];
    uint8_t record_buf[256];
    feb_error_payload_t payload;
    feb_unencrypted_record_t rec;
    size_t payload_len;
    size_t record_len;
    static const uint8_t session_id[FEB_SESSION_ID_LEN] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

    payload.code = "internal_error";
    payload.code_len = 14;
    payload.message = "test vector";
    payload.message_len = 11;
    payload.has_message = 1;
    payload.request_id = 42;
    payload.has_request_id = 1;
    payload_len = feb_cbor_encode_error_payload(payload_buf, sizeof(payload_buf), &payload);

    rec.version = 2;
    rec.type = "error";
    rec.type_len = 5;
    memcpy(rec.session_id, session_id, FEB_SESSION_ID_LEN);
    rec.board_id = "esp32-c6-test01";
    rec.board_id_len = 15;
    rec.payload_span = payload_buf;
    rec.payload_span_len = payload_len;
    record_len = feb_cbor_encode_unencrypted(record_buf, sizeof(record_buf), &rec);

    check(payload_len > 0 && bytes_eq(record_buf, record_len, FEB_VEC_RECORD, FEB_VEC_RECORD_LEN),
          "encode round-trip byte-identical to FEB_VEC_RECORD");
}

static void test_malformed_cbor(void)
{
    feb_unencrypted_record_t rec;
    feb_cbor_status_t st;

    st = feb_cbor_decode_unencrypted(FEB_VEC_DUPLICATE_KEY, FEB_VEC_DUPLICATE_KEY_LEN, &rec);
    check(st == FEB_CBOR_ERR_DUPLICATE_KEY, "duplicate key rejected");

    st = feb_cbor_decode_unencrypted(FEB_VEC_OUT_OF_ORDER_FIELDS, FEB_VEC_OUT_OF_ORDER_FIELDS_LEN, &rec);
    check(st == FEB_CBOR_ERR_OUT_OF_ORDER, "out-of-order fields rejected");

    st = feb_cbor_decode_unencrypted(FEB_VEC_INDEFINITE_LENGTH_MAP, FEB_VEC_INDEFINITE_LENGTH_MAP_LEN, &rec);
    check(st == FEB_CBOR_ERR_INDEFINITE_LENGTH, "indefinite-length map rejected");

    st = feb_cbor_decode_unencrypted(FEB_VEC_MISSING_FIELD, FEB_VEC_MISSING_FIELD_LEN, &rec);
    check(st == FEB_CBOR_ERR_MISSING_FIELD, "missing field rejected");

    st = feb_cbor_decode_unencrypted(FEB_VEC_UNEXPECTED_TYPE, FEB_VEC_UNEXPECTED_TYPE_LEN, &rec);
    check(st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "unexpected type rejected");

    st = feb_cbor_decode_unencrypted(FEB_VEC_OVERSIZED_PAYLOAD_RECORD, FEB_VEC_OVERSIZED_PAYLOAD_RECORD_LEN, &rec);
    check(st == FEB_CBOR_ERR_TOO_LARGE, "oversized payload record rejected");
}

int main(void)
{
    test_fragment_record(23, FEB_VEC_FRAGS_MTU23, FEB_VEC_FRAGS_MTU23_LENS,
                          FEB_VEC_FRAGS_MTU23_COUNT, "fragment_record MTU23 matches vectors");
    test_fragment_record(247, FEB_VEC_FRAGS_MTU247, FEB_VEC_FRAGS_MTU247_LENS,
                          FEB_VEC_FRAGS_MTU247_COUNT, "fragment_record MTU247 matches vectors");

    test_reassemble(FEB_VEC_FRAGS_MTU23, FEB_VEC_FRAGS_MTU23_LENS, FEB_VEC_FRAGS_MTU23_COUNT,
                     "reassembly MTU23 reconstructs FEB_VEC_RECORD");
    test_reassemble(FEB_VEC_FRAGS_MTU247, FEB_VEC_FRAGS_MTU247_LENS, FEB_VEC_FRAGS_MTU247_COUNT,
                     "reassembly MTU247 reconstructs FEB_VEC_RECORD");

    check(run_malformed_fragments(FEB_VEC_DUP_FRAGS, FEB_VEC_DUP_FRAGS_LENS,
                                   FEB_VEC_DUP_FRAGS_COUNT, FEB_FRAME_DUPLICATE_FRAGMENT),
          "duplicate fragment rejected and reassembly reset");
    check(run_malformed_fragments(FEB_VEC_INCONSISTENT_COUNT_FRAGS,
                                   FEB_VEC_INCONSISTENT_COUNT_FRAGS_LENS,
                                   FEB_VEC_INCONSISTENT_COUNT_FRAGS_COUNT,
                                   FEB_FRAME_INCONSISTENT_COUNT),
          "inconsistent fragment_count rejected and reassembly reset");
    check(run_malformed_fragments(FEB_VEC_OUT_OF_ORDER_FRAGS, FEB_VEC_OUT_OF_ORDER_FRAGS_LENS,
                                   FEB_VEC_OUT_OF_ORDER_FRAGS_COUNT, FEB_FRAME_OUT_OF_ORDER),
          "out-of-order fragment rejected and reassembly reset");
    {
        feb_reassembly_t r;
        const uint8_t *out_record = NULL;
        size_t out_len = 0;
        feb_frame_status_t st;

        feb_reassembly_reset(&r);
        st = feb_reassembly_feed(&r, FEB_VEC_OVERSIZED_FRAG0, FEB_VEC_OVERSIZED_FRAG0_LEN, 0,
                                  &out_record, &out_len);
        check(st == FEB_FRAME_OVERSIZED && r.in_progress == 0,
              "oversized fragment 0 rejected and reassembly reset");
    }

    test_decode_error_record();
    test_encode_roundtrip();
    test_malformed_cbor();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

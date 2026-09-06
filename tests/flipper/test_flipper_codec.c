/* Host-native test for flipper/framing.c and flipper/cbor_codec.c against the shared
   vectors in tests/vectors/vectors.h. Built and run with MSVC (cl.exe); see build.ps1. */
#include <stdio.h>
#include <string.h>

#include "framing.h"
#include "cbor_codec.h"
#include "vectors.h"

static int g_total = 0;
static int g_failed = 0;

#define CHECK(cond, desc)                                              \
    do {                                                                \
        g_total++;                                                     \
        if(cond) {                                                      \
            printf("PASS: %s\n", desc);                                \
        } else {                                                        \
            g_failed++;                                                 \
            printf("FAIL: %s (line %d)\n", desc, __LINE__);             \
        }                                                                \
    } while(0)

static int bytes_equal(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

/* ---- fragment capture (feb_fragment_record emit callback) ---- */

#define MAX_CAPTURED_FRAGS 16
#define MAX_FRAG_BYTES 260

typedef struct {
    uint8_t frags[MAX_CAPTURED_FRAGS][MAX_FRAG_BYTES];
    size_t lens[MAX_CAPTURED_FRAGS];
    size_t count;
} capture_ctx_t;

static void capture_emit(const uint8_t* fragment, size_t fragment_len, void* ctx) {
    capture_ctx_t* c = (capture_ctx_t*)ctx;
    if(c->count >= MAX_CAPTURED_FRAGS || fragment_len > MAX_FRAG_BYTES) {
        return;
    }
    memcpy(c->frags[c->count], fragment, fragment_len);
    c->lens[c->count] = fragment_len;
    c->count++;
}

static void test_fragmentation_at_mtu(
    uint16_t mtu,
    const uint8_t* const* expected_frags,
    const size_t* expected_lens,
    size_t expected_count,
    const char* label) {
    size_t capacity = feb_fragment_capacity(mtu);
    char desc[128];

    capture_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    uint8_t count =
        feb_fragment_record(FEB_VEC_RECORD, FEB_VEC_RECORD_LEN, capacity, 7, capture_emit, &ctx);

    snprintf(desc, sizeof(desc), "%s: fragment_count matches vector", label);
    CHECK(count == expected_count, desc);

    snprintf(desc, sizeof(desc), "%s: emitted fragment count matches", label);
    CHECK(ctx.count == expected_count, desc);

    int all_match = 1;
    for(size_t i = 0; i < expected_count && i < ctx.count; i++) {
        if(!bytes_equal(ctx.frags[i], ctx.lens[i], expected_frags[i], expected_lens[i])) {
            all_match = 0;
        }
    }
    snprintf(desc, sizeof(desc), "%s: fragments byte-identical to vector", label);
    CHECK(all_match, desc);

    /* Reassemble and confirm byte-identical to FEB_VEC_RECORD. */
    feb_reassembly_t r;
    feb_reassembly_reset(&r);
    const uint8_t* out_record = NULL;
    size_t out_len = 0;
    int reassembled_ok = 1;
    for(size_t i = 0; i < expected_count; i++) {
        feb_frame_status_t status =
            feb_reassembly_feed(&r, expected_frags[i], expected_lens[i], 0, &out_record, &out_len);
        if(i + 1 < expected_count) {
            if(status != FEB_FRAME_OK) {
                reassembled_ok = 0;
            }
        } else {
            if(status != FEB_FRAME_MESSAGE_COMPLETE) {
                reassembled_ok = 0;
            }
        }
    }
    snprintf(desc, sizeof(desc), "%s: reassembly status sequence OK", label);
    CHECK(reassembled_ok, desc);

    snprintf(desc, sizeof(desc), "%s: reassembled record byte-identical to FEB_VEC_RECORD", label);
    CHECK(bytes_equal(out_record, out_len, FEB_VEC_RECORD, FEB_VEC_RECORD_LEN), desc);
}

static void test_malformed_fragment_sequence(
    const uint8_t* const* frags,
    const size_t* lens,
    size_t count,
    feb_frame_status_t expected_final_status,
    const char* label) {
    feb_reassembly_t r;
    feb_reassembly_reset(&r);
    const uint8_t* out_record = NULL;
    size_t out_len = 0;
    feb_frame_status_t status = FEB_FRAME_OK;
    char desc[128];

    for(size_t i = 0; i < count; i++) {
        status = feb_reassembly_feed(&r, frags[i], lens[i], 0, &out_record, &out_len);
    }

    snprintf(desc, sizeof(desc), "%s: final status is the expected rejection", label);
    CHECK(status == expected_final_status, desc);

    snprintf(desc, sizeof(desc), "%s: reassembly struct reset (not in progress)", label);
    CHECK(r.in_progress == 0, desc);
}

static void test_near_max_record_at_uneven_capacity(void) {
    /* Regression for the fixed oversized-fragment check: a 768-byte record fragmented at
       capacity 240 (ATT MTU 247) needs 4 fragments (3 full + 1 partial), which the buggy
       max-based estimate (fragment_count * payload_len = 4 * 240 = 960 > 768) used to
       reject on the first fragment alone, even though 768 is the exact allowed maximum. */
    uint8_t first_fragment[4 + 240];
    feb_reassembly_t r;
    feb_frame_status_t status;
    const uint8_t* out_record = NULL;
    size_t out_len = 0;

    memset(first_fragment, 0x41, sizeof(first_fragment));
    first_fragment[0] = 0;
    first_fragment[1] = 3;
    first_fragment[2] = 0;
    first_fragment[3] = 4;

    feb_reassembly_reset(&r);
    status = feb_reassembly_feed(&r, first_fragment, sizeof(first_fragment), 0, &out_record, &out_len);
    CHECK(
        status == FEB_FRAME_OK,
        "near-max 768-byte record at uneven capacity: first fragment not spuriously rejected");
}

/* ---- CBOR decode/encode ---- */

static void test_decode_record(void) {
    feb_unencrypted_record_t record;
    feb_cbor_status_t status = feb_cbor_decode_unencrypted(FEB_VEC_RECORD, FEB_VEC_RECORD_LEN, &record);
    CHECK(status == FEB_CBOR_OK, "decode_unencrypted: status OK");
    CHECK(record.version == 2, "decode_unencrypted: version == 2");
    CHECK(
        record.type_len == 5 && memcmp(record.type, "error", 5) == 0,
        "decode_unencrypted: type == \"error\"");
    static const uint8_t expected_session_id[FEB_SESSION_ID_LEN] =
        {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    CHECK(
        memcmp(record.session_id, expected_session_id, FEB_SESSION_ID_LEN) == 0,
        "decode_unencrypted: session_id bytes match");
    CHECK(
        record.board_id_len == strlen("esp32-c6-test01") &&
            memcmp(record.board_id, "esp32-c6-test01", record.board_id_len) == 0,
        "decode_unencrypted: board_id == \"esp32-c6-test01\"");

    feb_error_payload_t payload;
    feb_cbor_status_t payload_status =
        feb_cbor_decode_error_payload(record.payload_span, record.payload_span_len, &payload);
    CHECK(payload_status == FEB_CBOR_OK, "decode_error_payload: status OK");
    CHECK(
        payload.code_len == strlen("internal_error") &&
            memcmp(payload.code, "internal_error", payload.code_len) == 0,
        "decode_error_payload: code == \"internal_error\"");
    CHECK(payload.has_message == 1, "decode_error_payload: has_message");
    CHECK(
        payload.message_len == strlen("test vector") &&
            memcmp(payload.message, "test vector", payload.message_len) == 0,
        "decode_error_payload: message == \"test vector\"");
    CHECK(payload.has_request_id == 1, "decode_error_payload: has_request_id");
    CHECK(payload.request_id == 42, "decode_error_payload: request_id == 42");
}

static void test_encode_roundtrip(void) {
    uint8_t payload_buf[128];
    feb_error_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.code = "internal_error";
    payload.code_len = strlen(payload.code);
    payload.message = "test vector";
    payload.message_len = strlen(payload.message);
    payload.has_message = 1;
    payload.request_id = 42;
    payload.has_request_id = 1;

    size_t payload_len = feb_cbor_encode_error_payload(payload_buf, sizeof(payload_buf), &payload);
    CHECK(payload_len > 0, "encode_error_payload: nonzero length");

    uint8_t record_buf[256];
    feb_unencrypted_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = 2;
    record.type = "error";
    record.type_len = strlen(record.type);
    static const uint8_t session_id[FEB_SESSION_ID_LEN] =
        {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    memcpy(record.session_id, session_id, FEB_SESSION_ID_LEN);
    record.board_id = "esp32-c6-test01";
    record.board_id_len = strlen(record.board_id);
    record.payload_span = payload_buf;
    record.payload_span_len = payload_len;

    size_t record_len = feb_cbor_encode_unencrypted(record_buf, sizeof(record_buf), &record);
    CHECK(record_len > 0, "encode_unencrypted: nonzero length");
    CHECK(
        bytes_equal(record_buf, record_len, FEB_VEC_RECORD, FEB_VEC_RECORD_LEN),
        "encode round-trip byte-identical to FEB_VEC_RECORD");
}

static void test_malformed_cbor(
    const uint8_t* vec,
    size_t vec_len,
    feb_cbor_status_t expected,
    const char* label) {
    feb_unencrypted_record_t record;
    feb_cbor_status_t status = feb_cbor_decode_unencrypted(vec, vec_len, &record);
    char desc[128];
    snprintf(desc, sizeof(desc), "%s: rejected with expected status", label);
    CHECK(status == expected, desc);
}

int main(void) {
    test_fragmentation_at_mtu(
        23,
        FEB_VEC_FRAGS_MTU23,
        FEB_VEC_FRAGS_MTU23_LENS,
        FEB_VEC_FRAGS_MTU23_COUNT,
        "MTU23");
    test_fragmentation_at_mtu(
        247,
        FEB_VEC_FRAGS_MTU247,
        FEB_VEC_FRAGS_MTU247_LENS,
        FEB_VEC_FRAGS_MTU247_COUNT,
        "MTU247");

    test_malformed_fragment_sequence(
        FEB_VEC_DUP_FRAGS,
        FEB_VEC_DUP_FRAGS_LENS,
        FEB_VEC_DUP_FRAGS_COUNT,
        FEB_FRAME_DUPLICATE_FRAGMENT,
        "DUP_FRAGS");
    test_malformed_fragment_sequence(
        FEB_VEC_INCONSISTENT_COUNT_FRAGS,
        FEB_VEC_INCONSISTENT_COUNT_FRAGS_LENS,
        2, /* only the first two fragments are needed to trigger the mismatch */
        FEB_FRAME_INCONSISTENT_COUNT,
        "INCONSISTENT_COUNT_FRAGS");
    {
        const uint8_t* single[1] = {FEB_VEC_OVERSIZED_FRAG0};
        size_t single_len[1] = {FEB_VEC_OVERSIZED_FRAG0_LEN};
        test_malformed_fragment_sequence(
            single, single_len, 1, FEB_FRAME_OVERSIZED, "OVERSIZED_FRAG0");
    }
    test_malformed_fragment_sequence(
        FEB_VEC_OUT_OF_ORDER_FRAGS,
        FEB_VEC_OUT_OF_ORDER_FRAGS_LENS,
        FEB_VEC_OUT_OF_ORDER_FRAGS_COUNT,
        FEB_FRAME_OUT_OF_ORDER,
        "OUT_OF_ORDER_FRAGS");

    test_near_max_record_at_uneven_capacity();

    test_decode_record();
    test_encode_roundtrip();

    test_malformed_cbor(
        FEB_VEC_DUPLICATE_KEY, FEB_VEC_DUPLICATE_KEY_LEN, FEB_CBOR_ERR_DUPLICATE_KEY, "DUPLICATE_KEY");
    test_malformed_cbor(
        FEB_VEC_OUT_OF_ORDER_FIELDS,
        FEB_VEC_OUT_OF_ORDER_FIELDS_LEN,
        FEB_CBOR_ERR_OUT_OF_ORDER,
        "OUT_OF_ORDER_FIELDS");
    test_malformed_cbor(
        FEB_VEC_INDEFINITE_LENGTH_MAP,
        FEB_VEC_INDEFINITE_LENGTH_MAP_LEN,
        FEB_CBOR_ERR_INDEFINITE_LENGTH,
        "INDEFINITE_LENGTH_MAP");
    test_malformed_cbor(
        FEB_VEC_MISSING_FIELD, FEB_VEC_MISSING_FIELD_LEN, FEB_CBOR_ERR_MISSING_FIELD, "MISSING_FIELD");
    test_malformed_cbor(
        FEB_VEC_UNEXPECTED_TYPE,
        FEB_VEC_UNEXPECTED_TYPE_LEN,
        FEB_CBOR_ERR_UNEXPECTED_TYPE,
        "UNEXPECTED_TYPE");
    test_malformed_cbor(
        FEB_VEC_OVERSIZED_PAYLOAD_RECORD,
        FEB_VEC_OVERSIZED_PAYLOAD_RECORD_LEN,
        FEB_CBOR_ERR_TOO_LARGE,
        "OVERSIZED_PAYLOAD_RECORD");

    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}

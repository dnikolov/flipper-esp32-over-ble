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

/* docs/CODE_REVIEW_FIX_PLAN.md W1-W3: feb_cbor_skip_value() itself has zero direct test
   coverage prior to this pass; these call it directly rather than only through envelope
   decoding of well-formed payloads. */
static void test_skip_value_direct(void) {
    feb_cbor_status_t st;
    const uint8_t* span;
    size_t span_len;
    size_t n;

    n = feb_cbor_skip_value(FEB_VEC_SKIP_NEGINT, FEB_VEC_SKIP_NEGINT_LEN, 2, &span, &span_len, &st);
    CHECK(n == 0 && st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "skip_value: negative integer rejected");

    n = feb_cbor_skip_value(FEB_VEC_SKIP_TRUE, FEB_VEC_SKIP_TRUE_LEN, 2, &span, &span_len, &st);
    CHECK(n == 0 && st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "skip_value: true rejected");

    n = feb_cbor_skip_value(FEB_VEC_SKIP_NULL, FEB_VEC_SKIP_NULL_LEN, 2, &span, &span_len, &st);
    CHECK(n == 0 && st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "skip_value: null rejected");

    n = feb_cbor_skip_value(
        FEB_VEC_SKIP_TRUNCATED_MAP, FEB_VEC_SKIP_TRUNCATED_MAP_LEN, 2, &span, &span_len, &st);
    CHECK(n == 0 && st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "skip_value: truncated map rejected (W1)");

    n = feb_cbor_skip_value(
        FEB_VEC_SKIP_NEST_AT_LIMIT, FEB_VEC_SKIP_NEST_AT_LIMIT_LEN, 2, &span, &span_len, &st);
    CHECK(n > 0 && st == FEB_CBOR_OK, "skip_value: nesting at FEB_CBOR_MAX_NESTING accepted");

    n = feb_cbor_skip_value(
        FEB_VEC_SKIP_NEST_TOO_DEEP, FEB_VEC_SKIP_NEST_TOO_DEEP_LEN, 2, &span, &span_len, &st);
    CHECK(
        n == 0 && st == FEB_CBOR_ERR_TOO_DEEP,
        "skip_value: nesting one level past FEB_CBOR_MAX_NESTING rejected");
}

/* Same shapes as above, wrapped as a real payload span and decoded through the full
   envelope decoder (docs/CODE_REVIEW_FIX_PLAN.md W1-W3, W6). */
static void test_payload_type_depth_and_trailing(void) {
    feb_unencrypted_record_t record;
    feb_cbor_status_t status;

    status = feb_cbor_decode_unencrypted(
        FEB_VEC_PAYLOAD_NEGINT_RECORD, FEB_VEC_PAYLOAD_NEGINT_RECORD_LEN, &record);
    CHECK(status == FEB_CBOR_ERR_UNEXPECTED_TYPE, "payload with negative-int value rejected");

    status = feb_cbor_decode_unencrypted(
        FEB_VEC_PAYLOAD_TRUE_RECORD, FEB_VEC_PAYLOAD_TRUE_RECORD_LEN, &record);
    CHECK(status == FEB_CBOR_ERR_UNEXPECTED_TYPE, "payload with true value rejected");

    status = feb_cbor_decode_unencrypted(
        FEB_VEC_PAYLOAD_NULL_RECORD, FEB_VEC_PAYLOAD_NULL_RECORD_LEN, &record);
    CHECK(status == FEB_CBOR_ERR_UNEXPECTED_TYPE, "payload with null value rejected");

    status = feb_cbor_decode_unencrypted(
        FEB_VEC_TRUNCATED_PAYLOAD_MAP_RECORD, FEB_VEC_TRUNCATED_PAYLOAD_MAP_RECORD_LEN, &record);
    CHECK(status == FEB_CBOR_ERR_UNEXPECTED_TYPE, "record with truncated payload map rejected (W1)");

    status = feb_cbor_decode_unencrypted(
        FEB_VEC_NEST_AT_LIMIT_RECORD, FEB_VEC_NEST_AT_LIMIT_RECORD_LEN, &record);
    CHECK(status == FEB_CBOR_OK, "payload nested exactly to FEB_CBOR_MAX_NESTING accepted");

    status = feb_cbor_decode_unencrypted(
        FEB_VEC_NEST_TOO_DEEP_RECORD, FEB_VEC_NEST_TOO_DEEP_RECORD_LEN, &record);
    CHECK(status == FEB_CBOR_ERR_TOO_DEEP, "payload nested one level too deep rejected (W3)");

    status = feb_cbor_decode_unencrypted(
        FEB_VEC_RECORD_TRAILING_BYTE, FEB_VEC_RECORD_TRAILING_BYTE_LEN, &record);
    CHECK(status == FEB_CBOR_ERR_UNEXPECTED_TYPE, "record with trailing byte rejected (W6)");
}

/* ---- wifi_scan payload codecs (docs/PROTOCOL.md "`wifi_scan` command and status
   payloads", docs/PLAN.md's Wi-Fi scan capability follow-on step) ---- */

static void check_ap_matches(
    const feb_wifi_scan_ap_t* ap,
    const uint8_t* ssid,
    size_t ssid_len,
    const uint8_t* bssid,
    uint64_t rssi_offset,
    uint64_t channel,
    const char* phy,
    const char* auth,
    const char* label) {
    char desc[160];
    snprintf(desc, sizeof(desc), "%s: ssid matches", label);
    CHECK(bytes_equal(ap->ssid, ap->ssid_len, ssid, ssid_len), desc);
    snprintf(desc, sizeof(desc), "%s: bssid matches", label);
    CHECK(memcmp(ap->bssid, bssid, FEB_WIFI_SCAN_BSSID_LEN) == 0, desc);
    snprintf(desc, sizeof(desc), "%s: rssi_offset matches", label);
    CHECK(ap->rssi_offset == rssi_offset, desc);
    snprintf(desc, sizeof(desc), "%s: channel matches", label);
    CHECK(ap->channel == channel, desc);
    snprintf(desc, sizeof(desc), "%s: phy matches", label);
    CHECK(ap->phy_len == strlen(phy) && memcmp(ap->phy, phy, ap->phy_len) == 0, desc);
    snprintf(desc, sizeof(desc), "%s: auth matches", label);
    CHECK(ap->auth_len == strlen(auth) && memcmp(ap->auth, auth, ap->auth_len) == 0, desc);
}

static void test_wifi_scan_ap_codec(void) {
    /* AP1: a normal entry. */
    {
        feb_cbor_status_t status;
        feb_wifi_scan_ap_t ap;
        size_t n = feb_cbor_decode_wifi_scan_ap(FEB_VEC_WIFI_SCAN_AP1, FEB_VEC_WIFI_SCAN_AP1_LEN, &ap, &status);
        CHECK(n == FEB_VEC_WIFI_SCAN_AP1_LEN && status == FEB_CBOR_OK, "AP1: decode consumes whole vector");
        static const uint8_t ssid[] = "TestNetwork";
        static const uint8_t bssid[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        check_ap_matches(&ap, ssid, sizeof(ssid) - 1, bssid, 0x4e, 6, "11n", "wpa2_psk", "AP1");

        uint8_t out[128];
        size_t out_len = feb_cbor_encode_wifi_scan_ap(out, sizeof(out), &ap);
        CHECK(out_len > 0, "AP1: encode succeeds");
        CHECK(
            bytes_equal(out, out_len, FEB_VEC_WIFI_SCAN_AP1, FEB_VEC_WIFI_SCAN_AP1_LEN),
            "AP1: encode round-trip byte-identical to vector");
    }

    /* AP2: rssi_offset extreme low (0, i.e. rssi_dbm -128), empty/hidden SSID (0 bytes). */
    {
        feb_cbor_status_t status;
        feb_wifi_scan_ap_t ap;
        size_t n = feb_cbor_decode_wifi_scan_ap(FEB_VEC_WIFI_SCAN_AP2, FEB_VEC_WIFI_SCAN_AP2_LEN, &ap, &status);
        CHECK(n == FEB_VEC_WIFI_SCAN_AP2_LEN && status == FEB_CBOR_OK, "AP2: decode consumes whole vector");
        CHECK(ap.ssid_len == 0, "AP2: ssid is zero-length (hidden network)");
        static const uint8_t bssid[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        check_ap_matches(&ap, ap.ssid, 0, bssid, 0, 1, "11b", "open", "AP2");

        uint8_t out[128];
        size_t out_len = feb_cbor_encode_wifi_scan_ap(out, sizeof(out), &ap);
        CHECK(out_len > 0, "AP2: encode succeeds");
        CHECK(
            bytes_equal(out, out_len, FEB_VEC_WIFI_SCAN_AP2, FEB_VEC_WIFI_SCAN_AP2_LEN),
            "AP2: encode round-trip byte-identical to vector");
    }

    /* AP3: rssi_offset extreme high (255, i.e. rssi_dbm +127), non-UTF-8 SSID (raw bytes,
       not text -- must not be validated/decoded as UTF-8), auth "unknown" fallback. */
    {
        feb_cbor_status_t status;
        feb_wifi_scan_ap_t ap;
        size_t n = feb_cbor_decode_wifi_scan_ap(FEB_VEC_WIFI_SCAN_AP3, FEB_VEC_WIFI_SCAN_AP3_LEN, &ap, &status);
        CHECK(n == FEB_VEC_WIFI_SCAN_AP3_LEN && status == FEB_CBOR_OK, "AP3: decode consumes whole vector");
        static const uint8_t ssid[] = {0xff, 0xfe, 0x00, 0x41};
        static const uint8_t bssid[] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
        check_ap_matches(&ap, ssid, sizeof(ssid), bssid, 255, 11, "11ax", "unknown", "AP3");

        uint8_t out[128];
        size_t out_len = feb_cbor_encode_wifi_scan_ap(out, sizeof(out), &ap);
        CHECK(out_len > 0, "AP3: encode succeeds");
        CHECK(
            bytes_equal(out, out_len, FEB_VEC_WIFI_SCAN_AP3, FEB_VEC_WIFI_SCAN_AP3_LEN),
            "AP3: encode round-trip byte-identical to vector");
    }
}

static void test_wifi_scan_result_payload_codec(void) {
    static feb_wifi_scan_ap_t ap1, ap2, ap3;
    feb_cbor_status_t status;
    feb_cbor_decode_wifi_scan_ap(FEB_VEC_WIFI_SCAN_AP1, FEB_VEC_WIFI_SCAN_AP1_LEN, &ap1, &status);
    feb_cbor_decode_wifi_scan_ap(FEB_VEC_WIFI_SCAN_AP2, FEB_VEC_WIFI_SCAN_AP2_LEN, &ap2, &status);
    feb_cbor_decode_wifi_scan_ap(FEB_VEC_WIFI_SCAN_AP3, FEB_VEC_WIFI_SCAN_AP3_LEN, &ap3, &status);

    /* single */
    {
        static feb_wifi_scan_result_payload_t single;
        single.aps[0] = ap1;
        single.ap_count = 1;
        static uint8_t out[256];
        size_t out_len = feb_cbor_encode_wifi_scan_result_payload(out, sizeof(out), &single);
        CHECK(out_len > 0, "RESULT_SINGLE: encode succeeds");
        CHECK(
            bytes_equal(out, out_len, FEB_VEC_WIFI_SCAN_RESULT_SINGLE, FEB_VEC_WIFI_SCAN_RESULT_SINGLE_LEN),
            "RESULT_SINGLE: encode byte-identical to vector");

        static feb_wifi_scan_result_payload_t decoded;
        feb_cbor_status_t decode_status = feb_cbor_decode_wifi_scan_result_payload(
            FEB_VEC_WIFI_SCAN_RESULT_SINGLE, FEB_VEC_WIFI_SCAN_RESULT_SINGLE_LEN, &decoded);
        CHECK(decode_status == FEB_CBOR_OK, "RESULT_SINGLE: decode status OK");
        CHECK(decoded.ap_count == 1, "RESULT_SINGLE: ap_count == 1");
    }

    /* multi (three APs) */
    {
        static feb_wifi_scan_result_payload_t multi;
        multi.aps[0] = ap1;
        multi.aps[1] = ap2;
        multi.aps[2] = ap3;
        multi.ap_count = 3;
        static uint8_t out[512];
        size_t out_len = feb_cbor_encode_wifi_scan_result_payload(out, sizeof(out), &multi);
        CHECK(out_len > 0, "RESULT_MULTI: encode succeeds");
        CHECK(
            bytes_equal(out, out_len, FEB_VEC_WIFI_SCAN_RESULT_MULTI, FEB_VEC_WIFI_SCAN_RESULT_MULTI_LEN),
            "RESULT_MULTI: encode byte-identical to vector");

        static feb_wifi_scan_result_payload_t decoded;
        feb_cbor_status_t decode_status = feb_cbor_decode_wifi_scan_result_payload(
            FEB_VEC_WIFI_SCAN_RESULT_MULTI, FEB_VEC_WIFI_SCAN_RESULT_MULTI_LEN, &decoded);
        CHECK(decode_status == FEB_CBOR_OK, "RESULT_MULTI: decode status OK");
        CHECK(decoded.ap_count == 3, "RESULT_MULTI: ap_count == 3");
    }

    /* empty */
    {
        feb_wifi_scan_result_payload_t empty;
        memset(&empty, 0, sizeof(empty));
        uint8_t out[16];
        size_t out_len = feb_cbor_encode_wifi_scan_result_payload(out, sizeof(out), &empty);
        CHECK(out_len > 0, "RESULT_EMPTY: encode succeeds");
        CHECK(
            bytes_equal(out, out_len, FEB_VEC_WIFI_SCAN_RESULT_EMPTY, FEB_VEC_WIFI_SCAN_RESULT_EMPTY_LEN),
            "RESULT_EMPTY: encode byte-identical to vector");

        feb_wifi_scan_result_payload_t decoded;
        feb_cbor_status_t decode_status = feb_cbor_decode_wifi_scan_result_payload(
            FEB_VEC_WIFI_SCAN_RESULT_EMPTY, FEB_VEC_WIFI_SCAN_RESULT_EMPTY_LEN, &decoded);
        CHECK(decode_status == FEB_CBOR_OK, "RESULT_EMPTY: decode status OK");
        CHECK(decoded.ap_count == 0, "RESULT_EMPTY: ap_count == 0");
    }
}

static void test_command_payload_codec(void) {
    /* valid: empty arguments */
    {
        uint8_t arguments_buf[4];
        size_t arguments_len = feb_cbor_encode_map_header(arguments_buf, sizeof(arguments_buf), 0);
        feb_command_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.capability = "wifi_scan";
        payload.capability_len = strlen(payload.capability);
        payload.request_id = 101;
        payload.arguments_span = arguments_buf;
        payload.arguments_span_len = arguments_len;

        uint8_t out[64];
        size_t out_len = feb_cbor_encode_command_payload(out, sizeof(out), &payload);
        CHECK(out_len > 0, "COMMAND_PAYLOAD: encode succeeds");
        CHECK(
            bytes_equal(out, out_len, FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD, FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD_LEN),
            "COMMAND_PAYLOAD: encode byte-identical to vector");

        feb_command_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_command_payload(
            FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD, FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "COMMAND_PAYLOAD: decode status OK");
        CHECK(
            decoded.capability_len == strlen("wifi_scan") &&
                memcmp(decoded.capability, "wifi_scan", decoded.capability_len) == 0,
            "COMMAND_PAYLOAD: decoded capability == \"wifi_scan\"");
        CHECK(decoded.request_id == 101, "COMMAND_PAYLOAD: decoded request_id == 101");
        CHECK(decoded.arguments_span_len == 1, "COMMAND_PAYLOAD: decoded arguments span is the empty map (1 byte)");
    }

    /* malformed per wifi_scan's own rule (non-empty arguments) but still structurally
       well-formed CBOR -- this generic codec must still decode it successfully, capturing
       the non-empty arguments span; rejecting a non-empty wifi_scan arguments map with
       invalid_command is a dispatch-layer decision, not this codec's job (see header
       comment). */
    {
        feb_command_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_command_payload(
            FEB_VEC_WIFI_SCAN_COMMAND_BAD_ARGUMENTS_PAYLOAD,
            FEB_VEC_WIFI_SCAN_COMMAND_BAD_ARGUMENTS_PAYLOAD_LEN,
            &decoded);
        CHECK(status == FEB_CBOR_OK, "COMMAND_BAD_ARGUMENTS_PAYLOAD: decode still succeeds structurally");
        CHECK(
            decoded.arguments_span_len > 1,
            "COMMAND_BAD_ARGUMENTS_PAYLOAD: decoded arguments span is non-empty");
    }
}

static void test_status_payload_codec(void) {
    /* partial, with a one-AP result */
    {
        feb_status_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_status_payload(
            FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "STATUS_PARTIAL: decode status OK");
        CHECK(decoded.request_id == 101, "STATUS_PARTIAL: decoded request_id == 101");
        CHECK(
            decoded.state_len == strlen("partial") && memcmp(decoded.state, "partial", decoded.state_len) == 0,
            "STATUS_PARTIAL: decoded state == \"partial\"");
        CHECK(decoded.has_result == 1, "STATUS_PARTIAL: has_result");

        static feb_wifi_scan_result_payload_t result;
        feb_cbor_status_t result_status =
            feb_cbor_decode_wifi_scan_result_payload(decoded.result_span, decoded.result_span_len, &result);
        CHECK(result_status == FEB_CBOR_OK, "STATUS_PARTIAL: nested result decodes OK");
        CHECK(result.ap_count == 1, "STATUS_PARTIAL: nested result has 1 AP");

        uint8_t out[256];
        feb_status_payload_t to_encode = decoded;
        size_t out_len = feb_cbor_encode_status_payload(out, sizeof(out), &to_encode);
        CHECK(out_len > 0, "STATUS_PARTIAL: encode succeeds");
        CHECK(
            bytes_equal(
                out, out_len, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD_LEN),
            "STATUS_PARTIAL: encode round-trip byte-identical to vector");
    }

    /* complete, with a three-AP result */
    {
        feb_status_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_status_payload(
            FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "STATUS_COMPLETE: decode status OK");
        CHECK(
            decoded.state_len == strlen("complete") && memcmp(decoded.state, "complete", decoded.state_len) == 0,
            "STATUS_COMPLETE: decoded state == \"complete\"");
        static feb_wifi_scan_result_payload_t result;
        feb_cbor_status_t result_status =
            feb_cbor_decode_wifi_scan_result_payload(decoded.result_span, decoded.result_span_len, &result);
        CHECK(result_status == FEB_CBOR_OK, "STATUS_COMPLETE: nested result decodes OK");
        CHECK(result.ap_count == 3, "STATUS_COMPLETE: nested result has 3 APs");
    }

    /* complete with an empty aps array (a prior partial already delivered everything) */
    {
        feb_status_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_status_payload(
            FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_EMPTY_PAYLOAD,
            FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_EMPTY_PAYLOAD_LEN,
            &decoded);
        CHECK(status == FEB_CBOR_OK, "STATUS_COMPLETE_EMPTY: decode status OK");
        feb_wifi_scan_result_payload_t result;
        feb_cbor_status_t result_status =
            feb_cbor_decode_wifi_scan_result_payload(decoded.result_span, decoded.result_span_len, &result);
        CHECK(result_status == FEB_CBOR_OK, "STATUS_COMPLETE_EMPTY: nested result decodes OK");
        CHECK(result.ap_count == 0, "STATUS_COMPLETE_EMPTY: nested result has 0 APs");
    }

    /* malformed state ("started", neither "partial" nor "complete") -- this generic codec
       does not validate the specific state string; that is the dispatch layer's job, so
       decode must still succeed structurally. */
    {
        feb_status_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_status_payload(
            FEB_VEC_WIFI_SCAN_STATUS_BAD_STATE_PAYLOAD, FEB_VEC_WIFI_SCAN_STATUS_BAD_STATE_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "STATUS_BAD_STATE: decode still succeeds structurally");
        CHECK(
            decoded.state_len == strlen("started") && memcmp(decoded.state, "started", decoded.state_len) == 0,
            "STATUS_BAD_STATE: decoded state == \"started\" (not partial/complete)");
    }
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

    test_skip_value_direct();
    test_payload_type_depth_and_trailing();

    test_wifi_scan_ap_codec();
    test_wifi_scan_result_payload_codec();
    test_command_payload_codec();
    test_status_payload_codec();

    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}

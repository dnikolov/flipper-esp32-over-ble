/* Host-native test for flipper/framing.c and flipper/cbor_codec.c against the shared
   vectors in tests/vectors/vectors.h. Built and run with MSVC (cl.exe); see build.ps1. */
#include <stdio.h>
#include <string.h>

#include "framing.h"
#include "cbor_codec.h"
#include "wardriving_csv.h"
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

/* ---- ble_scan payload codecs (docs/PROTOCOL.md "`ble_scan` command and status
   payloads") ----
   No shared FEB_VEC_* vectors exist for these yet (this pass is scoped to the codec layer
   only, and a parallel ESP32-side task is producing its own vectors from the same frozen
   PROTOCOL.md spec) -- these tests build their own inputs directly via this file's own
   encode functions and hand-assembled raw CBOR, rather than extending
   tests/vectors/generate_vectors.py, to avoid a concurrent-edit collision with that
   parallel task on a shared generated file. */

static void test_ble_scan_device_codec(void) {
    /* device with a name */
    {
        feb_ble_scan_device_t device;
        memset(&device, 0, sizeof(device));
        static const uint8_t address[FEB_BLE_SCAN_ADDRESS_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
        memcpy(device.address, address, FEB_BLE_SCAN_ADDRESS_LEN);
        device.name = "TestDevice";
        device.name_len = strlen(device.name);
        device.has_name = 1;
        device.rssi_offset = 78; /* rssi_dbm -50 */
        device.addr_type = "public";
        device.addr_type_len = strlen(device.addr_type);

        uint8_t out[128];
        size_t out_len = feb_cbor_encode_ble_scan_device(out, sizeof(out), &device);
        CHECK(out_len > 0, "BLE_DEVICE_NAMED: encode succeeds");

        feb_cbor_status_t status;
        feb_ble_scan_device_t decoded;
        size_t n = feb_cbor_decode_ble_scan_device(out, out_len, &decoded, &status);
        CHECK(n == out_len && status == FEB_CBOR_OK, "BLE_DEVICE_NAMED: decode consumes whole buffer");
        CHECK(memcmp(decoded.address, address, FEB_BLE_SCAN_ADDRESS_LEN) == 0, "BLE_DEVICE_NAMED: address matches");
        CHECK(decoded.has_name == 1, "BLE_DEVICE_NAMED: has_name set");
        CHECK(
            decoded.name_len == strlen("TestDevice") && memcmp(decoded.name, "TestDevice", decoded.name_len) == 0,
            "BLE_DEVICE_NAMED: name matches");
        CHECK(decoded.rssi_offset == 78, "BLE_DEVICE_NAMED: rssi_offset matches");
        CHECK(
            decoded.addr_type_len == strlen("public") && memcmp(decoded.addr_type, "public", decoded.addr_type_len) == 0,
            "BLE_DEVICE_NAMED: addr_type matches");

        uint8_t reencoded[128];
        size_t reencoded_len = feb_cbor_encode_ble_scan_device(reencoded, sizeof(reencoded), &decoded);
        CHECK(
            bytes_equal(reencoded, reencoded_len, out, out_len),
            "BLE_DEVICE_NAMED: re-encode byte-identical");
    }

    /* device with no advertised name -- optional-field omission, same convention as
       feb_error_payload_t.has_message */
    {
        feb_ble_scan_device_t device;
        memset(&device, 0, sizeof(device));
        static const uint8_t address[FEB_BLE_SCAN_ADDRESS_LEN] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        memcpy(device.address, address, FEB_BLE_SCAN_ADDRESS_LEN);
        device.has_name = 0;
        device.rssi_offset = 0; /* rssi_dbm -128, extreme low */
        device.addr_type = "random";
        device.addr_type_len = strlen(device.addr_type);

        uint8_t out[128];
        size_t out_len = feb_cbor_encode_ble_scan_device(out, sizeof(out), &device);
        CHECK(out_len > 0, "BLE_DEVICE_NO_NAME: encode succeeds");

        feb_cbor_status_t status;
        feb_ble_scan_device_t decoded;
        size_t n = feb_cbor_decode_ble_scan_device(out, out_len, &decoded, &status);
        CHECK(n == out_len && status == FEB_CBOR_OK, "BLE_DEVICE_NO_NAME: decode consumes whole buffer");
        CHECK(decoded.has_name == 0, "BLE_DEVICE_NO_NAME: has_name not set");
        CHECK(decoded.rssi_offset == 0, "BLE_DEVICE_NO_NAME: rssi_offset matches");
        CHECK(
            decoded.addr_type_len == strlen("random") && memcmp(decoded.addr_type, "random", decoded.addr_type_len) == 0,
            "BLE_DEVICE_NO_NAME: addr_type matches");
    }

    /* malformed: only 2 fields present (address, rssi_offset) -- missing addr_type */
    {
        uint8_t buf[64];
        size_t pos = 0;
        size_t n;
        static const uint8_t address[FEB_BLE_SCAN_ADDRESS_LEN] = {0, 1, 2, 3, 4, 5};
        n = feb_cbor_encode_map_header(buf, sizeof(buf), 2);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "address", sizeof("address") - 1);
        pos += n;
        n = feb_cbor_encode_bytes(buf + pos, sizeof(buf) - pos, address, FEB_BLE_SCAN_ADDRESS_LEN);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "rssi_offset", sizeof("rssi_offset") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 50);
        pos += n;

        feb_cbor_status_t status;
        feb_ble_scan_device_t decoded;
        size_t decode_n = feb_cbor_decode_ble_scan_device(buf, pos, &decoded, &status);
        CHECK(
            decode_n == 0 && status == FEB_CBOR_ERR_MISSING_FIELD,
            "BLE_DEVICE_MISSING_ADDR_TYPE: rejected with MISSING_FIELD");
    }
}

static void test_ble_scan_result_payload_codec(void) {
    static feb_ble_scan_device_t device1, device2;
    memset(&device1, 0, sizeof(device1));
    memset(&device2, 0, sizeof(device2));
    static const uint8_t addr1[FEB_BLE_SCAN_ADDRESS_LEN] = {1, 2, 3, 4, 5, 6};
    static const uint8_t addr2[FEB_BLE_SCAN_ADDRESS_LEN] = {6, 5, 4, 3, 2, 1};
    memcpy(device1.address, addr1, FEB_BLE_SCAN_ADDRESS_LEN);
    device1.name = "Alpha";
    device1.name_len = strlen(device1.name);
    device1.has_name = 1;
    device1.rssi_offset = 90;
    device1.addr_type = "public";
    device1.addr_type_len = strlen(device1.addr_type);

    memcpy(device2.address, addr2, FEB_BLE_SCAN_ADDRESS_LEN);
    device2.has_name = 0;
    device2.rssi_offset = 40;
    device2.addr_type = "random";
    device2.addr_type_len = strlen(device2.addr_type);

    /* two devices */
    {
        static feb_ble_scan_result_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.devices[0] = device1;
        payload.devices[1] = device2;
        payload.device_count = 2;

        static uint8_t out[256];
        size_t out_len = feb_cbor_encode_ble_scan_result_payload(out, sizeof(out), &payload);
        CHECK(out_len > 0, "BLE_RESULT_TWO: encode succeeds");

        static feb_ble_scan_result_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_ble_scan_result_payload(out, out_len, &decoded);
        CHECK(status == FEB_CBOR_OK, "BLE_RESULT_TWO: decode status OK");
        CHECK(decoded.device_count == 2, "BLE_RESULT_TWO: device_count == 2");
        CHECK(decoded.devices[0].has_name == 1, "BLE_RESULT_TWO: device0 has_name");
        CHECK(decoded.devices[1].has_name == 0, "BLE_RESULT_TWO: device1 has no name");
    }

    /* empty */
    {
        feb_ble_scan_result_payload_t empty;
        memset(&empty, 0, sizeof(empty));
        uint8_t out[16];
        size_t out_len = feb_cbor_encode_ble_scan_result_payload(out, sizeof(out), &empty);
        CHECK(out_len > 0, "BLE_RESULT_EMPTY: encode succeeds");

        feb_ble_scan_result_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_ble_scan_result_payload(out, out_len, &decoded);
        CHECK(status == FEB_CBOR_OK, "BLE_RESULT_EMPTY: decode status OK");
        CHECK(decoded.device_count == 0, "BLE_RESULT_EMPTY: device_count == 0");
    }
}

/* ---- wardriving payload codecs (docs/PROTOCOL.md "`wardriving` command and status
   payloads") ---- */

static void test_wardriving_command_payload_codec(void) {
    /* start, both sources, aggressive/point-4 defaults */
    {
        feb_wardriving_command_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.action = "start";
        payload.action_len = strlen(payload.action);
        payload.sources[0] = "wifi";
        payload.source_lens[0] = strlen("wifi");
        payload.sources[1] = "ble";
        payload.source_lens[1] = strlen("ble");
        payload.source_count = 2;
        payload.has_sources = 1;
        payload.wifi_interval_ms = 30; /* continuous-ish, step 4 point 4 */
        payload.has_wifi_interval_ms = 1;
        payload.ble_window_ms = 30;
        payload.ble_interval_ms = 30;
        payload.has_ble_params = 1;

        uint8_t out[128];
        size_t out_len = feb_cbor_encode_wardriving_command_payload(out, sizeof(out), &payload);
        CHECK(out_len > 0, "WARDRIVING_CMD_START_BOTH: encode succeeds");

        feb_wardriving_command_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_wardriving_command_payload(out, out_len, &decoded);
        CHECK(status == FEB_CBOR_OK, "WARDRIVING_CMD_START_BOTH: decode status OK");
        CHECK(
            decoded.action_len == strlen("start") && memcmp(decoded.action, "start", decoded.action_len) == 0,
            "WARDRIVING_CMD_START_BOTH: action == \"start\"");
        CHECK(decoded.has_sources == 1 && decoded.source_count == 2, "WARDRIVING_CMD_START_BOTH: 2 sources");
        CHECK(decoded.has_wifi_interval_ms == 1 && decoded.wifi_interval_ms == 30, "WARDRIVING_CMD_START_BOTH: wifi_interval_ms == 30");
        CHECK(
            decoded.has_ble_params == 1 && decoded.ble_window_ms == 30 && decoded.ble_interval_ms == 30,
            "WARDRIVING_CMD_START_BOTH: ble params == 30/30");

        uint8_t reencoded[128];
        size_t reencoded_len = feb_cbor_encode_wardriving_command_payload(reencoded, sizeof(reencoded), &decoded);
        CHECK(bytes_equal(reencoded, reencoded_len, out, out_len), "WARDRIVING_CMD_START_BOTH: re-encode byte-identical");
    }

    /* start, wifi source only, conservative point-1 interval */
    {
        feb_wardriving_command_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.action = "start";
        payload.action_len = strlen(payload.action);
        payload.sources[0] = "wifi";
        payload.source_lens[0] = strlen("wifi");
        payload.source_count = 1;
        payload.has_sources = 1;
        payload.wifi_interval_ms = 30000;
        payload.has_wifi_interval_ms = 1;

        uint8_t out[128];
        size_t out_len = feb_cbor_encode_wardriving_command_payload(out, sizeof(out), &payload);
        CHECK(out_len > 0, "WARDRIVING_CMD_START_WIFI_ONLY: encode succeeds");

        feb_wardriving_command_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_wardriving_command_payload(out, out_len, &decoded);
        CHECK(status == FEB_CBOR_OK, "WARDRIVING_CMD_START_WIFI_ONLY: decode status OK");
        CHECK(decoded.source_count == 1, "WARDRIVING_CMD_START_WIFI_ONLY: 1 source");
        CHECK(decoded.has_wifi_interval_ms == 1 && decoded.wifi_interval_ms == 30000, "WARDRIVING_CMD_START_WIFI_ONLY: wifi_interval_ms == 30000");
        CHECK(decoded.has_ble_params == 0, "WARDRIVING_CMD_START_WIFI_ONLY: no ble params");
    }

    /* start, ble source only */
    {
        feb_wardriving_command_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.action = "start";
        payload.action_len = strlen(payload.action);
        payload.sources[0] = "ble";
        payload.source_lens[0] = strlen("ble");
        payload.source_count = 1;
        payload.has_sources = 1;
        payload.ble_window_ms = 100;
        payload.ble_interval_ms = 1000;
        payload.has_ble_params = 1;

        uint8_t out[128];
        size_t out_len = feb_cbor_encode_wardriving_command_payload(out, sizeof(out), &payload);
        CHECK(out_len > 0, "WARDRIVING_CMD_START_BLE_ONLY: encode succeeds");

        feb_wardriving_command_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_wardriving_command_payload(out, out_len, &decoded);
        CHECK(status == FEB_CBOR_OK, "WARDRIVING_CMD_START_BLE_ONLY: decode status OK");
        CHECK(decoded.has_wifi_interval_ms == 0, "WARDRIVING_CMD_START_BLE_ONLY: no wifi_interval_ms");
        CHECK(
            decoded.has_ble_params == 1 && decoded.ble_window_ms == 100 && decoded.ble_interval_ms == 1000,
            "WARDRIVING_CMD_START_BLE_ONLY: ble params == 100/1000");
    }

    /* status query */
    {
        feb_wardriving_command_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.action = "status";
        payload.action_len = strlen(payload.action);

        uint8_t out[32];
        size_t out_len = feb_cbor_encode_wardriving_command_payload(out, sizeof(out), &payload);
        CHECK(out_len > 0, "WARDRIVING_CMD_STATUS_QUERY: encode succeeds");

        feb_wardriving_command_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_wardriving_command_payload(out, out_len, &decoded);
        CHECK(status == FEB_CBOR_OK, "WARDRIVING_CMD_STATUS_QUERY: decode status OK");
        CHECK(
            decoded.action_len == strlen("status") && memcmp(decoded.action, "status", decoded.action_len) == 0,
            "WARDRIVING_CMD_STATUS_QUERY: action == \"status\"");
        CHECK(decoded.has_sources == 0, "WARDRIVING_CMD_STATUS_QUERY: no sources");
        CHECK(decoded.has_wifi_interval_ms == 0 && decoded.has_ble_params == 0, "WARDRIVING_CMD_STATUS_QUERY: no interval fields");
    }

    /* stop */
    {
        feb_wardriving_command_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.action = "stop";
        payload.action_len = strlen(payload.action);

        uint8_t out[32];
        size_t out_len = feb_cbor_encode_wardriving_command_payload(out, sizeof(out), &payload);
        CHECK(out_len > 0, "WARDRIVING_CMD_STOP: encode succeeds");

        feb_wardriving_command_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_wardriving_command_payload(out, out_len, &decoded);
        CHECK(status == FEB_CBOR_OK, "WARDRIVING_CMD_STOP: decode status OK");
        CHECK(
            decoded.action_len == strlen("stop") && memcmp(decoded.action, "stop", decoded.action_len) == 0,
            "WARDRIVING_CMD_STOP: action == \"stop\"");
        CHECK(decoded.has_sources == 0, "WARDRIVING_CMD_STOP: no sources");
        CHECK(decoded.has_wifi_interval_ms == 0 && decoded.has_ble_params == 0, "WARDRIVING_CMD_STOP: no interval fields");
    }

    /* malformed: field at position 1 is "wifi_interval_ms", not the required "sources"
       -- fixed field order violation. */
    {
        uint8_t buf[64];
        size_t pos = 0;
        size_t n;
        n = feb_cbor_encode_map_header(buf, sizeof(buf), 2);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "action", sizeof("action") - 1);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "start", sizeof("start") - 1);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "wifi_interval_ms", sizeof("wifi_interval_ms") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 30000);
        pos += n;

        feb_wardriving_command_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_wardriving_command_payload(buf, pos, &decoded);
        CHECK(
            status == FEB_CBOR_ERR_OUT_OF_ORDER,
            "WARDRIVING_CMD_BAD_ORDER: rejected (sources skipped, out of order)");
    }
}

static void test_wardriving_record_and_status_result_codec(void) {
    feb_wardriving_record_t wifi_record;
    memset(&wifi_record, 0, sizeof(wifi_record));
    wifi_record.timestamp_ms = 12345;
    wifi_record.utc_timestamp_s = 1700000000;
    wifi_record.lat_e7_offset = 900000000u + 12345678u;
    wifi_record.lon_e7_offset = 1800000000u + 98765432u;
    wifi_record.source = "wifi";
    wifi_record.source_len = strlen(wifi_record.source);
    static const uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    wifi_record.payload_kind = FEB_WARDRIVING_PAYLOAD_WIFI;
    wifi_record.payload.wifi.ssid = (const uint8_t*)"TestAP";
    wifi_record.payload.wifi.ssid_len = strlen("TestAP");
    memcpy(wifi_record.payload.wifi.bssid, bssid, FEB_WIFI_SCAN_BSSID_LEN);
    wifi_record.payload.wifi.rssi_offset = 78;
    wifi_record.payload.wifi.channel = 6;
    wifi_record.payload.wifi.auth = "wpa2_psk";
    wifi_record.payload.wifi.auth_len = strlen(wifi_record.payload.wifi.auth);

    feb_wardriving_record_t ble_record;
    memset(&ble_record, 0, sizeof(ble_record));
    ble_record.timestamp_ms = 67890;
    ble_record.utc_timestamp_s = 1700000123;
    ble_record.lat_e7_offset = 900000000u + 11111111u;
    ble_record.lon_e7_offset = 1800000000u + 22222222u;
    ble_record.source = "ble";
    ble_record.source_len = strlen(ble_record.source);
    static const uint8_t ble_addr[FEB_BLE_SCAN_ADDRESS_LEN] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    ble_record.payload_kind = FEB_WARDRIVING_PAYLOAD_BLE;
    memcpy(ble_record.payload.ble.address, ble_addr, FEB_BLE_SCAN_ADDRESS_LEN);
    ble_record.payload.ble.name = "Widget";
    ble_record.payload.ble.name_len = strlen(ble_record.payload.ble.name);
    ble_record.payload.ble.has_name = 1;
    ble_record.payload.ble.rssi_offset = 100;

    /* per-record round trip */
    {
        uint8_t out[256];
        size_t out_len = feb_cbor_encode_wardriving_record(out, sizeof(out), &wifi_record);
        CHECK(out_len > 0, "WARDRIVING_RECORD_WIFI: encode succeeds");
        feb_cbor_status_t status;
        feb_wardriving_record_t decoded;
        size_t n = feb_cbor_decode_wardriving_record(out, out_len, &decoded, &status);
        CHECK(n == out_len && status == FEB_CBOR_OK, "WARDRIVING_RECORD_WIFI: decode consumes whole buffer");
        CHECK(decoded.timestamp_ms == 12345, "WARDRIVING_RECORD_WIFI: timestamp_ms matches");
        CHECK(decoded.utc_timestamp_s == 1700000000, "WARDRIVING_RECORD_WIFI: utc_timestamp_s matches");
        CHECK(
            decoded.payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI,
            "WARDRIVING_RECORD_WIFI: payload_kind matches");
        CHECK(
            decoded.payload.wifi.ssid_len == strlen("TestAP") &&
                memcmp(decoded.payload.wifi.ssid, "TestAP", decoded.payload.wifi.ssid_len) == 0,
            "WARDRIVING_RECORD_WIFI: ssid matches");
        CHECK(decoded.payload.wifi.channel == 6, "WARDRIVING_RECORD_WIFI: channel matches");
    }
    {
        uint8_t out[256];
        size_t out_len = feb_cbor_encode_wardriving_record(out, sizeof(out), &ble_record);
        CHECK(out_len > 0, "WARDRIVING_RECORD_BLE: encode succeeds");
        feb_cbor_status_t status;
        feb_wardriving_record_t decoded;
        size_t n = feb_cbor_decode_wardriving_record(out, out_len, &decoded, &status);
        CHECK(n == out_len && status == FEB_CBOR_OK, "WARDRIVING_RECORD_BLE: decode consumes whole buffer");
        CHECK(decoded.utc_timestamp_s == 1700000123, "WARDRIVING_RECORD_BLE: utc_timestamp_s matches");
        CHECK(
            decoded.payload_kind == FEB_WARDRIVING_PAYLOAD_BLE,
            "WARDRIVING_RECORD_BLE: payload_kind matches");
        CHECK(decoded.payload.ble.has_name == 1, "WARDRIVING_RECORD_BLE: has_name set");
        CHECK(
            decoded.payload.ble.name_len == strlen("Widget") &&
                memcmp(decoded.payload.ble.name, "Widget", decoded.payload.ble.name_len) == 0,
            "WARDRIVING_RECORD_BLE: name matches");
        CHECK(decoded.payload.ble.rssi_offset == 100, "WARDRIVING_RECORD_BLE: rssi_offset matches");
    }

    /* malformed: unknown source value -- rejected at this codec's own layer, unlike
       command's action/sources which are left to the dispatch layer. */
    {
        uint8_t buf[128];
        size_t pos = 0;
        size_t n;
        n = feb_cbor_encode_map_header(buf, sizeof(buf), 6);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "timestamp_ms", sizeof("timestamp_ms") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 1);
        pos += n;
        n = feb_cbor_encode_text(
            buf + pos, sizeof(buf) - pos, "utc_timestamp_s", sizeof("utc_timestamp_s") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 1700000000u);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "lat_e7_offset", sizeof("lat_e7_offset") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 900000000u);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "lon_e7_offset", sizeof("lon_e7_offset") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 1800000000u);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "source", sizeof("source") - 1);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "cellular", sizeof("cellular") - 1);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "payload", sizeof("payload") - 1);
        pos += n;
        n = feb_cbor_encode_map_header(buf + pos, sizeof(buf) - pos, 0);
        pos += n;

        feb_cbor_status_t status;
        feb_wardriving_record_t decoded;
        size_t decode_n = feb_cbor_decode_wardriving_record(buf, pos, &decoded, &status);
        CHECK(
            decode_n == 0 && status == FEB_CBOR_ERR_UNEXPECTED_TYPE,
            "WARDRIVING_RECORD_BAD_SOURCE: rejected (unknown source value)");
    }

    /* status_result_payload: one wifi record + one ble record, non-zero backlog_remaining */
    static feb_wardriving_status_result_payload_t result_payload;
    memset(&result_payload, 0, sizeof(result_payload));
    result_payload.records[0] = wifi_record;
    result_payload.records[1] = ble_record;
    result_payload.record_count = 2;
    result_payload.backlog_remaining = 5;

    static uint8_t result_out[512];
    size_t result_out_len =
        feb_cbor_encode_wardriving_status_result_payload(result_out, sizeof(result_out), &result_payload);
    CHECK(result_out_len > 0, "WARDRIVING_STATUS_RESULT: encode succeeds");

    static feb_wardriving_status_result_payload_t result_decoded;
    feb_cbor_status_t result_status =
        feb_cbor_decode_wardriving_status_result_payload(result_out, result_out_len, &result_decoded);
    CHECK(result_status == FEB_CBOR_OK, "WARDRIVING_STATUS_RESULT: decode status OK");
    CHECK(result_decoded.record_count == 2, "WARDRIVING_STATUS_RESULT: record_count == 2");
    CHECK(result_decoded.backlog_remaining == 5, "WARDRIVING_STATUS_RESULT: backlog_remaining == 5");

    /* Nesting-depth check (docs/PROTOCOL.md "wardriving command and status payloads" ->
       "Nesting depth"): wrap this exact nested result as status.result (state="data",
       request_id=0 per the unsolicited-backlog-drain convention) and decode through the
       full generic feb_cbor_decode_status_payload() path, which captures `result` via a
       fresh depth-0 feb_cbor_skip_value() call -- this is the actual call site whose depth
       budget PROTOCOL.md documents landing exactly at FEB_CBOR_MAX_NESTING (result map(0)
       -> records array(1) -> <wardriving-record> map(2) -> payload map(3) -> payload's own
       scalar fields(4)). A regression here would surface as FEB_CBOR_ERR_TOO_DEEP. */
    {
        feb_status_payload_t status_payload;
        memset(&status_payload, 0, sizeof(status_payload));
        status_payload.request_id = 0;
        status_payload.state = "data";
        status_payload.state_len = strlen(status_payload.state);
        status_payload.has_result = 1;
        status_payload.result_span = result_out;
        status_payload.result_span_len = result_out_len;

        static uint8_t status_out[600];
        size_t status_out_len = feb_cbor_encode_status_payload(status_out, sizeof(status_out), &status_payload);
        CHECK(status_out_len > 0, "WARDRIVING_STATUS_DATA: encode succeeds");

        feb_status_payload_t status_decoded;
        feb_cbor_status_t decode_status =
            feb_cbor_decode_status_payload(status_out, status_out_len, &status_decoded);
        CHECK(
            decode_status == FEB_CBOR_OK,
            "WARDRIVING_STATUS_DATA: full nested result decodes without exceeding FEB_CBOR_MAX_NESTING");
        CHECK(status_decoded.request_id == 0, "WARDRIVING_STATUS_DATA: request_id == 0 (unsolicited sentinel)");

        static feb_wardriving_status_result_payload_t reparsed;
        feb_cbor_status_t reparsed_status = feb_cbor_decode_wardriving_status_result_payload(
            status_decoded.result_span, status_decoded.result_span_len, &reparsed);
        CHECK(reparsed_status == FEB_CBOR_OK, "WARDRIVING_STATUS_DATA: nested result re-decodes OK");
        CHECK(reparsed.record_count == 2, "WARDRIVING_STATUS_DATA: nested result has 2 records");
    }
}

/* ---- shared cross-firmware vectors (tests/vectors/vectors.h), <wardriving-record> with
   the new utc_timestamp_s field -- mirrors tests/esp32/test_framing_cbor.c's
   test_wardriving_record_roundtrip()/test_wardriving_status_result_payload() exactly, so a
   divergence between the two firmwares' codecs shows up as a Flipper-side test failure
   too, not just an ESP32-side one. */
static void test_wardriving_record_and_status_result_vectors(void) {
    feb_wardriving_record_t record;
    feb_cbor_status_t status;
    size_t consumed;
    uint8_t encode_buf[256];
    size_t encoded_len;

    consumed = feb_cbor_decode_wardriving_record(
        FEB_VEC_WARDRIVING_RECORD_WIFI, FEB_VEC_WARDRIVING_RECORD_WIFI_LEN, &record, &status);
    CHECK(
        consumed == FEB_VEC_WARDRIVING_RECORD_WIFI_LEN && status == FEB_CBOR_OK,
        "VEC_WARDRIVING_RECORD_WIFI: decode consumes whole vector");
    CHECK(
        record.timestamp_ms == 1000 && record.utc_timestamp_s == 1757667010 &&
            record.payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI,
        "VEC_WARDRIVING_RECORD_WIFI: timestamp_ms/utc_timestamp_s/payload_kind match");
    CHECK(
        record.payload.wifi.ssid_len == strlen("TestNetwork") &&
            memcmp(record.payload.wifi.ssid, "TestNetwork", record.payload.wifi.ssid_len) == 0 &&
            record.payload.wifi.channel == 6,
        "VEC_WARDRIVING_RECORD_WIFI: ssid/channel match");
    encoded_len = feb_cbor_encode_wardriving_record(encode_buf, sizeof(encode_buf), &record);
    CHECK(
        bytes_equal(encode_buf, encoded_len, FEB_VEC_WARDRIVING_RECORD_WIFI, FEB_VEC_WARDRIVING_RECORD_WIFI_LEN),
        "VEC_WARDRIVING_RECORD_WIFI: encode round-trip byte-identical");

    consumed = feb_cbor_decode_wardriving_record(
        FEB_VEC_WARDRIVING_RECORD_BLE, FEB_VEC_WARDRIVING_RECORD_BLE_LEN, &record, &status);
    CHECK(
        consumed == FEB_VEC_WARDRIVING_RECORD_BLE_LEN && status == FEB_CBOR_OK,
        "VEC_WARDRIVING_RECORD_BLE: decode consumes whole vector");
    CHECK(
        record.payload_kind == FEB_WARDRIVING_PAYLOAD_BLE && record.payload.ble.has_name &&
            record.payload.ble.name_len == strlen("MyPhone") &&
            memcmp(record.payload.ble.name, "MyPhone", record.payload.ble.name_len) == 0,
        "VEC_WARDRIVING_RECORD_BLE: payload_kind/name match");
    encoded_len = feb_cbor_encode_wardriving_record(encode_buf, sizeof(encode_buf), &record);
    CHECK(
        bytes_equal(encode_buf, encoded_len, FEB_VEC_WARDRIVING_RECORD_BLE, FEB_VEC_WARDRIVING_RECORD_BLE_LEN),
        "VEC_WARDRIVING_RECORD_BLE: encode round-trip byte-identical");

    consumed = feb_cbor_decode_wardriving_record(
        FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME,
        FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME_LEN,
        &record,
        &status);
    CHECK(
        consumed == FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME_LEN && status == FEB_CBOR_OK,
        "VEC_WARDRIVING_RECORD_BLE_NO_NAME: decode consumes whole vector");
    CHECK(
        record.payload_kind == FEB_WARDRIVING_PAYLOAD_BLE && !record.payload.ble.has_name,
        "VEC_WARDRIVING_RECORD_BLE_NO_NAME: has_name is 0");
    encoded_len = feb_cbor_encode_wardriving_record(encode_buf, sizeof(encode_buf), &record);
    CHECK(
        bytes_equal(
            encode_buf,
            encoded_len,
            FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME,
            FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME_LEN),
        "VEC_WARDRIVING_RECORD_BLE_NO_NAME: encode round-trip byte-identical");

    /* "data" state through the real generic status_payload decoder. */
    {
        feb_status_payload_t st;
        static feb_wardriving_status_result_payload_t result;
        feb_cbor_status_t st_status = feb_cbor_decode_status_payload(
            FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD, FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD_LEN, &st);
        CHECK(
            st_status == FEB_CBOR_OK && st.request_id == 0 && st.has_result &&
                st.state_len == strlen("data") && memcmp(st.state, "data", st.state_len) == 0,
            "VEC_WARDRIVING_STATUS_DATA: request_id==0 (unsolicited sentinel), state, has_result");

        feb_cbor_status_t result_status =
            feb_cbor_decode_wardriving_status_result_payload(st.result_span, st.result_span_len, &result);
        CHECK(
            result_status == FEB_CBOR_OK && result.record_count == 2 && result.backlog_remaining == 3 &&
                result.records[0].payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI &&
                result.records[1].payload_kind == FEB_WARDRIVING_PAYLOAD_BLE,
            "VEC_WARDRIVING_STATUS_DATA: result has 1 wifi + 1 ble record, backlog_remaining matches");

        static uint8_t result_encode_buf[FEB_CBOR_MAX_PAYLOAD];
        size_t result_encoded_len = feb_cbor_encode_wardriving_status_result_payload(
            result_encode_buf, sizeof(result_encode_buf), &result);
        CHECK(
            bytes_equal(
                result_encode_buf,
                result_encoded_len,
                FEB_VEC_WARDRIVING_RESULT_MIXED,
                FEB_VEC_WARDRIVING_RESULT_MIXED_LEN),
            "VEC_WARDRIVING_STATUS_DATA: result encode round-trip byte-identical");

        static uint8_t status_encode_buf[FEB_CBOR_MAX_PAYLOAD];
        size_t status_encoded_len =
            feb_cbor_encode_status_payload(status_encode_buf, sizeof(status_encode_buf), &st);
        CHECK(
            bytes_equal(
                status_encode_buf,
                status_encoded_len,
                FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD,
                FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD_LEN),
            "VEC_WARDRIVING_STATUS_DATA: full status payload encode round-trip byte-identical");
    }

    /* "started"/"stopped": no result field. */
    {
        feb_status_payload_t st;
        feb_cbor_status_t st_status = feb_cbor_decode_status_payload(
            FEB_VEC_WARDRIVING_STATUS_STARTED_PAYLOAD,
            FEB_VEC_WARDRIVING_STATUS_STARTED_PAYLOAD_LEN,
            &st);
        CHECK(
            st_status == FEB_CBOR_OK && st.request_id == 501 && !st.has_result &&
                st.state_len == strlen("started") && memcmp(st.state, "started", st.state_len) == 0,
            "VEC_WARDRIVING_STATUS_STARTED: decodes request_id/state, no result field");
    }
    {
        feb_status_payload_t st;
        feb_cbor_status_t st_status = feb_cbor_decode_status_payload(
            FEB_VEC_WARDRIVING_STATUS_STOPPED_PAYLOAD,
            FEB_VEC_WARDRIVING_STATUS_STOPPED_PAYLOAD_LEN,
            &st);
        CHECK(
            st_status == FEB_CBOR_OK && st.request_id == 502 && !st.has_result &&
                st.state_len == strlen("stopped") && memcmp(st.state, "stopped", st.state_len) == 0,
            "VEC_WARDRIVING_STATUS_STOPPED: decodes request_id/state, no result field");
    }
}

/* Mirrors tests/esp32/test_framing_cbor.c's test_gps_command_payload()/
   test_gps_status_payload() against the same shared vectors. */
static void test_gps_payload_vectors(void) {
    {
        feb_command_payload_t cmd;
        size_t arg_count;
        feb_cbor_status_t status;
        uint8_t encode_buf[128];
        size_t encoded_len;

        feb_cbor_status_t cmd_status = feb_cbor_decode_command_payload(
            FEB_VEC_GPS_COMMAND_PAYLOAD, FEB_VEC_GPS_COMMAND_PAYLOAD_LEN, &cmd);
        CHECK(
            cmd_status == FEB_CBOR_OK && cmd.capability_len == strlen("gps") &&
                memcmp(cmd.capability, "gps", cmd.capability_len) == 0 && cmd.request_id == 601 &&
                feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) >
                    0 &&
                arg_count == 0,
            "VEC_GPS_COMMAND: decodes capability/request_id/empty arguments");

        encoded_len = feb_cbor_encode_command_payload(encode_buf, sizeof(encode_buf), &cmd);
        CHECK(
            bytes_equal(encode_buf, encoded_len, FEB_VEC_GPS_COMMAND_PAYLOAD, FEB_VEC_GPS_COMMAND_PAYLOAD_LEN),
            "VEC_GPS_COMMAND: encode round-trip byte-identical");

        cmd_status = feb_cbor_decode_command_payload(
            FEB_VEC_GPS_COMMAND_BAD_ARGUMENTS_PAYLOAD, FEB_VEC_GPS_COMMAND_BAD_ARGUMENTS_PAYLOAD_LEN, &cmd);
        CHECK(
            cmd_status == FEB_CBOR_OK &&
                feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) >
                    0 &&
                arg_count == 1,
            "VEC_GPS_COMMAND_BAD_ARGUMENTS: decodes structurally OK (rejection is main.c's "
            "invalid_command dispatch concern, not a codec error)");
    }

    {
        feb_status_payload_t st;
        feb_cbor_status_t status = feb_cbor_decode_status_payload(
            FEB_VEC_GPS_STATUS_NO_SIGNAL_PAYLOAD, FEB_VEC_GPS_STATUS_NO_SIGNAL_PAYLOAD_LEN, &st);
        CHECK(
            status == FEB_CBOR_OK && st.request_id == 601 && !st.has_result &&
                st.state_len == strlen("no_signal") && memcmp(st.state, "no_signal", st.state_len) == 0,
            "VEC_GPS_STATUS_NO_SIGNAL: decodes request_id/state, no result field");
    }
    {
        feb_status_payload_t st;
        feb_cbor_status_t status = feb_cbor_decode_status_payload(
            FEB_VEC_GPS_STATUS_ACQUIRING_PAYLOAD, FEB_VEC_GPS_STATUS_ACQUIRING_PAYLOAD_LEN, &st);
        CHECK(
            status == FEB_CBOR_OK && st.request_id == 601 && !st.has_result &&
                st.state_len == strlen("acquiring") && memcmp(st.state, "acquiring", st.state_len) == 0,
            "VEC_GPS_STATUS_ACQUIRING: decodes request_id/state, no result field");
    }
    {
        feb_status_payload_t st;
        feb_gps_result_payload_t result;
        uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
        size_t encoded_len;

        feb_cbor_status_t status = feb_cbor_decode_status_payload(
            FEB_VEC_GPS_STATUS_FIX_PAYLOAD, FEB_VEC_GPS_STATUS_FIX_PAYLOAD_LEN, &st);
        CHECK(
            status == FEB_CBOR_OK && st.request_id == 601 && st.has_result &&
                st.state_len == strlen("fix") && memcmp(st.state, "fix", st.state_len) == 0,
            "VEC_GPS_STATUS_FIX: decodes request_id/state/has_result");

        feb_cbor_status_t result_status =
            feb_cbor_decode_gps_result_payload(st.result_span, st.result_span_len, &result);
        CHECK(
            result_status == FEB_CBOR_OK && result.fix_quality == 1 && result.satellites == 9 &&
                result.hdop_e1 == 20 && result.utc_timestamp_s == 1757667010 &&
                result.altitude_dm_offset == 1000529,
            "VEC_GPS_STATUS_FIX: result decodes fix_quality/satellites/hdop_e1/utc_timestamp_s/"
            "altitude_dm_offset");

        encoded_len = feb_cbor_encode_gps_result_payload(encode_buf, sizeof(encode_buf), &result);
        CHECK(
            bytes_equal(encode_buf, encoded_len, FEB_VEC_GPS_RESULT_FIX, FEB_VEC_GPS_RESULT_FIX_LEN),
            "VEC_GPS_STATUS_FIX: result encode round-trip byte-identical");

        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        CHECK(
            bytes_equal(
                encode_buf, encoded_len, FEB_VEC_GPS_STATUS_FIX_PAYLOAD, FEB_VEC_GPS_STATUS_FIX_PAYLOAD_LEN),
            "VEC_GPS_STATUS_FIX: full status payload encode round-trip byte-identical");
    }
}

/* ---- gps status.result payload (docs/PROTOCOL.md "`gps` command and status payloads",
   frozen 2026-09-12) ---- */
static void test_gps_status_result_payload_codec(void) {
    feb_gps_result_payload_t payload;
    payload.lat_e7_offset = 900000000u + 12345678u;
    payload.lon_e7_offset = 1800000000u + 98765432u;
    payload.fix_quality = 1;
    payload.satellites = 7;
    payload.hdop_e1 = 23;
    payload.utc_timestamp_s = 1700000000;
    payload.altitude_dm_offset = 1000123;

    /* per-field round trip */
    {
        uint8_t out[128];
        size_t out_len = feb_cbor_encode_gps_result_payload(out, sizeof(out), &payload);
        CHECK(out_len > 0, "GPS_RESULT: encode succeeds");

        feb_gps_result_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_gps_result_payload(out, out_len, &decoded);
        CHECK(status == FEB_CBOR_OK, "GPS_RESULT: decode status OK");
        CHECK(decoded.lat_e7_offset == payload.lat_e7_offset, "GPS_RESULT: lat_e7_offset matches");
        CHECK(decoded.lon_e7_offset == payload.lon_e7_offset, "GPS_RESULT: lon_e7_offset matches");
        CHECK(decoded.fix_quality == 1, "GPS_RESULT: fix_quality matches");
        CHECK(decoded.satellites == 7, "GPS_RESULT: satellites matches");
        CHECK(decoded.hdop_e1 == 23, "GPS_RESULT: hdop_e1 matches");
        CHECK(decoded.utc_timestamp_s == 1700000000, "GPS_RESULT: utc_timestamp_s matches");
        CHECK(
            decoded.altitude_dm_offset == 1000123, "GPS_RESULT: altitude_dm_offset matches");
    }

    /* Full status(state="fix") envelope round trip -- confirms the result span
       feb_cbor_decode_status_payload() captures generically re-decodes correctly through
       feb_cbor_decode_gps_result_payload(), same pattern as
       WARDRIVING_STATUS_DATA above (this shape is flat, well within FEB_CBOR_MAX_NESTING). */
    {
        uint8_t result_out[128];
        size_t result_out_len =
            feb_cbor_encode_gps_result_payload(result_out, sizeof(result_out), &payload);
        CHECK(result_out_len > 0, "GPS_STATUS_FIX: result encode succeeds");

        feb_status_payload_t status_payload;
        memset(&status_payload, 0, sizeof(status_payload));
        status_payload.request_id = 1;
        status_payload.state = "fix";
        status_payload.state_len = strlen(status_payload.state);
        status_payload.has_result = 1;
        status_payload.result_span = result_out;
        status_payload.result_span_len = result_out_len;

        uint8_t status_out[256];
        size_t status_out_len =
            feb_cbor_encode_status_payload(status_out, sizeof(status_out), &status_payload);
        CHECK(status_out_len > 0, "GPS_STATUS_FIX: status encode succeeds");

        feb_status_payload_t status_decoded;
        feb_cbor_status_t decode_status =
            feb_cbor_decode_status_payload(status_out, status_out_len, &status_decoded);
        CHECK(decode_status == FEB_CBOR_OK, "GPS_STATUS_FIX: status decodes OK");
        CHECK(
            status_decoded.state_len == strlen("fix") &&
                memcmp(status_decoded.state, "fix", status_decoded.state_len) == 0,
            "GPS_STATUS_FIX: state == \"fix\"");
        CHECK(status_decoded.has_result == 1, "GPS_STATUS_FIX: has_result set");

        feb_gps_result_payload_t reparsed;
        feb_cbor_status_t reparsed_status = feb_cbor_decode_gps_result_payload(
            status_decoded.result_span, status_decoded.result_span_len, &reparsed);
        CHECK(reparsed_status == FEB_CBOR_OK, "GPS_STATUS_FIX: nested result re-decodes OK");
        CHECK(
            reparsed.utc_timestamp_s == 1700000000,
            "GPS_STATUS_FIX: nested result utc_timestamp_s matches");
    }

    /* malformed: missing field (6 of 7 fields present, altitude_dm_offset omitted) */
    {
        uint8_t buf[128];
        size_t pos = 0;
        size_t n;
        n = feb_cbor_encode_map_header(buf, sizeof(buf), 6);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "lat_e7_offset", sizeof("lat_e7_offset") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 900000000u);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "lon_e7_offset", sizeof("lon_e7_offset") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 1800000000u);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "fix_quality", sizeof("fix_quality") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 1);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "satellites", sizeof("satellites") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 7);
        pos += n;
        n = feb_cbor_encode_text(buf + pos, sizeof(buf) - pos, "hdop_e1", sizeof("hdop_e1") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 23);
        pos += n;
        n = feb_cbor_encode_text(
            buf + pos, sizeof(buf) - pos, "utc_timestamp_s", sizeof("utc_timestamp_s") - 1);
        pos += n;
        n = feb_cbor_encode_uint(buf + pos, sizeof(buf) - pos, 1700000000);
        pos += n;
        /* altitude_dm_offset deliberately omitted */

        feb_gps_result_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_gps_result_payload(buf, pos, &decoded);
        CHECK(
            status == FEB_CBOR_ERR_MISSING_FIELD,
            "GPS_RESULT_MISSING_FIELD: rejected (altitude_dm_offset omitted)");
    }
}

/* ---- wardriving_csv (docs/CAPABILITIES.md's wardriving bullet: WiGLE CSV export) ----
   Pure formatting/arithmetic only (no Furi/Storage dependency) -- see wardriving_csv.h's
   own scope note for why this is host-testable at all. */

static void test_wardriving_csv_format_header(void) {
    char out[FEB_WARDRIVING_CSV_HEADER_MAX_LEN];
    size_t n = feb_wardriving_csv_format_header(out, sizeof(out));
    CHECK(n > 0, "CSV_HEADER: format succeeds");
    CHECK(
        n >= 14 && memcmp(out, "WigleWifi-1.4,", 14) == 0,
        "CSV_HEADER: metadata line starts with WigleWifi-1.4,");
    CHECK(
        strstr(out, "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
                    "CurrentLongitude,AltitudeMeters,AccuracyMeters,Type") != NULL,
        "CSV_HEADER: contains the WigleWifi-1.4 column header line");
    CHECK(out[n - 1] == '\n', "CSV_HEADER: ends with a newline");

    /* Too-small out_cap fails cleanly rather than writing a truncated/corrupt header. */
    char tiny[4];
    CHECK(
        feb_wardriving_csv_format_header(tiny, sizeof(tiny)) == 0,
        "CSV_HEADER: too-small out_cap returns 0, not a truncated line");
}

static void test_wardriving_csv_format_row(void) {
    /* wifi record: SSID containing a comma (must be quoted), auth lowercased on the wire,
       uppercased in the CSV AuthMode column. */
    feb_wardriving_record_t wifi_record;
    memset(&wifi_record, 0, sizeof(wifi_record));
    wifi_record.timestamp_ms = 12345;
    wifi_record.lat_e7_offset = 900000000u + 12345678u; /* -> 1.2345678 */
    wifi_record.lon_e7_offset = 1800000000u - 98765432u; /* -> -9.8765432 */
    wifi_record.source = "wifi";
    wifi_record.source_len = strlen(wifi_record.source);
    wifi_record.payload_kind = FEB_WARDRIVING_PAYLOAD_WIFI;
    static const uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    memcpy(wifi_record.payload.wifi.bssid, bssid, FEB_WIFI_SCAN_BSSID_LEN);
    wifi_record.payload.wifi.ssid = (const uint8_t*)"Cafe, Free WiFi";
    wifi_record.payload.wifi.ssid_len = strlen("Cafe, Free WiFi");
    wifi_record.payload.wifi.rssi_offset = 78; /* -50 dBm */
    wifi_record.payload.wifi.channel = 6;
    wifi_record.payload.wifi.auth = "wpa2_psk";
    wifi_record.payload.wifi.auth_len = strlen("wpa2_psk");

    static const char first_seen[] = "2026-09-08 12:00:00";
    char out[FEB_WARDRIVING_CSV_ROW_MAX_LEN];
    size_t n = feb_wardriving_csv_format_row(
        out, sizeof(out), &wifi_record, first_seen, strlen(first_seen));
    CHECK(n > 0, "CSV_ROW_WIFI: format succeeds");
    out[n] = '\0';
    CHECK(strstr(out, "aa:bb:cc:dd:ee:ff") != NULL, "CSV_ROW_WIFI: MAC formatted as hex pairs");
    CHECK(strstr(out, "\"Cafe, Free WiFi\"") != NULL, "CSV_ROW_WIFI: comma-bearing SSID is quoted");
    CHECK(strstr(out, "WPA2_PSK") != NULL, "CSV_ROW_WIFI: AuthMode uppercased");
    CHECK(strstr(out, "2026-09-08 12:00:00") != NULL, "CSV_ROW_WIFI: FirstSeen carried through verbatim");
    CHECK(
        strstr(out, ",6,2437,") != NULL,
        "CSV_ROW_WIFI: channel 6 and its derived frequency (2407+5*6=2437MHz) both present");
    CHECK(strstr(out, "-50") != NULL, "CSV_ROW_WIFI: RSSI decoded from offset (78-128=-50)");
    CHECK(strstr(out, "1.2345678") != NULL, "CSV_ROW_WIFI: latitude decoded from lat_e7_offset");
    CHECK(strstr(out, "-9.8765432") != NULL, "CSV_ROW_WIFI: longitude decoded from lon_e7_offset");
    CHECK(strstr(out, ",WIFI") != NULL, "CSV_ROW_WIFI: Type == WIFI");
    CHECK(out[n - 1] == '\n', "CSV_ROW_WIFI: row ends with a newline");

    /* Channel 14 is the one non-linear case (Japan-only 802.11b, 2484MHz instead of the
       2407+5*14=2477MHz the regular formula would give) -- worth its own regression check. */
    wifi_record.payload.wifi.channel = 14;
    size_t n14 = feb_wardriving_csv_format_row(
        out, sizeof(out), &wifi_record, first_seen, strlen(first_seen));
    CHECK(n14 > 0, "CSV_ROW_WIFI_CH14: format succeeds");
    out[n14] = '\0';
    CHECK(
        strstr(out, ",14,2484,") != NULL,
        "CSV_ROW_WIFI_CH14: channel 14's frequency is 2484MHz, not the linear-formula 2477MHz");

    /* ble record: no name (blank SSID field), no channel (blank Channel field), no auth. */
    feb_wardriving_record_t ble_record;
    memset(&ble_record, 0, sizeof(ble_record));
    ble_record.timestamp_ms = 67890;
    ble_record.lat_e7_offset = 900000000u;
    ble_record.lon_e7_offset = 1800000000u;
    ble_record.source = "ble";
    ble_record.source_len = strlen(ble_record.source);
    ble_record.payload_kind = FEB_WARDRIVING_PAYLOAD_BLE;
    static const uint8_t ble_addr[FEB_BLE_SCAN_ADDRESS_LEN] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    memcpy(ble_record.payload.ble.address, ble_addr, FEB_BLE_SCAN_ADDRESS_LEN);
    ble_record.payload.ble.has_name = 0;
    ble_record.payload.ble.rssi_offset = 100; /* -28 dBm */

    size_t ble_n = feb_wardriving_csv_format_row(
        out, sizeof(out), &ble_record, first_seen, strlen(first_seen));
    CHECK(ble_n > 0, "CSV_ROW_BLE: format succeeds");
    out[ble_n] = '\0';
    CHECK(strstr(out, "11:12:13:14:15:16") != NULL, "CSV_ROW_BLE: MAC formatted as hex pairs");
    CHECK(strstr(out, ",,,") != NULL, "CSV_ROW_BLE: blank SSID/AuthMode fields for a no-name device");
    CHECK(
        strstr(out, "12:00:00,,,-28") != NULL,
        "CSV_ROW_BLE: blank Channel/Frequency fields (BLE hops channels, no single value applies)");
    CHECK(strstr(out, "-28") != NULL, "CSV_ROW_BLE: RSSI decoded from offset (100-128=-28)");
    CHECK(strstr(out, ",BLE") != NULL, "CSV_ROW_BLE: Type == BLE");

    /* Too-small out_cap fails cleanly. */
    char tiny[4];
    CHECK(
        feb_wardriving_csv_format_row(tiny, sizeof(tiny), &wifi_record, first_seen, strlen(first_seen)) ==
            0,
        "CSV_ROW: too-small out_cap returns 0, not a truncated row");
}

static feb_wardriving_record_t make_dedup_wifi_record(
    const uint8_t bssid[6], int32_t rssi_dbm, uint64_t lat_e7_offset, uint64_t lon_e7_offset) {
    feb_wardriving_record_t record;
    memset(&record, 0, sizeof(record));
    record.lat_e7_offset = lat_e7_offset;
    record.lon_e7_offset = lon_e7_offset;
    record.source = "wifi";
    record.source_len = strlen(record.source);
    record.payload_kind = FEB_WARDRIVING_PAYLOAD_WIFI;
    memcpy(record.payload.wifi.bssid, bssid, 6);
    record.payload.wifi.rssi_offset = (uint64_t)(rssi_dbm + 128);
    return record;
}

static feb_wardriving_record_t make_dedup_ble_record(
    const uint8_t address[6], int32_t rssi_dbm, uint64_t lat_e7_offset, uint64_t lon_e7_offset) {
    feb_wardriving_record_t record;
    memset(&record, 0, sizeof(record));
    record.lat_e7_offset = lat_e7_offset;
    record.lon_e7_offset = lon_e7_offset;
    record.source = "ble";
    record.source_len = strlen(record.source);
    record.payload_kind = FEB_WARDRIVING_PAYLOAD_BLE;
    memcpy(record.payload.ble.address, address, 6);
    record.payload.ble.rssi_offset = (uint64_t)(rssi_dbm + 128);
    return record;
}

/* Base coordinate used throughout: lat_e7_offset/lon_e7_offset == 900000000/1800000000 decode
   to (0, 0) per feb_wardriving_csv_format_row()'s own arithmetic (see that test above). */
#define DEDUP_BASE_LAT 900000000u
#define DEDUP_BASE_LON 1800000000u

static void test_wardriving_dedup(void) {
    static const uint8_t addr_a[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t addr_b[6] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    feb_wardriving_dedup_table_t table;
    feb_wardriving_dedup_reset(&table);

    feb_wardriving_record_t r1 = make_dedup_wifi_record(addr_a, -60, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        feb_wardriving_dedup_should_write(&table, &r1),
        "DEDUP: a never-seen-before BSSID is always written");

    feb_wardriving_record_t r2 = make_dedup_wifi_record(addr_a, -60, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        !feb_wardriving_dedup_should_write(&table, &r2),
        "DEDUP: same BSSID, same RSSI, same position -> skipped as a redundant repeat");

    feb_wardriving_record_t r3 = make_dedup_wifi_record(addr_a, -55, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        !feb_wardriving_dedup_should_write(&table, &r3),
        "DEDUP: RSSI improved by less than the threshold (5dB < 6dB) -> still skipped");

    feb_wardriving_record_t r4 = make_dedup_wifi_record(addr_a, -54, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        feb_wardriving_dedup_should_write(&table, &r4),
        "DEDUP: RSSI improved by exactly the threshold (6dB) -> written again");

    feb_wardriving_record_t r5 = make_dedup_wifi_record(addr_a, -54, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        !feb_wardriving_dedup_should_write(&table, &r5),
        "DEDUP: repeat right after a refresh (no further RSSI/position change) -> skipped again");

    /* ~0.00027 degrees of latitude is ~30m -- comfortably past the 30m move threshold. Longitude
       held fixed so this exercises the latitude leg of the distance approximation. */
    feb_wardriving_record_t r6 =
        make_dedup_wifi_record(addr_a, -54, DEDUP_BASE_LAT + 2700u, DEDUP_BASE_LON);
    CHECK(
        feb_wardriving_dedup_should_write(&table, &r6),
        "DEDUP: moved >= 30m with no RSSI change -> written again");

    feb_wardriving_record_t r7 =
        make_dedup_wifi_record(addr_a, -54, DEDUP_BASE_LAT + 2700u, DEDUP_BASE_LON);
    CHECK(
        !feb_wardriving_dedup_should_write(&table, &r7),
        "DEDUP: repeat at the same (already-refreshed) position -> skipped");

    /* A tiny nudge (~1m) must NOT cross the 30m threshold. */
    feb_wardriving_record_t r8 =
        make_dedup_wifi_record(addr_a, -54, DEDUP_BASE_LAT + 2700u + 90u, DEDUP_BASE_LON);
    CHECK(
        !feb_wardriving_dedup_should_write(&table, &r8),
        "DEDUP: a ~1m nudge does not cross the 30m move threshold -> skipped");

    /* A second, distinct BSSID is tracked independently of the first. */
    feb_wardriving_record_t r9 = make_dedup_ble_record(addr_b, -70, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        feb_wardriving_dedup_should_write(&table, &r9),
        "DEDUP: a second, distinct address is written regardless of the first address's state");

    /* Same 6-byte value as addr_b, but as a WiFi BSSID instead of a BLE address -- payload_kind
       is part of the key, so this must be treated as a different entry, not a repeat of r9. */
    feb_wardriving_record_t r10 = make_dedup_wifi_record(addr_b, -70, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        feb_wardriving_dedup_should_write(&table, &r10),
        "DEDUP: same 6 bytes but a different payload_kind (wifi vs ble) is not treated as a repeat");
}

static void test_wardriving_dedup_eviction(void) {
    feb_wardriving_dedup_table_t table;
    feb_wardriving_dedup_reset(&table);

    /* Fill the table with FEB_WARDRIVING_DEDUP_CAPACITY distinct addresses. */
    for(uint32_t i = 0; i < FEB_WARDRIVING_DEDUP_CAPACITY; i++) {
        uint8_t addr[6] = {
            0,
            0,
            (uint8_t)(i >> 24),
            (uint8_t)(i >> 16),
            (uint8_t)(i >> 8),
            (uint8_t)i,
        };
        feb_wardriving_record_t record =
            make_dedup_wifi_record(addr, -60, DEDUP_BASE_LAT, DEDUP_BASE_LON);
        CHECK(
            feb_wardriving_dedup_should_write(&table, &record),
            "DEDUP_EVICT: filling the table, every distinct address is written once");
    }

    /* One more, distinct, address: the table is full, so this evicts the oldest (first-inserted)
       entry via the ring cursor rather than growing. */
    uint8_t overflow_addr[6] = {0, 0, 0, 0, 0xff, 0xff};
    feb_wardriving_record_t overflow_record =
        make_dedup_wifi_record(overflow_addr, -60, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        feb_wardriving_dedup_should_write(&table, &overflow_record),
        "DEDUP_EVICT: a new address past capacity is still written (evicts the oldest slot)");

    /* The very first address inserted above should have been evicted -- it's now treated as new
       again rather than remembered. */
    uint8_t first_addr[6] = {0, 0, 0, 0, 0, 0};
    feb_wardriving_record_t first_record_again =
        make_dedup_wifi_record(first_addr, -60, DEDUP_BASE_LAT, DEDUP_BASE_LON);
    CHECK(
        feb_wardriving_dedup_should_write(&table, &first_record_again),
        "DEDUP_EVICT: the oldest entry was evicted to make room, so it's no longer remembered");
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
    {
        static const uint8_t nonzero_flags_frag0[4] = {0x01, 0x00, 0x00, 0x01};
        const uint8_t* single[1] = {nonzero_flags_frag0};
        size_t single_len[1] = {sizeof(nonzero_flags_frag0)};
        test_malformed_fragment_sequence(
            single, single_len, 1, FEB_FRAME_INVALID_HEADER, "NONZERO_FLAGS_FRAG0");
    }

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

    test_ble_scan_device_codec();
    test_ble_scan_result_payload_codec();
    test_wardriving_command_payload_codec();
    test_wardriving_record_and_status_result_codec();
    test_wardriving_record_and_status_result_vectors();

    test_gps_status_result_payload_codec();
    test_gps_payload_vectors();

    test_wardriving_csv_format_header();
    test_wardriving_csv_format_row();
    test_wardriving_dedup();
    test_wardriving_dedup_eviction();

    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}

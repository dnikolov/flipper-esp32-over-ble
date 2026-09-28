/* Host-native test driver for esp32/main/framing.c + the split cbor_*.c codec (docs/
   OPTIMIZATION.md item 1: cbor_primitives.c/cbor_records.c/cbor_wifi_scan.c/
   cbor_ble_scan.c/cbor_wardriving.c), compiled directly (not copies) against the shared
   vectors in tests/vectors/vectors.h. See docs/PLAN.md step 3 "Validation gate is
   host-native, not on-device". */
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

/* docs/archive/CODE_REVIEW_FIX_PLAN.md W1-W3: feb_cbor_skip_value() itself has zero direct test
   coverage prior to this pass; these call it directly rather than only through envelope
   decoding of well-formed payloads. */
static void test_skip_value_direct(void)
{
    feb_cbor_status_t st;
    const uint8_t *span;
    size_t span_len;
    size_t n;

    n = feb_cbor_skip_value(FEB_VEC_SKIP_NEGINT, FEB_VEC_SKIP_NEGINT_LEN, 2, &span, &span_len, &st);
    check(n == 0 && st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "skip_value: negative integer rejected");

    n = feb_cbor_skip_value(FEB_VEC_SKIP_TRUE, FEB_VEC_SKIP_TRUE_LEN, 2, &span, &span_len, &st);
    check(n == 0 && st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "skip_value: true rejected");

    n = feb_cbor_skip_value(FEB_VEC_SKIP_NULL, FEB_VEC_SKIP_NULL_LEN, 2, &span, &span_len, &st);
    check(n == 0 && st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "skip_value: null rejected");

    n = feb_cbor_skip_value(FEB_VEC_SKIP_TRUNCATED_MAP, FEB_VEC_SKIP_TRUNCATED_MAP_LEN, 2, &span, &span_len, &st);
    check(n == 0 && st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "skip_value: truncated map rejected (W1)");

    n = feb_cbor_skip_value(FEB_VEC_SKIP_NEST_AT_LIMIT, FEB_VEC_SKIP_NEST_AT_LIMIT_LEN, 2, &span, &span_len, &st);
    check(n > 0 && st == FEB_CBOR_OK, "skip_value: nesting at FEB_CBOR_MAX_NESTING accepted");

    n = feb_cbor_skip_value(FEB_VEC_SKIP_NEST_TOO_DEEP, FEB_VEC_SKIP_NEST_TOO_DEEP_LEN, 2, &span, &span_len, &st);
    check(n == 0 && st == FEB_CBOR_ERR_TOO_DEEP,
          "skip_value: nesting one level past FEB_CBOR_MAX_NESTING rejected");
}

/* Same shapes as above, wrapped as a real payload span and decoded through the full
   envelope decoder (docs/archive/CODE_REVIEW_FIX_PLAN.md W1-W3, W6). */
static void test_payload_type_depth_and_trailing(void)
{
    feb_unencrypted_record_t rec;
    feb_cbor_status_t st;

    st = feb_cbor_decode_unencrypted(FEB_VEC_PAYLOAD_NEGINT_RECORD, FEB_VEC_PAYLOAD_NEGINT_RECORD_LEN, &rec);
    check(st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "payload with negative-int value rejected");

    st = feb_cbor_decode_unencrypted(FEB_VEC_PAYLOAD_TRUE_RECORD, FEB_VEC_PAYLOAD_TRUE_RECORD_LEN, &rec);
    check(st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "payload with true value rejected");

    st = feb_cbor_decode_unencrypted(FEB_VEC_PAYLOAD_NULL_RECORD, FEB_VEC_PAYLOAD_NULL_RECORD_LEN, &rec);
    check(st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "payload with null value rejected");

    st = feb_cbor_decode_unencrypted(FEB_VEC_TRUNCATED_PAYLOAD_MAP_RECORD,
                                      FEB_VEC_TRUNCATED_PAYLOAD_MAP_RECORD_LEN, &rec);
    check(st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "record with truncated payload map rejected (W1)");

    st = feb_cbor_decode_unencrypted(FEB_VEC_NEST_AT_LIMIT_RECORD, FEB_VEC_NEST_AT_LIMIT_RECORD_LEN, &rec);
    check(st == FEB_CBOR_OK, "payload nested exactly to FEB_CBOR_MAX_NESTING accepted");

    st = feb_cbor_decode_unencrypted(FEB_VEC_NEST_TOO_DEEP_RECORD, FEB_VEC_NEST_TOO_DEEP_RECORD_LEN, &rec);
    check(st == FEB_CBOR_ERR_TOO_DEEP, "payload nested one level too deep rejected (W3)");

    st = feb_cbor_decode_unencrypted(FEB_VEC_RECORD_TRAILING_BYTE, FEB_VEC_RECORD_TRAILING_BYTE_LEN, &rec);
    check(st == FEB_CBOR_ERR_UNEXPECTED_TYPE, "record with trailing byte rejected (W6)");
}

/* docs/PLAN.md "Wi-Fi scan capability" step: pure-codec wifi_scan vectors (no session
   crypto involved -- the end-to-end protected-record wraps of FEB_VEC_WIFI_SCAN_CMD_RECORD/
   STATUS_PARTIAL_RECORD/STATUS_COMPLETE_RECORD are tested in tests/esp32/test_session.c
   instead, since this binary's build.ps1 only links framing.c and the split cbor_*.c codec
   files (docs/OPTIMIZATION.md item 1) and has no session.c/mbedtls AES-GCM dependency --
   see the esp32-developer report for this step). */
static void test_wifi_scan_ap_roundtrip(const uint8_t *vec, size_t vec_len,
                                         const uint8_t *expected_ssid, size_t expected_ssid_len,
                                         uint64_t expected_rssi_offset, uint64_t expected_channel,
                                         const char *expected_phy, const char *expected_auth,
                                         const char *name)
{
    feb_wifi_scan_ap_t ap;
    feb_cbor_status_t status;
    size_t consumed;
    uint8_t encode_buf[128];
    size_t encoded_len;
    int ok;
    char check_name[128];

    consumed = feb_cbor_decode_wifi_scan_ap(vec, vec_len, &ap, &status);
    ok = (consumed == vec_len && status == FEB_CBOR_OK);
    ok = ok && bytes_eq(ap.ssid, ap.ssid_len, expected_ssid, expected_ssid_len);
    ok = ok && ap.rssi_offset == expected_rssi_offset;
    ok = ok && ap.channel == expected_channel;
    ok = ok && ap.phy_len == strlen(expected_phy) && memcmp(ap.phy, expected_phy, ap.phy_len) == 0;
    ok = ok && ap.auth_len == strlen(expected_auth) && memcmp(ap.auth, expected_auth, ap.auth_len) == 0;
    snprintf(check_name, sizeof(check_name), "%s: decode matches expected fields", name);
    check(ok, check_name);

    encoded_len = feb_cbor_encode_wifi_scan_ap(encode_buf, sizeof(encode_buf), &ap);
    snprintf(check_name, sizeof(check_name), "%s: encode round-trip byte-identical", name);
    check(bytes_eq(encode_buf, encoded_len, vec, vec_len), check_name);
}

static void test_wifi_scan_result_payload(void)
{
    feb_wifi_scan_result_payload_t result;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;

    status = feb_cbor_decode_wifi_scan_result_payload(FEB_VEC_WIFI_SCAN_RESULT_SINGLE,
                                                        FEB_VEC_WIFI_SCAN_RESULT_SINGLE_LEN, &result);
    check(status == FEB_CBOR_OK && result.ap_count == 1, "wifi_scan result (single): decodes 1 AP");
    encoded_len = feb_cbor_encode_wifi_scan_result_payload(encode_buf, sizeof(encode_buf), &result);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WIFI_SCAN_RESULT_SINGLE, FEB_VEC_WIFI_SCAN_RESULT_SINGLE_LEN),
          "wifi_scan result (single): encode round-trip byte-identical");

    status = feb_cbor_decode_wifi_scan_result_payload(FEB_VEC_WIFI_SCAN_RESULT_MULTI,
                                                        FEB_VEC_WIFI_SCAN_RESULT_MULTI_LEN, &result);
    check(status == FEB_CBOR_OK && result.ap_count == 3, "wifi_scan result (multi): decodes 3 APs");
    encoded_len = feb_cbor_encode_wifi_scan_result_payload(encode_buf, sizeof(encode_buf), &result);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WIFI_SCAN_RESULT_MULTI, FEB_VEC_WIFI_SCAN_RESULT_MULTI_LEN),
          "wifi_scan result (multi): encode round-trip byte-identical");

    status = feb_cbor_decode_wifi_scan_result_payload(FEB_VEC_WIFI_SCAN_RESULT_EMPTY,
                                                        FEB_VEC_WIFI_SCAN_RESULT_EMPTY_LEN, &result);
    check(status == FEB_CBOR_OK && result.ap_count == 0, "wifi_scan result (empty): decodes 0 APs");
    encoded_len = feb_cbor_encode_wifi_scan_result_payload(encode_buf, sizeof(encode_buf), &result);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WIFI_SCAN_RESULT_EMPTY, FEB_VEC_WIFI_SCAN_RESULT_EMPTY_LEN),
          "wifi_scan result (empty): encode round-trip byte-identical");
}

/* Streaming decode (docs/HARDENING_BACKLOG.md H04): one-AP-at-a-time counterpart to
   feb_cbor_decode_wifi_scan_result_payload() above. Confirms a well-formed batch matches the
   whole-array decode element-by-element, and a batch malformed partway through fails
   identically on both decoders with zero callback invocations -- no partial prefix of a batch
   that turns out invalid. */
#define WIFI_SCAN_STREAM_CAPTURE_MAX 8u

typedef struct {
    feb_wifi_scan_ap_t aps[WIFI_SCAN_STREAM_CAPTURE_MAX];
    size_t count;
} wifi_scan_stream_capture_t;

static void wifi_scan_stream_capture_cb(const feb_wifi_scan_ap_t *ap, void *ctx)
{
    wifi_scan_stream_capture_t *cap = (wifi_scan_stream_capture_t *)ctx;

    if (cap->count < WIFI_SCAN_STREAM_CAPTURE_MAX) {
        cap->aps[cap->count++] = *ap;
    }
}

static void test_wifi_scan_result_payload_stream(void)
{
    feb_wifi_scan_result_payload_t whole;
    feb_cbor_status_t whole_status;
    wifi_scan_stream_capture_t cap;
    feb_cbor_status_t stream_status;
    size_t ap_count_out;
    size_t truncated_len;
    feb_wifi_scan_result_payload_t whole_truncated;
    feb_cbor_status_t whole_truncated_status;
    wifi_scan_stream_capture_t cap_truncated;
    size_t ap_count_out_truncated;

    whole_status = feb_cbor_decode_wifi_scan_result_payload(FEB_VEC_WIFI_SCAN_RESULT_MULTI,
                                                              FEB_VEC_WIFI_SCAN_RESULT_MULTI_LEN, &whole);
    check(whole_status == FEB_CBOR_OK && whole.ap_count == 3,
          "wifi_scan result stream: whole-array baseline decodes 3 APs");

    memset(&cap, 0, sizeof(cap));
    ap_count_out = 0;
    stream_status = feb_cbor_decode_wifi_scan_result_payload_stream(FEB_VEC_WIFI_SCAN_RESULT_MULTI,
                                                                      FEB_VEC_WIFI_SCAN_RESULT_MULTI_LEN,
                                                                      wifi_scan_stream_capture_cb, &cap,
                                                                      &ap_count_out);
    check(stream_status == FEB_CBOR_OK, "wifi_scan result stream: streaming decode status OK");
    check(ap_count_out == 3 && cap.count == 3,
          "wifi_scan result stream: streaming decode reports/captures 3 APs");
    check(cap.aps[0].channel == whole.aps[0].channel && cap.aps[1].channel == whole.aps[1].channel &&
          cap.aps[2].channel == whole.aps[2].channel,
          "wifi_scan result stream: captured APs match whole-array decode, in order (channel)");
    check(cap.aps[0].ssid_len == whole.aps[0].ssid_len &&
          memcmp(cap.aps[0].ssid, whole.aps[0].ssid, cap.aps[0].ssid_len) == 0,
          "wifi_scan result stream: captured AP0 ssid matches whole-array decode");

    /* Truncate the last byte, landing inside the 3rd AP's trailing `auth` field. */
    truncated_len = FEB_VEC_WIFI_SCAN_RESULT_MULTI_LEN - 1;
    whole_truncated_status = feb_cbor_decode_wifi_scan_result_payload(FEB_VEC_WIFI_SCAN_RESULT_MULTI,
                                                                       truncated_len, &whole_truncated);
    check(whole_truncated_status != FEB_CBOR_OK,
          "wifi_scan result stream: whole-array decode of a truncated 3-AP batch fails");

    memset(&cap_truncated, 0, sizeof(cap_truncated));
    ap_count_out_truncated = 123; /* poisoned; must come back 0 */
    stream_status = feb_cbor_decode_wifi_scan_result_payload_stream(FEB_VEC_WIFI_SCAN_RESULT_MULTI,
                                                                      truncated_len, wifi_scan_stream_capture_cb,
                                                                      &cap_truncated, &ap_count_out_truncated);
    check(stream_status == whole_truncated_status,
          "wifi_scan result stream: streaming decode of the same truncated batch fails with the same status");
    check(cap_truncated.count == 0 && ap_count_out_truncated == 0,
          "wifi_scan result stream: a malformed batch invokes the callback zero times (no partial prefix)");
}

static void test_wifi_scan_command_payload(void)
{
    feb_command_payload_t cmd;
    feb_cbor_status_t status;
    size_t arg_count;
    uint8_t encode_buf[128];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_command_payload(FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD,
                                              FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && cmd.capability_len == strlen("wifi_scan") && memcmp(cmd.capability, "wifi_scan", cmd.capability_len) == 0;
    ok = ok && cmd.request_id == 101;
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 0;
    check(ok, "wifi_scan command payload: decodes capability/request_id/empty arguments");

    encoded_len = feb_cbor_encode_command_payload(encode_buf, sizeof(encode_buf), &cmd);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD, FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD_LEN),
          "wifi_scan command payload: encode round-trip byte-identical");

    status = feb_cbor_decode_command_payload(FEB_VEC_WIFI_SCAN_COMMAND_BAD_ARGUMENTS_PAYLOAD,
                                              FEB_VEC_WIFI_SCAN_COMMAND_BAD_ARGUMENTS_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 1;
    check(ok, "wifi_scan command payload (non-empty arguments): decodes structurally OK; "
              "rejection is a main.c dispatch-layer concern (invalid_command), not a codec error");
}

static void test_wifi_scan_status_payload(void)
{
    feb_status_payload_t st;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_status_payload(FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD,
                                             FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 101 && st.has_result;
    ok = ok && st.state_len == strlen("partial") && memcmp(st.state, "partial", st.state_len) == 0;
    check(ok, "wifi_scan status (partial): decodes request_id/state/result");
    encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD,
                   FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD_LEN),
          "wifi_scan status (partial): encode round-trip byte-identical");

    status = feb_cbor_decode_status_payload(FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD,
                                             FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 101 && st.has_result;
    ok = ok && st.state_len == strlen("complete") && memcmp(st.state, "complete", st.state_len) == 0;
    check(ok, "wifi_scan status (complete, with results): decodes request_id/state/result");
    encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD,
                   FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD_LEN),
          "wifi_scan status (complete, with results): encode round-trip byte-identical");

    status = feb_cbor_decode_status_payload(FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_EMPTY_PAYLOAD,
                                             FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_EMPTY_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 101 && st.has_result;
    ok = ok && st.state_len == strlen("complete") && memcmp(st.state, "complete", st.state_len) == 0;
    check(ok, "wifi_scan status (complete, empty aps): decodes request_id/state/result");
    encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_EMPTY_PAYLOAD,
                   FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_EMPTY_PAYLOAD_LEN),
          "wifi_scan status (complete, empty aps): encode round-trip byte-identical");

    status = feb_cbor_decode_status_payload(FEB_VEC_WIFI_SCAN_STATUS_BAD_STATE_PAYLOAD,
                                             FEB_VEC_WIFI_SCAN_STATUS_BAD_STATE_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.state_len == strlen("started") && memcmp(st.state, "started", st.state_len) == 0;
    check(ok, "wifi_scan status (bad state \"started\"): decodes structurally OK; "
              "the partial/complete enum check is a main.c/peer semantic concern, not a codec error");
}

/* docs/PLAN.md wardriving/ble_scan wire-format step: ble_scan codec vectors, mirroring
   wifi_scan's own test structure exactly. */
static void test_ble_scan_device_roundtrip(const uint8_t *vec, size_t vec_len,
                                            const uint8_t *expected_addr,
                                            int expected_has_name, const char *expected_name,
                                            uint64_t expected_rssi_offset,
                                            const char *expected_addr_type,
                                            const char *name)
{
    feb_ble_scan_device_t device;
    feb_cbor_status_t status;
    size_t consumed;
    uint8_t encode_buf[128];
    size_t encoded_len;
    int ok;
    char check_name[160];

    consumed = feb_cbor_decode_ble_scan_device(vec, vec_len, &device, &status);
    ok = (consumed == vec_len && status == FEB_CBOR_OK);
    ok = ok && memcmp(device.address, expected_addr, FEB_BLE_SCAN_ADDRESS_LEN) == 0;
    ok = ok && device.has_name == expected_has_name;
    if (expected_has_name) {
        ok = ok && device.name_len == strlen(expected_name) &&
             memcmp(device.name, expected_name, device.name_len) == 0;
    }
    ok = ok && device.rssi_offset == expected_rssi_offset;
    ok = ok && device.addr_type_len == strlen(expected_addr_type) &&
         memcmp(device.addr_type, expected_addr_type, device.addr_type_len) == 0;
    snprintf(check_name, sizeof(check_name), "%s: decode matches expected fields", name);
    check(ok, check_name);

    encoded_len = feb_cbor_encode_ble_scan_device(encode_buf, sizeof(encode_buf), &device);
    snprintf(check_name, sizeof(check_name), "%s: encode round-trip byte-identical", name);
    check(bytes_eq(encode_buf, encoded_len, vec, vec_len), check_name);
}

static void test_ble_scan_result_payload(void)
{
    feb_ble_scan_result_payload_t result;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;

    status = feb_cbor_decode_ble_scan_result_payload(FEB_VEC_BLE_SCAN_RESULT_SINGLE,
                                                       FEB_VEC_BLE_SCAN_RESULT_SINGLE_LEN, &result);
    check(status == FEB_CBOR_OK && result.device_count == 1, "ble_scan result (single): decodes 1 device");
    encoded_len = feb_cbor_encode_ble_scan_result_payload(encode_buf, sizeof(encode_buf), &result);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_BLE_SCAN_RESULT_SINGLE, FEB_VEC_BLE_SCAN_RESULT_SINGLE_LEN),
          "ble_scan result (single): encode round-trip byte-identical");

    status = feb_cbor_decode_ble_scan_result_payload(FEB_VEC_BLE_SCAN_RESULT_MULTI,
                                                       FEB_VEC_BLE_SCAN_RESULT_MULTI_LEN, &result);
    check(status == FEB_CBOR_OK && result.device_count == 2, "ble_scan result (multi): decodes 2 devices");
    check(result.devices[1].has_name == 0, "ble_scan result (multi): device 2 has_name is 0 (no name advertised)");
    encoded_len = feb_cbor_encode_ble_scan_result_payload(encode_buf, sizeof(encode_buf), &result);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_BLE_SCAN_RESULT_MULTI, FEB_VEC_BLE_SCAN_RESULT_MULTI_LEN),
          "ble_scan result (multi): encode round-trip byte-identical");

    status = feb_cbor_decode_ble_scan_result_payload(FEB_VEC_BLE_SCAN_RESULT_EMPTY,
                                                       FEB_VEC_BLE_SCAN_RESULT_EMPTY_LEN, &result);
    check(status == FEB_CBOR_OK && result.device_count == 0, "ble_scan result (empty): decodes 0 devices");
    encoded_len = feb_cbor_encode_ble_scan_result_payload(encode_buf, sizeof(encode_buf), &result);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_BLE_SCAN_RESULT_EMPTY, FEB_VEC_BLE_SCAN_RESULT_EMPTY_LEN),
          "ble_scan result (empty): encode round-trip byte-identical");
}

/* Streaming decode (docs/HARDENING_BACKLOG.md H04) -- same contract as
   test_wifi_scan_result_payload_stream() above. */
#define BLE_SCAN_STREAM_CAPTURE_MAX 8u

typedef struct {
    feb_ble_scan_device_t devices[BLE_SCAN_STREAM_CAPTURE_MAX];
    size_t count;
} ble_scan_stream_capture_t;

static void ble_scan_stream_capture_cb(const feb_ble_scan_device_t *device, void *ctx)
{
    ble_scan_stream_capture_t *cap = (ble_scan_stream_capture_t *)ctx;

    if (cap->count < BLE_SCAN_STREAM_CAPTURE_MAX) {
        cap->devices[cap->count++] = *device;
    }
}

static void test_ble_scan_result_payload_stream(void)
{
    ble_scan_stream_capture_t cap;
    feb_cbor_status_t stream_status;
    size_t device_count_out;
    size_t truncated_len;
    feb_ble_scan_result_payload_t whole_truncated;
    feb_cbor_status_t whole_truncated_status;
    ble_scan_stream_capture_t cap_truncated;
    size_t device_count_out_truncated;

    memset(&cap, 0, sizeof(cap));
    device_count_out = 0;
    stream_status = feb_cbor_decode_ble_scan_result_payload_stream(FEB_VEC_BLE_SCAN_RESULT_MULTI,
                                                                     FEB_VEC_BLE_SCAN_RESULT_MULTI_LEN,
                                                                     ble_scan_stream_capture_cb, &cap,
                                                                     &device_count_out);
    check(stream_status == FEB_CBOR_OK, "ble_scan result stream: streaming decode status OK");
    check(device_count_out == 2 && cap.count == 2,
          "ble_scan result stream: streaming decode reports/captures 2 devices");
    check(cap.devices[1].has_name == 0,
          "ble_scan result stream: captured device 2 has_name is 0 (no name advertised)");

    /* Truncate the last byte of the 2-device vector. */
    truncated_len = FEB_VEC_BLE_SCAN_RESULT_MULTI_LEN - 1;
    whole_truncated_status = feb_cbor_decode_ble_scan_result_payload(FEB_VEC_BLE_SCAN_RESULT_MULTI,
                                                                      truncated_len, &whole_truncated);
    check(whole_truncated_status != FEB_CBOR_OK,
          "ble_scan result stream: whole-array decode of a truncated batch fails");

    memset(&cap_truncated, 0, sizeof(cap_truncated));
    device_count_out_truncated = 123;
    stream_status = feb_cbor_decode_ble_scan_result_payload_stream(FEB_VEC_BLE_SCAN_RESULT_MULTI,
                                                                     truncated_len, ble_scan_stream_capture_cb,
                                                                     &cap_truncated, &device_count_out_truncated);
    check(stream_status == whole_truncated_status,
          "ble_scan result stream: streaming decode of the same truncated batch fails with the same status");
    check(cap_truncated.count == 0 && device_count_out_truncated == 0,
          "ble_scan result stream: a malformed batch invokes the callback zero times (no partial prefix)");
}

static void test_ble_scan_command_payload(void)
{
    feb_command_payload_t cmd;
    feb_cbor_status_t status;
    size_t arg_count;
    uint8_t encode_buf[128];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_command_payload(FEB_VEC_BLE_SCAN_COMMAND_PAYLOAD,
                                              FEB_VEC_BLE_SCAN_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && cmd.capability_len == strlen("ble_scan") && memcmp(cmd.capability, "ble_scan", cmd.capability_len) == 0;
    ok = ok && cmd.request_id == 401;
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 0;
    check(ok, "ble_scan command payload: decodes capability/request_id/empty arguments");

    encoded_len = feb_cbor_encode_command_payload(encode_buf, sizeof(encode_buf), &cmd);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_BLE_SCAN_COMMAND_PAYLOAD, FEB_VEC_BLE_SCAN_COMMAND_PAYLOAD_LEN),
          "ble_scan command payload: encode round-trip byte-identical");

    status = feb_cbor_decode_command_payload(FEB_VEC_BLE_SCAN_COMMAND_BAD_ARGUMENTS_PAYLOAD,
                                              FEB_VEC_BLE_SCAN_COMMAND_BAD_ARGUMENTS_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 1;
    check(ok, "ble_scan command payload (non-empty arguments): decodes structurally OK; "
              "rejection is a main.c dispatch-layer concern (invalid_command), not a codec error");
}

static void test_ble_scan_status_payload(void)
{
    feb_status_payload_t st;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_status_payload(FEB_VEC_BLE_SCAN_STATUS_PARTIAL_PAYLOAD,
                                             FEB_VEC_BLE_SCAN_STATUS_PARTIAL_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 401 && st.has_result;
    ok = ok && st.state_len == strlen("partial") && memcmp(st.state, "partial", st.state_len) == 0;
    check(ok, "ble_scan status (partial): decodes request_id/state/result");
    encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_BLE_SCAN_STATUS_PARTIAL_PAYLOAD,
                   FEB_VEC_BLE_SCAN_STATUS_PARTIAL_PAYLOAD_LEN),
          "ble_scan status (partial): encode round-trip byte-identical");

    status = feb_cbor_decode_status_payload(FEB_VEC_BLE_SCAN_STATUS_COMPLETE_PAYLOAD,
                                             FEB_VEC_BLE_SCAN_STATUS_COMPLETE_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 401 && st.has_result;
    ok = ok && st.state_len == strlen("complete") && memcmp(st.state, "complete", st.state_len) == 0;
    check(ok, "ble_scan status (complete): decodes request_id/state/result");
    encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_BLE_SCAN_STATUS_COMPLETE_PAYLOAD,
                   FEB_VEC_BLE_SCAN_STATUS_COMPLETE_PAYLOAD_LEN),
          "ble_scan status (complete): encode round-trip byte-identical");
}

/* wardriving codec vectors -- docs/PLAN.md's highest-risk item is the nesting depth of
   status.result's <wardriving-record>.payload shape; test_wardriving_status_result_payload()
   below decodes it through feb_cbor_decode_status_payload()'s real feb_cbor_skip_value()-based
   `result` span capture (fresh depth-0 budget), not just this module's own direct
   record-level decoder, to prove the whole capability-agnostic path accepts it. */
static void test_wardriving_command_payload(void)
{
    feb_command_payload_t cmd;
    feb_wardriving_command_payload_t wc;
    feb_cbor_status_t status;
    uint8_t encode_buf[128];
    size_t encoded_len;
    int ok;

    /* start, both sources, explicit intervals */
    status = feb_cbor_decode_command_payload(FEB_VEC_WARDRIVING_START_COMMAND_PAYLOAD,
                                              FEB_VEC_WARDRIVING_START_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK) && cmd.capability_len == strlen("wardriving") &&
         memcmp(cmd.capability, "wardriving", cmd.capability_len) == 0 && cmd.request_id == 501;
    if (ok) {
        status = feb_cbor_decode_wardriving_command_payload(cmd.arguments_span, cmd.arguments_span_len, &wc);
        ok = (status == FEB_CBOR_OK);
        ok = ok && wc.action_len == strlen("start") && memcmp(wc.action, "start", wc.action_len) == 0;
        ok = ok && wc.has_sources && wc.source_count == 2;
        ok = ok && wc.source_lens[0] == strlen("wifi") && memcmp(wc.sources[0], "wifi", wc.source_lens[0]) == 0;
        ok = ok && wc.source_lens[1] == strlen("ble") && memcmp(wc.sources[1], "ble", wc.source_lens[1]) == 0;
        ok = ok && wc.has_wifi_interval_ms && wc.wifi_interval_ms == 30000;
        ok = ok && wc.has_ble_params && wc.ble_window_ms == 30 && wc.ble_interval_ms == 30;
    }
    check(ok, "wardriving command (start, both sources): decodes action/sources/intervals");
    if (ok) {
        encoded_len = feb_cbor_encode_wardriving_command_payload(encode_buf, sizeof(encode_buf), &wc);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WARDRIVING_START_ARGS, FEB_VEC_WARDRIVING_START_ARGS_LEN),
              "wardriving command (start, both sources): encode round-trip byte-identical");
    } else {
        check(0, "wardriving command (start, both sources): encode round-trip byte-identical");
    }

    /* stop: action only */
    status = feb_cbor_decode_command_payload(FEB_VEC_WARDRIVING_STOP_COMMAND_PAYLOAD,
                                              FEB_VEC_WARDRIVING_STOP_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK) && cmd.request_id == 502;
    if (ok) {
        status = feb_cbor_decode_wardriving_command_payload(cmd.arguments_span, cmd.arguments_span_len, &wc);
        ok = (status == FEB_CBOR_OK);
        ok = ok && wc.action_len == strlen("stop") && memcmp(wc.action, "stop", wc.action_len) == 0;
        ok = ok && !wc.has_sources && !wc.has_wifi_interval_ms && !wc.has_ble_params;
    }
    check(ok, "wardriving command (stop): decodes action only, no other fields present");
    if (ok) {
        encoded_len = feb_cbor_encode_wardriving_command_payload(encode_buf, sizeof(encode_buf), &wc);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WARDRIVING_STOP_ARGS, FEB_VEC_WARDRIVING_STOP_ARGS_LEN),
              "wardriving command (stop): encode round-trip byte-identical");
    } else {
        check(0, "wardriving command (stop): encode round-trip byte-identical");
    }

    /* start missing wifi_interval_ms despite "wifi" in sources: decodes structurally OK;
       the action/sources-dependent requiredness check is a caller (main.c) concern, not a
       codec error -- see cbor_wardriving.h's wardriving comment, same split as wifi_scan's
       non-empty-arguments case above. */
    status = feb_cbor_decode_command_payload(FEB_VEC_WARDRIVING_START_MISSING_INTERVAL_COMMAND_PAYLOAD,
                                              FEB_VEC_WARDRIVING_START_MISSING_INTERVAL_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    if (ok) {
        status = feb_cbor_decode_wardriving_command_payload(cmd.arguments_span, cmd.arguments_span_len, &wc);
        ok = (status == FEB_CBOR_OK) && wc.has_sources && wc.source_count == 1 && !wc.has_wifi_interval_ms;
    }
    check(ok, "wardriving command (start missing wifi_interval_ms): decodes structurally OK; "
              "rejection is a main.c dispatch-layer concern (invalid_command), not a codec error");

    /* hard-malformed arguments: unrecognized field name must be rejected by the codec
       itself, unlike the semantic case above. */
    status = feb_cbor_decode_command_payload(FEB_VEC_WARDRIVING_BAD_FIELD_COMMAND_PAYLOAD,
                                              FEB_VEC_WARDRIVING_BAD_FIELD_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    if (ok) {
        status = feb_cbor_decode_wardriving_command_payload(cmd.arguments_span, cmd.arguments_span_len, &wc);
    }
    check(status == FEB_CBOR_ERR_UNEXPECTED_TYPE,
          "wardriving command (unrecognized field name): hard-rejected by the codec itself");

    /* start, wifi only, with wifi_swelling/country (docs/WARDRIVING_REDESIGN.md, added
       2026-09-21) -- appended after ble_interval_ms per docs/PROTOCOL.md's field order. */
    status = feb_cbor_decode_command_payload(FEB_VEC_WARDRIVING_START_SWELLING_COUNTRY_COMMAND_PAYLOAD,
                                              FEB_VEC_WARDRIVING_START_SWELLING_COUNTRY_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK) && cmd.request_id == 503;
    if (ok) {
        status = feb_cbor_decode_wardriving_command_payload(cmd.arguments_span, cmd.arguments_span_len, &wc);
        ok = (status == FEB_CBOR_OK);
        ok = ok && wc.has_sources && wc.source_count == 1;
        ok = ok && wc.source_lens[0] == strlen("wifi") && memcmp(wc.sources[0], "wifi", wc.source_lens[0]) == 0;
        ok = ok && wc.has_wifi_interval_ms && wc.wifi_interval_ms == 5000;
        ok = ok && !wc.has_ble_params;
        ok = ok && wc.has_wifi_swelling && wc.wifi_swelling_len == strlen("aggressive") &&
             memcmp(wc.wifi_swelling, "aggressive", wc.wifi_swelling_len) == 0;
        ok = ok && wc.has_country && wc.country_len == strlen("BG") &&
             memcmp(wc.country, "BG", wc.country_len) == 0;
    }
    check(ok, "wardriving command (start, wifi only): decodes wifi_swelling/country");
    if (ok) {
        encoded_len = feb_cbor_encode_wardriving_command_payload(encode_buf, sizeof(encode_buf), &wc);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WARDRIVING_START_SWELLING_COUNTRY_ARGS,
                       FEB_VEC_WARDRIVING_START_SWELLING_COUNTRY_ARGS_LEN),
              "wardriving command (start, wifi only, wifi_swelling/country): encode round-trip byte-identical");
    } else {
        check(0, "wardriving command (start, wifi only, wifi_swelling/country): encode round-trip byte-identical");
    }
}

static void test_wardriving_record_roundtrip(void)
{
    feb_wardriving_record_t record;
    feb_cbor_status_t status;
    size_t consumed;
    uint8_t encode_buf[256];
    size_t encoded_len;
    int ok;

    consumed = feb_cbor_decode_wardriving_record(FEB_VEC_WARDRIVING_RECORD_WIFI,
                                                  FEB_VEC_WARDRIVING_RECORD_WIFI_LEN, &record, &status);
    ok = (consumed == FEB_VEC_WARDRIVING_RECORD_WIFI_LEN && status == FEB_CBOR_OK);
    ok = ok && record.timestamp_ms == 1000 && record.utc_timestamp_s == 1757667010 &&
         record.payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI;
    ok = ok && record.payload.wifi.ssid_len == strlen("TestNetwork") &&
         memcmp(record.payload.wifi.ssid, "TestNetwork", record.payload.wifi.ssid_len) == 0;
    ok = ok && record.payload.wifi.channel == 6;
    check(ok, "wardriving record (wifi-sourced): decodes timestamp/utc_timestamp_s/coords/source/payload");
    encoded_len = feb_cbor_encode_wardriving_record(encode_buf, sizeof(encode_buf), &record);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WARDRIVING_RECORD_WIFI, FEB_VEC_WARDRIVING_RECORD_WIFI_LEN),
          "wardriving record (wifi-sourced): encode round-trip byte-identical");

    consumed = feb_cbor_decode_wardriving_record(FEB_VEC_WARDRIVING_RECORD_BLE,
                                                  FEB_VEC_WARDRIVING_RECORD_BLE_LEN, &record, &status);
    ok = (consumed == FEB_VEC_WARDRIVING_RECORD_BLE_LEN && status == FEB_CBOR_OK);
    ok = ok && record.payload_kind == FEB_WARDRIVING_PAYLOAD_BLE && record.payload.ble.has_name;
    ok = ok && record.payload.ble.name_len == strlen("MyPhone") &&
         memcmp(record.payload.ble.name, "MyPhone", record.payload.ble.name_len) == 0;
    check(ok, "wardriving record (ble-sourced, named): decodes timestamp/coords/source/payload");
    encoded_len = feb_cbor_encode_wardriving_record(encode_buf, sizeof(encode_buf), &record);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WARDRIVING_RECORD_BLE, FEB_VEC_WARDRIVING_RECORD_BLE_LEN),
          "wardriving record (ble-sourced, named): encode round-trip byte-identical");

    consumed = feb_cbor_decode_wardriving_record(FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME,
                                                  FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME_LEN, &record, &status);
    ok = (consumed == FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME_LEN && status == FEB_CBOR_OK);
    ok = ok && record.payload_kind == FEB_WARDRIVING_PAYLOAD_BLE && !record.payload.ble.has_name;
    check(ok, "wardriving record (ble-sourced, no name): has_name is 0 (optional-field omission)");
    encoded_len = feb_cbor_encode_wardriving_record(encode_buf, sizeof(encode_buf), &record);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME,
                   FEB_VEC_WARDRIVING_RECORD_BLE_NO_NAME_LEN),
          "wardriving record (ble-sourced, no name): encode round-trip byte-identical");
}

static void test_wardriving_status_result_payload(void)
{
    feb_status_payload_t st;
    feb_wardriving_status_result_payload_t result;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    /* "data" state: decode through the real generic status_payload decoder first --
       this is what exercises feb_cbor_skip_value()'s fresh depth-0 budget against the
       4-container-level <wardriving-record>.payload shape, not just this module's own
       direct record decoder. */
    status = feb_cbor_decode_status_payload(FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD,
                                             FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 0 && st.has_result;
    ok = ok && st.state_len == strlen("data") && memcmp(st.state, "data", st.state_len) == 0;
    check(ok, "wardriving status (data): request_id=0 (unsolicited sentinel), state, result "
              "captured through feb_cbor_skip_value()'s fresh depth-0 budget without FEB_CBOR_ERR_TOO_DEEP");

    if (ok) {
        status = feb_cbor_decode_wardriving_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.record_count == 2 && result.backlog_remaining == 3;
        ok = ok && result.records[0].payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI;
        ok = ok && result.records[1].payload_kind == FEB_WARDRIVING_PAYLOAD_BLE;
    }
    check(ok, "wardriving status (data): result decodes 1 wifi record + 1 ble record, "
              "backlog_remaining matches");

    if (ok) {
        encoded_len = feb_cbor_encode_wardriving_status_result_payload(encode_buf, sizeof(encode_buf), &result);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WARDRIVING_RESULT_MIXED, FEB_VEC_WARDRIVING_RESULT_MIXED_LEN),
              "wardriving status (data): result encode round-trip byte-identical");

        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD,
                       FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD_LEN),
              "wardriving status (data): full status payload encode round-trip byte-identical");
    } else {
        check(0, "wardriving status (data): result encode round-trip byte-identical");
        check(0, "wardriving status (data): full status payload encode round-trip byte-identical");
    }

    /* "started"/"stopped": no result field at all (state model distinct from
       wifi_scan/ble_scan's partial/complete pair -- see docs/PROTOCOL.md). */
    status = feb_cbor_decode_status_payload(FEB_VEC_WARDRIVING_STATUS_STARTED_PAYLOAD,
                                             FEB_VEC_WARDRIVING_STATUS_STARTED_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 501 && !st.has_result;
    ok = ok && st.state_len == strlen("started") && memcmp(st.state, "started", st.state_len) == 0;
    check(ok, "wardriving status (started): decodes request_id/state, no result field");

    status = feb_cbor_decode_status_payload(FEB_VEC_WARDRIVING_STATUS_STOPPED_PAYLOAD,
                                             FEB_VEC_WARDRIVING_STATUS_STOPPED_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 502 && !st.has_result;
    ok = ok && st.state_len == strlen("stopped") && memcmp(st.state, "stopped", st.state_len) == 0;
    check(ok, "wardriving status (stopped): decodes request_id/state, no result field");
}

/* Streaming decode (docs/HARDENING_BACKLOG.md H04) -- one-record-at-a-time counterpart to
   feb_cbor_decode_wardriving_status_result_payload() above, same contract as
   test_wifi_scan_result_payload_stream()'s own comment. The truncation case here lands inside
   the trailing `backlog_remaining` field, AFTER both records already decode cleanly on their
   own -- the specific shape this decoder's two-pass validate-then-apply design exists for (a
   single-pass streaming decoder would have already invoked the callback for both records
   before ever reaching the broken tail). */
#define WARDRIVING_STREAM_CAPTURE_MAX 8u

typedef struct {
    feb_wardriving_record_t records[WARDRIVING_STREAM_CAPTURE_MAX];
    size_t count;
} wardriving_stream_capture_t;

static void wardriving_stream_capture_cb(const feb_wardriving_record_t *record, void *ctx)
{
    wardriving_stream_capture_t *cap = (wardriving_stream_capture_t *)ctx;

    if (cap->count < WARDRIVING_STREAM_CAPTURE_MAX) {
        cap->records[cap->count++] = *record;
    }
}

static void test_wardriving_status_result_payload_stream(void)
{
    wardriving_stream_capture_t cap;
    uint64_t backlog_remaining_out;
    feb_cbor_status_t stream_status;
    size_t truncated_len;
    feb_wardriving_status_result_payload_t whole_truncated;
    feb_cbor_status_t whole_truncated_status;
    wardriving_stream_capture_t cap_truncated;
    uint64_t backlog_remaining_out_truncated;

    memset(&cap, 0, sizeof(cap));
    backlog_remaining_out = 0;
    stream_status = feb_cbor_decode_wardriving_status_result_payload_stream(
        FEB_VEC_WARDRIVING_RESULT_MIXED, FEB_VEC_WARDRIVING_RESULT_MIXED_LEN,
        wardriving_stream_capture_cb, &cap, &backlog_remaining_out);
    check(stream_status == FEB_CBOR_OK, "wardriving result stream: streaming decode status OK");
    check(cap.count == 2, "wardriving result stream: streaming decode captures 2 records");
    check(backlog_remaining_out == 3, "wardriving result stream: streaming decode reports backlog_remaining == 3");
    check(cap.records[0].payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI &&
          cap.records[1].payload_kind == FEB_WARDRIVING_PAYLOAD_BLE,
          "wardriving result stream: captured records match encode order (wifi then ble)");

    /* Truncate the last byte, landing inside backlog_remaining's own encoding -- both
       records still decode cleanly on their own. */
    truncated_len = FEB_VEC_WARDRIVING_RESULT_MIXED_LEN - 1;
    whole_truncated_status = feb_cbor_decode_wardriving_status_result_payload(
        FEB_VEC_WARDRIVING_RESULT_MIXED, truncated_len, &whole_truncated);
    check(whole_truncated_status != FEB_CBOR_OK,
          "wardriving result stream: whole-array decode of a truncated batch (bad trailing "
          "backlog_remaining) fails");

    memset(&cap_truncated, 0, sizeof(cap_truncated));
    backlog_remaining_out_truncated = 123;
    stream_status = feb_cbor_decode_wardriving_status_result_payload_stream(
        FEB_VEC_WARDRIVING_RESULT_MIXED, truncated_len, wardriving_stream_capture_cb,
        &cap_truncated, &backlog_remaining_out_truncated);
    check(stream_status == whole_truncated_status,
          "wardriving result stream: streaming decode of the same truncated batch fails with the same status");
    check(cap_truncated.count == 0 && backlog_remaining_out_truncated == 0,
          "wardriving result stream: a malformed trailing field invokes the callback zero times "
          "even though both records were individually well-formed (two-pass validate-then-apply)");
}

/* gps codec vectors (docs/PROTOCOL.md "`gps` command and status payloads", design frozen
   2026-09-12). Mirrors ble_scan's command/status vector strategy above -- payload-codec-only,
   no protected-record end-to-end wrap. */
static void test_gps_command_payload(void)
{
    feb_command_payload_t cmd;
    feb_cbor_status_t status;
    size_t arg_count;
    uint8_t encode_buf[128];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_command_payload(FEB_VEC_GPS_COMMAND_PAYLOAD,
                                              FEB_VEC_GPS_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && cmd.capability_len == strlen("gps") && memcmp(cmd.capability, "gps", cmd.capability_len) == 0;
    ok = ok && cmd.request_id == 601;
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 0;
    check(ok, "gps command payload: decodes capability/request_id/empty arguments");

    encoded_len = feb_cbor_encode_command_payload(encode_buf, sizeof(encode_buf), &cmd);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_GPS_COMMAND_PAYLOAD, FEB_VEC_GPS_COMMAND_PAYLOAD_LEN),
          "gps command payload: encode round-trip byte-identical");

    status = feb_cbor_decode_command_payload(FEB_VEC_GPS_COMMAND_BAD_ARGUMENTS_PAYLOAD,
                                              FEB_VEC_GPS_COMMAND_BAD_ARGUMENTS_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 1;
    check(ok, "gps command payload (non-empty arguments): decodes structurally OK; "
              "rejection is a main.c dispatch-layer concern (invalid_command), not a codec error");
}

static void test_gps_status_payload(void)
{
    feb_status_payload_t st;
    feb_gps_result_payload_t result;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_status_payload(FEB_VEC_GPS_STATUS_NO_SIGNAL_PAYLOAD,
                                             FEB_VEC_GPS_STATUS_NO_SIGNAL_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 601 && !st.has_result;
    ok = ok && st.state_len == strlen("no_signal") && memcmp(st.state, "no_signal", st.state_len) == 0;
    check(ok, "gps status (no_signal): decodes request_id/state, no result field");

    status = feb_cbor_decode_status_payload(FEB_VEC_GPS_STATUS_ACQUIRING_PAYLOAD,
                                             FEB_VEC_GPS_STATUS_ACQUIRING_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 601 && !st.has_result;
    ok = ok && st.state_len == strlen("acquiring") && memcmp(st.state, "acquiring", st.state_len) == 0;
    check(ok, "gps status (acquiring): decodes request_id/state, no result field");

    status = feb_cbor_decode_status_payload(FEB_VEC_GPS_STATUS_FIX_PAYLOAD,
                                             FEB_VEC_GPS_STATUS_FIX_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 601 && st.has_result;
    ok = ok && st.state_len == strlen("fix") && memcmp(st.state, "fix", st.state_len) == 0;
    check(ok, "gps status (fix): decodes request_id/state/result");

    if (ok) {
        status = feb_cbor_decode_gps_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.fix_quality == 1 && result.satellites == 9 &&
             result.hdop_e1 == 20 && result.utc_timestamp_s == 1757667010 &&
             result.altitude_dm_offset == 1000529 && result.speed_e1_kmh == 456;
    }
    check(ok, "gps status (fix): result decodes fix_quality/satellites/hdop_e1/utc_timestamp_s/"
              "altitude_dm_offset/speed_e1_kmh");

    if (ok) {
        encoded_len = feb_cbor_encode_gps_result_payload(encode_buf, sizeof(encode_buf), &result);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_GPS_RESULT_FIX, FEB_VEC_GPS_RESULT_FIX_LEN),
              "gps status (fix): result encode round-trip byte-identical");

        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_GPS_STATUS_FIX_PAYLOAD,
                       FEB_VEC_GPS_STATUS_FIX_PAYLOAD_LEN),
              "gps status (fix): full status payload encode round-trip byte-identical");
    } else {
        check(0, "gps status (fix): result encode round-trip byte-identical");
        check(0, "gps status (fix): full status payload encode round-trip byte-identical");
    }
}

/* meshcore_scan codec vectors (docs/PROTOCOL.md "`meshcore_scan` command and status
   payloads", Heltec-only, design plan "MeshCore Scan Capability -- Heltec Board (Phase 1)").
   Mirrors gps/ble_scan's command-vector strategy above -- payload-codec-only, no
   protected-record end-to-end wrap. Also confirms FEB_MESHCORE_MAX_NODES_PER_RESULT's
   worst-case-sizing claim (cbor_meshcore.h's sizing-note comment): the generator
   (tests/vectors/generate_vectors.py) already asserts this at vector-generation time, but a
   fresh decode+re-encode round-trip here through the real C decoder/encoder (not just
   Python's byte-length arithmetic) is the actual "host-native" confirmation the design plan
   asked for. */
static void test_meshcore_node_roundtrip(void)
{
    feb_meshcore_node_t node;
    feb_cbor_status_t status;
    uint8_t encode_buf[256];
    size_t encoded_len;
    int ok;

    encoded_len = feb_cbor_decode_meshcore_node(FEB_VEC_MESHCORE_NODE1, FEB_VEC_MESHCORE_NODE1_LEN,
                                                 &node, &status);
    ok = (encoded_len == FEB_VEC_MESHCORE_NODE1_LEN) && (status == FEB_CBOR_OK);
    ok = ok && node.node_id_len == 16 && memcmp(node.node_id, "aabbccddeeff0011", 16) == 0;
    ok = ok && node.has_name && node.name_len == strlen("Bob's Node") &&
         memcmp(node.name, "Bob's Node", node.name_len) == 0;
    ok = ok && node.role_len == strlen("repeater") && memcmp(node.role, "repeater", node.role_len) == 0;
    ok = ok && node.rssi_offset == 58 /* -70 + 128 */ && node.last_seen_ms == 12345;
    ok = ok && !node.has_location;
    check(ok, "meshcore node1: decodes node_id/name/role/rssi_offset/last_seen_ms, no location");

    encoded_len = feb_cbor_encode_meshcore_node(encode_buf, sizeof(encode_buf), &node);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHCORE_NODE1, FEB_VEC_MESHCORE_NODE1_LEN),
          "meshcore node1: encode round-trip byte-identical");

    encoded_len = feb_cbor_decode_meshcore_node(FEB_VEC_MESHCORE_NODE2, FEB_VEC_MESHCORE_NODE2_LEN,
                                                 &node, &status);
    ok = (encoded_len == FEB_VEC_MESHCORE_NODE2_LEN) && (status == FEB_CBOR_OK);
    ok = ok && !node.has_name;
    ok = ok && node.rssi_offset == 0 /* -128 + 128, low extreme */;
    ok = ok && node.has_location && node.lat_e7_offset == (uint64_t)(423601000 + 900000000) &&
         node.lon_e7_offset == (uint64_t)(1800000000 - 710589000);
    check(ok, "meshcore node2: no name (optional omission), has location, rssi at low extreme");

    encoded_len = feb_cbor_encode_meshcore_node(encode_buf, sizeof(encode_buf), &node);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHCORE_NODE2, FEB_VEC_MESHCORE_NODE2_LEN),
          "meshcore node2: encode round-trip byte-identical");

    encoded_len = feb_cbor_decode_meshcore_node(FEB_VEC_MESHCORE_NODE_BAD_LOCATION_PAIR,
                                                 FEB_VEC_MESHCORE_NODE_BAD_LOCATION_PAIR_LEN,
                                                 &node, &status);
    check(encoded_len == 0 && status == FEB_CBOR_ERR_MISSING_FIELD,
          "meshcore node (lat_e7_offset without lon_e7_offset): rejected FEB_CBOR_ERR_MISSING_FIELD");
}

static void test_meshcore_command_payload(void)
{
    feb_command_payload_t cmd;
    feb_cbor_status_t status;
    size_t arg_count;
    uint8_t encode_buf[128];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_command_payload(FEB_VEC_MESHCORE_COMMAND_PAYLOAD,
                                              FEB_VEC_MESHCORE_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && cmd.capability_len == strlen("meshcore_scan") &&
         memcmp(cmd.capability, "meshcore_scan", cmd.capability_len) == 0;
    ok = ok && cmd.request_id == 701;
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 0;
    check(ok, "meshcore_scan command payload: decodes capability/request_id/empty arguments");

    encoded_len = feb_cbor_encode_command_payload(encode_buf, sizeof(encode_buf), &cmd);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHCORE_COMMAND_PAYLOAD,
                   FEB_VEC_MESHCORE_COMMAND_PAYLOAD_LEN),
          "meshcore_scan command payload: encode round-trip byte-identical");

    status = feb_cbor_decode_command_payload(FEB_VEC_MESHCORE_COMMAND_BAD_ARGUMENTS_PAYLOAD,
                                              FEB_VEC_MESHCORE_COMMAND_BAD_ARGUMENTS_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 1;
    check(ok, "meshcore_scan command payload (non-empty arguments): decodes structurally OK; "
              "rejection is a main.c dispatch-layer concern (invalid_command), not a codec error");
}

static void test_meshcore_status_result_payload(void)
{
    feb_status_payload_t st;
    feb_meshcore_status_result_payload_t result;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_status_payload(FEB_VEC_MESHCORE_STATUS_PAYLOAD,
                                             FEB_VEC_MESHCORE_STATUS_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 701 && st.has_result;
    ok = ok && st.state_len == strlen("ok") && memcmp(st.state, "ok", st.state_len) == 0;
    check(ok, "meshcore_scan status: decodes request_id/state(\"ok\")/result");

    if (ok) {
        status = feb_cbor_decode_meshcore_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.node_count == 2 && result.total_known_nodes == 5;
    }
    check(ok, "meshcore_scan status: result decodes nodes[]/total_known_nodes "
              "(total_known_nodes > nodes.length signals a truncated listing)");

    if (ok) {
        encoded_len = feb_cbor_encode_meshcore_status_result_payload(encode_buf, sizeof(encode_buf), &result);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHCORE_RESULT_MULTI,
                       FEB_VEC_MESHCORE_RESULT_MULTI_LEN),
              "meshcore_scan status: result encode round-trip byte-identical");

        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHCORE_STATUS_PAYLOAD,
                       FEB_VEC_MESHCORE_STATUS_PAYLOAD_LEN),
              "meshcore_scan status: full status payload encode round-trip byte-identical");
    } else {
        check(0, "meshcore_scan status: result encode round-trip byte-identical");
        check(0, "meshcore_scan status: full status payload encode round-trip byte-identical");
    }

    status = feb_cbor_decode_status_payload(FEB_VEC_MESHCORE_STATUS_EMPTY_PAYLOAD,
                                             FEB_VEC_MESHCORE_STATUS_EMPTY_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.has_result;
    if (ok) {
        status = feb_cbor_decode_meshcore_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.node_count == 0 && result.total_known_nodes == 0;
    }
    check(ok, "meshcore_scan status (empty table): result decodes node_count=0/total_known_nodes=0");

    /* Worst-case sizing vector: FEB_MESHCORE_MAX_NODES_PER_RESULT nodes, each at every
       field's own simultaneous worst-case length -- confirms (via the real C decoder/
       encoder, not just Python arithmetic) that this still fits FEB_CBOR_MAX_PAYLOAD. */
    check(FEB_VEC_MESHCORE_STATUS_WORST_CASE_PAYLOAD_LEN <= FEB_CBOR_MAX_PAYLOAD,
          "meshcore_scan status (worst case): frozen vector itself fits FEB_CBOR_MAX_PAYLOAD");
    status = feb_cbor_decode_status_payload(FEB_VEC_MESHCORE_STATUS_WORST_CASE_PAYLOAD,
                                             FEB_VEC_MESHCORE_STATUS_WORST_CASE_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.has_result;
    if (ok) {
        status = feb_cbor_decode_meshcore_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.node_count == FEB_MESHCORE_MAX_NODES_PER_RESULT;
    }
    check(ok, "meshcore_scan status (worst case): decodes a full FEB_MESHCORE_MAX_NODES_PER_RESULT batch");
    if (ok) {
        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        check(encoded_len > 0 && encoded_len <= FEB_CBOR_MAX_PAYLOAD &&
              bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHCORE_STATUS_WORST_CASE_PAYLOAD,
                       FEB_VEC_MESHCORE_STATUS_WORST_CASE_PAYLOAD_LEN),
              "meshcore_scan status (worst case): re-encodes byte-identical and fits FEB_CBOR_MAX_PAYLOAD");
    } else {
        check(0, "meshcore_scan status (worst case): re-encodes byte-identical and fits FEB_CBOR_MAX_PAYLOAD");
    }
}

/* meshtastic_scan codec vectors (docs/PROTOCOL.md "`meshtastic_scan` command and status
   payloads", Heltec-only). Mirrors test_meshcore_node_roundtrip()/etc above -- same
   payload-codec-only strategy -- but <meshtastic-node> has no role/location fields, and
   FEB_MESHTASTIC_MAX_NODES_PER_RESULT/FEB_MESHTASTIC_NAME_MAX_LEN are both much smaller (see
   cbor_meshtastic.h's sizing-note comment on why: this board's DRAM budget, not a wire-size
   constraint). Also confirms FEB_MESHTASTIC_MAX_NODES_PER_RESULT's worst-case-sizing claim
   the same way the meshcore test does. */
static void test_meshtastic_node_roundtrip(void)
{
    feb_meshtastic_node_t node;
    feb_cbor_status_t status;
    uint8_t encode_buf[256];
    size_t encoded_len;
    int ok;

    encoded_len = feb_cbor_decode_meshtastic_node(FEB_VEC_MESHTASTIC_NODE1, FEB_VEC_MESHTASTIC_NODE1_LEN,
                                                   &node, &status);
    ok = (encoded_len == FEB_VEC_MESHTASTIC_NODE1_LEN) && (status == FEB_CBOR_OK);
    ok = ok && node.node_id_len == 8 && memcmp(node.node_id, "433d2b1c", 8) == 0;
    ok = ok && node.has_name && node.name_len == strlen("Bob") &&
         memcmp(node.name, "Bob", node.name_len) == 0;
    ok = ok && node.rssi_offset == 58 /* -70 + 128 */ && node.last_seen_ms == 12345;
    check(ok, "meshtastic node1: decodes node_id/name/rssi_offset/last_seen_ms");

    encoded_len = feb_cbor_encode_meshtastic_node(encode_buf, sizeof(encode_buf), &node);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHTASTIC_NODE1, FEB_VEC_MESHTASTIC_NODE1_LEN),
          "meshtastic node1: encode round-trip byte-identical");

    encoded_len = feb_cbor_decode_meshtastic_node(FEB_VEC_MESHTASTIC_NODE2, FEB_VEC_MESHTASTIC_NODE2_LEN,
                                                   &node, &status);
    ok = (encoded_len == FEB_VEC_MESHTASTIC_NODE2_LEN) && (status == FEB_CBOR_OK);
    ok = ok && !node.has_name;
    ok = ok && node.rssi_offset == 0 /* -128 + 128, low extreme */;
    check(ok, "meshtastic node2: no name (optional omission), rssi at low extreme");

    encoded_len = feb_cbor_encode_meshtastic_node(encode_buf, sizeof(encode_buf), &node);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHTASTIC_NODE2, FEB_VEC_MESHTASTIC_NODE2_LEN),
          "meshtastic node2: encode round-trip byte-identical");
}

static void test_meshtastic_command_payload(void)
{
    feb_command_payload_t cmd;
    feb_cbor_status_t status;
    size_t arg_count;
    uint8_t encode_buf[128];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_command_payload(FEB_VEC_MESHTASTIC_COMMAND_PAYLOAD,
                                              FEB_VEC_MESHTASTIC_COMMAND_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && cmd.capability_len == strlen("meshtastic_scan") &&
         memcmp(cmd.capability, "meshtastic_scan", cmd.capability_len) == 0;
    ok = ok && cmd.request_id == 801;
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 0;
    check(ok, "meshtastic_scan command payload: decodes capability/request_id/empty arguments");

    encoded_len = feb_cbor_encode_command_payload(encode_buf, sizeof(encode_buf), &cmd);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHTASTIC_COMMAND_PAYLOAD,
                   FEB_VEC_MESHTASTIC_COMMAND_PAYLOAD_LEN),
          "meshtastic_scan command payload: encode round-trip byte-identical");

    status = feb_cbor_decode_command_payload(FEB_VEC_MESHTASTIC_COMMAND_BAD_ARGUMENTS_PAYLOAD,
                                              FEB_VEC_MESHTASTIC_COMMAND_BAD_ARGUMENTS_PAYLOAD_LEN, &cmd);
    ok = (status == FEB_CBOR_OK);
    ok = ok && feb_cbor_decode_map_header(cmd.arguments_span, cmd.arguments_span_len, &arg_count, &status) > 0
            && arg_count == 1;
    check(ok, "meshtastic_scan command payload (non-empty arguments): decodes structurally OK; "
              "rejection is a main.c dispatch-layer concern (invalid_command), not a codec error");
}

static void test_meshtastic_status_result_payload(void)
{
    feb_status_payload_t st;
    feb_meshtastic_status_result_payload_t result;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_status_payload(FEB_VEC_MESHTASTIC_STATUS_PAYLOAD,
                                             FEB_VEC_MESHTASTIC_STATUS_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 801 && st.has_result;
    ok = ok && st.state_len == strlen("ok") && memcmp(st.state, "ok", st.state_len) == 0;
    check(ok, "meshtastic_scan status: decodes request_id/state(\"ok\")/result");

    if (ok) {
        status = feb_cbor_decode_meshtastic_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.node_count == 2 && result.total_known_nodes == 5;
    }
    check(ok, "meshtastic_scan status: result decodes nodes[]/total_known_nodes "
              "(total_known_nodes > nodes.length signals a truncated listing)");

    if (ok) {
        encoded_len = feb_cbor_encode_meshtastic_status_result_payload(encode_buf, sizeof(encode_buf), &result);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHTASTIC_RESULT_MULTI,
                       FEB_VEC_MESHTASTIC_RESULT_MULTI_LEN),
              "meshtastic_scan status: result encode round-trip byte-identical");

        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHTASTIC_STATUS_PAYLOAD,
                       FEB_VEC_MESHTASTIC_STATUS_PAYLOAD_LEN),
              "meshtastic_scan status: full status payload encode round-trip byte-identical");
    } else {
        check(0, "meshtastic_scan status: result encode round-trip byte-identical");
        check(0, "meshtastic_scan status: full status payload encode round-trip byte-identical");
    }

    status = feb_cbor_decode_status_payload(FEB_VEC_MESHTASTIC_STATUS_EMPTY_PAYLOAD,
                                             FEB_VEC_MESHTASTIC_STATUS_EMPTY_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.has_result;
    if (ok) {
        status = feb_cbor_decode_meshtastic_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.node_count == 0 && result.total_known_nodes == 0;
    }
    check(ok, "meshtastic_scan status (empty table): result decodes node_count=0/total_known_nodes=0");

    /* Worst-case sizing vector: FEB_MESHTASTIC_MAX_NODES_PER_RESULT nodes, each at every
       field's own simultaneous worst-case length -- confirms (via the real C decoder/
       encoder, not just Python arithmetic) that this still fits FEB_CBOR_MAX_PAYLOAD. */
    check(FEB_VEC_MESHTASTIC_STATUS_WORST_CASE_PAYLOAD_LEN <= FEB_CBOR_MAX_PAYLOAD,
          "meshtastic_scan status (worst case): frozen vector itself fits FEB_CBOR_MAX_PAYLOAD");
    status = feb_cbor_decode_status_payload(FEB_VEC_MESHTASTIC_STATUS_WORST_CASE_PAYLOAD,
                                             FEB_VEC_MESHTASTIC_STATUS_WORST_CASE_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.has_result;
    if (ok) {
        status = feb_cbor_decode_meshtastic_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.node_count == FEB_MESHTASTIC_MAX_NODES_PER_RESULT;
    }
    check(ok, "meshtastic_scan status (worst case): decodes a full FEB_MESHTASTIC_MAX_NODES_PER_RESULT batch");
    if (ok) {
        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        check(encoded_len > 0 && encoded_len <= FEB_CBOR_MAX_PAYLOAD &&
              bytes_eq(encode_buf, encoded_len, FEB_VEC_MESHTASTIC_STATUS_WORST_CASE_PAYLOAD,
                       FEB_VEC_MESHTASTIC_STATUS_WORST_CASE_PAYLOAD_LEN),
              "meshtastic_scan status (worst case): re-encodes byte-identical and fits FEB_CBOR_MAX_PAYLOAD");
    } else {
        check(0, "meshtastic_scan status (worst case): re-encodes byte-identical and fits FEB_CBOR_MAX_PAYLOAD");
    }
}

/* mesh_log status payloads (docs/PROTOCOL.md "`mesh_log` command and status payloads",
   Heltec-only, docs/WARDRIVING_PUBLISH.md "Mesh node publishing"). No command payload test
   exists for this capability -- see cbor_mesh_log.h's top comment: the ESP32 never decodes a
   `command` for it at all, only encodes/decodes the record and status.result shapes. */
static void test_mesh_log_record_roundtrip(void)
{
    feb_mesh_log_record_t record;
    feb_cbor_status_t status;
    uint8_t encode_buf[256];
    size_t encoded_len;
    int ok;

    encoded_len = feb_cbor_decode_mesh_log_record(FEB_VEC_MESH_LOG_RECORD1, FEB_VEC_MESH_LOG_RECORD1_LEN,
                                                  &record, &status);
    ok = (encoded_len == FEB_VEC_MESH_LOG_RECORD1_LEN) && (status == FEB_CBOR_OK);
    ok = ok && record.node_id_len == 16 && memcmp(record.node_id, "aabbccddeeff0011", 16) == 0;
    ok = ok && record.network_len == strlen("meshcore") && memcmp(record.network, "meshcore", record.network_len) == 0;
    ok = ok && record.lat_e7_offset == (uint64_t)(423601000 + 900000000) &&
         record.lon_e7_offset == (uint64_t)(1800000000 - 710589000);
    check(ok, "mesh_log record1: decodes node_id/network/lat_e7_offset/lon_e7_offset (MeshCore-shaped)");

    encoded_len = feb_cbor_encode_mesh_log_record(encode_buf, sizeof(encode_buf), &record);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESH_LOG_RECORD1, FEB_VEC_MESH_LOG_RECORD1_LEN),
          "mesh_log record1: encode round-trip byte-identical");

    encoded_len = feb_cbor_decode_mesh_log_record(FEB_VEC_MESH_LOG_RECORD2, FEB_VEC_MESH_LOG_RECORD2_LEN,
                                                  &record, &status);
    ok = (encoded_len == FEB_VEC_MESH_LOG_RECORD2_LEN) && (status == FEB_CBOR_OK);
    ok = ok && record.node_id_len == 8 && memcmp(record.node_id, "433d2b1c", 8) == 0;
    ok = ok && record.network_len == strlen("meshtastic") &&
         memcmp(record.network, "meshtastic", record.network_len) == 0;
    check(ok, "mesh_log record2: decodes a shorter Meshtastic-shaped 8-hex node_id");

    encoded_len = feb_cbor_encode_mesh_log_record(encode_buf, sizeof(encode_buf), &record);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESH_LOG_RECORD2, FEB_VEC_MESH_LOG_RECORD2_LEN),
          "mesh_log record2: encode round-trip byte-identical");

    encoded_len = feb_cbor_decode_mesh_log_record(FEB_VEC_MESH_LOG_RECORD_BAD_FIELD,
                                                  FEB_VEC_MESH_LOG_RECORD_BAD_FIELD_LEN,
                                                  &record, &status);
    check(encoded_len == 0 && status == FEB_CBOR_ERR_UNEXPECTED_TYPE,
          "mesh_log record (unrecognized field in place of lon_e7_offset): rejected FEB_CBOR_ERR_UNEXPECTED_TYPE");
}

static void test_mesh_log_status_result_payload(void)
{
    feb_status_payload_t st;
    feb_mesh_log_status_result_payload_t result;
    feb_cbor_status_t status;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    status = feb_cbor_decode_status_payload(FEB_VEC_MESH_LOG_STATUS_PAYLOAD,
                                             FEB_VEC_MESH_LOG_STATUS_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.request_id == 0 && st.has_result;
    ok = ok && st.state_len == strlen("mesh_data") && memcmp(st.state, "mesh_data", st.state_len) == 0;
    check(ok, "mesh_log status: decodes request_id(0)/state(\"mesh_data\")/result");

    if (ok) {
        status = feb_cbor_decode_mesh_log_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.record_count == 1 && result.backlog_remaining == 0;
    }
    check(ok, "mesh_log status: result decodes records[]/backlog_remaining (caught up to live)");

    if (ok) {
        encoded_len = feb_cbor_encode_mesh_log_status_result_payload(encode_buf, sizeof(encode_buf), &result);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESH_LOG_RESULT_ONE, FEB_VEC_MESH_LOG_RESULT_ONE_LEN),
              "mesh_log status: result encode round-trip byte-identical");

        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        check(bytes_eq(encode_buf, encoded_len, FEB_VEC_MESH_LOG_STATUS_PAYLOAD,
                       FEB_VEC_MESH_LOG_STATUS_PAYLOAD_LEN),
              "mesh_log status: full status payload encode round-trip byte-identical");
    } else {
        check(0, "mesh_log status: result encode round-trip byte-identical");
        check(0, "mesh_log status: full status payload encode round-trip byte-identical");
    }

    status = feb_cbor_decode_status_payload(FEB_VEC_MESH_LOG_STATUS_MORE_PENDING_PAYLOAD,
                                             FEB_VEC_MESH_LOG_STATUS_MORE_PENDING_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.has_result;
    if (ok) {
        status = feb_cbor_decode_mesh_log_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.record_count == 1 && result.backlog_remaining == 3;
    }
    check(ok, "mesh_log status (more pending): backlog_remaining > 0 with only one record in this reply");

    status = feb_cbor_decode_status_payload(FEB_VEC_MESH_LOG_STATUS_EMPTY_PAYLOAD,
                                             FEB_VEC_MESH_LOG_STATUS_EMPTY_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.has_result;
    if (ok) {
        status = feb_cbor_decode_mesh_log_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.record_count == 0 && result.backlog_remaining == 0;
    }
    check(ok, "mesh_log status (empty): result decodes record_count=0/backlog_remaining=0");

    /* Worst-case sizing vector: the one record FEB_MESH_LOG_MAX_RECORDS_PER_BATCH allows, at
       every field's own simultaneous worst-case length -- confirms (via the real C decoder/
       encoder, not just Python arithmetic) that this still fits FEB_CBOR_MAX_PAYLOAD. */
    check(FEB_VEC_MESH_LOG_STATUS_WORST_CASE_PAYLOAD_LEN <= FEB_CBOR_MAX_PAYLOAD,
          "mesh_log status (worst case): frozen vector itself fits FEB_CBOR_MAX_PAYLOAD");
    status = feb_cbor_decode_status_payload(FEB_VEC_MESH_LOG_STATUS_WORST_CASE_PAYLOAD,
                                             FEB_VEC_MESH_LOG_STATUS_WORST_CASE_PAYLOAD_LEN, &st);
    ok = (status == FEB_CBOR_OK) && st.has_result;
    if (ok) {
        status = feb_cbor_decode_mesh_log_status_result_payload(st.result_span, st.result_span_len, &result);
        ok = (status == FEB_CBOR_OK) && result.record_count == FEB_MESH_LOG_MAX_RECORDS_PER_BATCH;
    }
    check(ok, "mesh_log status (worst case): decodes a full FEB_MESH_LOG_MAX_RECORDS_PER_BATCH batch");
    if (ok) {
        encoded_len = feb_cbor_encode_status_payload(encode_buf, sizeof(encode_buf), &st);
        check(encoded_len > 0 && encoded_len <= FEB_CBOR_MAX_PAYLOAD &&
              bytes_eq(encode_buf, encoded_len, FEB_VEC_MESH_LOG_STATUS_WORST_CASE_PAYLOAD,
                       FEB_VEC_MESH_LOG_STATUS_WORST_CASE_PAYLOAD_LEN),
              "mesh_log status (worst case): re-encodes byte-identical and fits FEB_CBOR_MAX_PAYLOAD");
    } else {
        check(0, "mesh_log status (worst case): re-encodes byte-identical and fits FEB_CBOR_MAX_PAYLOAD");
    }
}

/* HP-25/HP-26/HP-38 shared-layer strictness (tests/vectors/vectors.h). Same expected status
   on both firmwares, independent of size_t width. */
static void test_shared_strictness_vectors(void)
{
    feb_unencrypted_record_t rec;
    feb_command_payload_t cmd;
    feb_status_payload_t stp;
    feb_cbor_status_t st = FEB_CBOR_OK;
    size_t count = 0;
    size_t n;
    const uint8_t *span;
    size_t span_len;
    static const uint8_t map_head_u32_max[] = {0xBA, 0xFF, 0xFF, 0xFF, 0xFF};
    static const char name32[] = "0123456789abcdef0123456789abcdef";

    check(feb_cbor_decode_unencrypted(FEB_VEC_MAP_HEAD_COUNT_2POW32_PLUS_5_RECORD,
                                         FEB_VEC_MAP_HEAD_COUNT_2POW32_PLUS_5_RECORD_LEN, &rec) ==
                 FEB_CBOR_ERR_TOO_LARGE,
             "HP-25: record with 9-byte map count 2^32+5 rejected FEB_CBOR_ERR_TOO_LARGE");
    n = feb_cbor_decode_map_header(FEB_VEC_MAP_HEAD_COUNT_2POW32_PLUS_5_RECORD,
                                   FEB_VEC_MAP_HEAD_COUNT_2POW32_PLUS_5_RECORD_LEN, &count, &st);
    check(n == 0 && st == FEB_CBOR_ERR_TOO_LARGE, "HP-25: map header count 2^32+5 -> FEB_CBOR_ERR_TOO_LARGE");
    n = feb_cbor_decode_array_header(FEB_VEC_ARRAY_HEAD_COUNT_2POW32_PLUS_1,
                                     FEB_VEC_ARRAY_HEAD_COUNT_2POW32_PLUS_1_LEN, &count, &st);
    check(n == 0 && st == FEB_CBOR_ERR_TOO_LARGE, "HP-25: array header count 2^32+1 -> FEB_CBOR_ERR_TOO_LARGE");
    n = feb_cbor_skip_value(FEB_VEC_ARRAY_HEAD_COUNT_2POW32_PLUS_1, FEB_VEC_ARRAY_HEAD_COUNT_2POW32_PLUS_1_LEN, 0,
                            &span, &span_len, &st);
    check(n == 0 && st == FEB_CBOR_ERR_TOO_LARGE, "HP-25: skip_value array count 2^32+1 -> FEB_CBOR_ERR_TOO_LARGE");
    n = feb_cbor_decode_map_header(map_head_u32_max, sizeof(map_head_u32_max), &count, &st);
    check(n == 5 && st == FEB_CBOR_OK && count == 0xFFFFFFFFu, "HP-25: map header count UINT32_MAX still decodes");
    check(feb_cbor_decode_unencrypted(FEB_VEC_RECORD_VERSION_2POW32_PLUS_2, FEB_VEC_RECORD_VERSION_2POW32_PLUS_2_LEN,
                                         &rec) == FEB_CBOR_ERR_TOO_LARGE,
             "HP-25: record version 2^32+2 rejected FEB_CBOR_ERR_TOO_LARGE");

    check(feb_cbor_decode_command_payload(FEB_VEC_COMMAND_ARGUMENTS_NOT_MAP_PAYLOAD,
                                             FEB_VEC_COMMAND_ARGUMENTS_NOT_MAP_PAYLOAD_LEN, &cmd) ==
                 FEB_CBOR_ERR_UNEXPECTED_TYPE,
             "HP-26: command.arguments not a map rejected FEB_CBOR_ERR_UNEXPECTED_TYPE");
    check(feb_cbor_decode_status_payload(FEB_VEC_STATUS_RESULT_NOT_MAP_PAYLOAD,
                                            FEB_VEC_STATUS_RESULT_NOT_MAP_PAYLOAD_LEN, &stp) ==
                 FEB_CBOR_ERR_UNEXPECTED_TYPE,
             "HP-26: status.result not a map rejected FEB_CBOR_ERR_UNEXPECTED_TYPE");

    {
        feb_ble_scan_device_t dev;
        feb_wardriving_record_t wr;
        uint8_t out[256];

        memset(&dev, 0, sizeof(dev));
        dev.has_name = 1;
        dev.name = name32;
        dev.name_len = FEB_BLE_SCAN_NAME_MAX_LEN;
        dev.rssi_offset = 100;
        dev.addr_type = "public";
        dev.addr_type_len = 6;
        check(feb_cbor_encode_ble_scan_device(out, sizeof(out), &dev) > 0,
                 "HP-38: ble_scan device name of FEB_BLE_SCAN_NAME_MAX_LEN encodes");
        dev.name_len = FEB_BLE_SCAN_NAME_MAX_LEN + 1u;
        check(feb_cbor_encode_ble_scan_device(out, sizeof(out), &dev) == 0,
                 "HP-38: ble_scan device name of FEB_BLE_SCAN_NAME_MAX_LEN + 1 refused");

        memset(&wr, 0, sizeof(wr));
        wr.timestamp_ms = 1;
        wr.utc_timestamp_s = 1;
        wr.lat_e7_offset = 900000000u;
        wr.lon_e7_offset = 1800000000u;
        wr.source = "ble";
        wr.source_len = 3;
        wr.payload_kind = FEB_WARDRIVING_PAYLOAD_BLE;
        wr.payload.ble.has_name = 1;
        wr.payload.ble.name = name32;
        wr.payload.ble.name_len = FEB_WARDRIVING_BLE_NAME_MAX_LEN;
        wr.payload.ble.rssi_offset = 100;
        check(feb_cbor_encode_wardriving_record(out, sizeof(out), &wr) > 0,
                 "HP-38: wardriving ble name of FEB_WARDRIVING_BLE_NAME_MAX_LEN encodes");
        wr.payload.ble.name_len = FEB_WARDRIVING_BLE_NAME_MAX_LEN + 1u;
        check(feb_cbor_encode_wardriving_record(out, sizeof(out), &wr) == 0,
                 "HP-38: wardriving ble name of FEB_WARDRIVING_BLE_NAME_MAX_LEN + 1 refused");
    }
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
    {
        /* G02: fragment 0 declares a 4-byte payload capacity; fragment 1 tries to smuggle
           a 5-byte payload through. Must be rejected even though the running total
           (4 + 5 = 9 bytes) is nowhere near FEB_MAX_RECORD_SIZE. */
        feb_reassembly_t r;
        const uint8_t *out_record = NULL;
        size_t out_len = 0;
        feb_frame_status_t st;
        static const uint8_t frag0[] = {0x00, 0x2a, 0x00, 0x02, 0xaa, 0xbb, 0xcc, 0xdd};
        static const uint8_t frag1_oversized[] = {0x00, 0x2a, 0x01, 0x02,
                                                   0x11, 0x22, 0x33, 0x44, 0x55};

        feb_reassembly_reset(&r);
        st = feb_reassembly_feed(&r, frag0, sizeof(frag0), 0, &out_record, &out_len);
        check(st == FEB_FRAME_OK, "mid-message oversized fragment: fragment 0 accepted");

        st = feb_reassembly_feed(&r, frag1_oversized, sizeof(frag1_oversized), 0,
                                  &out_record, &out_len);
        check(st == FEB_FRAME_OVERSIZED && r.in_progress == 0,
              "mid-message fragment exceeding fragment-0 capacity rejected and reassembly reset");
    }
    {
        /* G01: nonzero flags must be rejected regardless of an otherwise well-formed
           single-fragment header. */
        feb_reassembly_t r;
        const uint8_t *out_record = NULL;
        size_t out_len = 0;
        feb_frame_status_t st;
        static const uint8_t frag_bad_flags[] = {0x01, 0x2a, 0x00, 0x01, 0x99};

        feb_reassembly_reset(&r);
        st = feb_reassembly_feed(&r, frag_bad_flags, sizeof(frag_bad_flags), 0,
                                  &out_record, &out_len);
        check(st == FEB_FRAME_INVALID_HEADER && r.in_progress == 0,
              "nonzero flags rejected as invalid header");
    }

    test_decode_error_record();
    test_encode_roundtrip();
    test_malformed_cbor();
    test_skip_value_direct();
    test_payload_type_depth_and_trailing();

    {
        static const uint8_t ap1_ssid[] = {'T','e','s','t','N','e','t','w','o','r','k'};
        static const uint8_t ap2_ssid[] = {0};
        static const uint8_t ap3_ssid[] = {0xff, 0xfe, 0x00, 0x41};

        test_wifi_scan_ap_roundtrip(FEB_VEC_WIFI_SCAN_AP1, FEB_VEC_WIFI_SCAN_AP1_LEN,
                                    ap1_ssid, sizeof(ap1_ssid), 78, 6, "11n", "wpa2_psk",
                                    "wifi_scan AP1 (normal entry)");
        test_wifi_scan_ap_roundtrip(FEB_VEC_WIFI_SCAN_AP2, FEB_VEC_WIFI_SCAN_AP2_LEN,
                                    ap2_ssid, 0, 0, 1, "11b", "open",
                                    "wifi_scan AP2 (rssi_offset=0 boundary, hidden SSID)");
        test_wifi_scan_ap_roundtrip(FEB_VEC_WIFI_SCAN_AP3, FEB_VEC_WIFI_SCAN_AP3_LEN,
                                    ap3_ssid, sizeof(ap3_ssid), 255, 11, "11ax", "unknown",
                                    "wifi_scan AP3 (rssi_offset=255 boundary, non-UTF-8 SSID, auth=unknown)");
    }
    test_wifi_scan_result_payload();
    test_wifi_scan_result_payload_stream();
    test_wifi_scan_command_payload();
    test_wifi_scan_status_payload();

    {
        static const uint8_t device1_addr[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        static const uint8_t device2_addr[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

        test_ble_scan_device_roundtrip(FEB_VEC_BLE_SCAN_DEVICE1, FEB_VEC_BLE_SCAN_DEVICE1_LEN,
                                        device1_addr, 1, "MyPhone", 68, "public",
                                        "ble_scan DEVICE1 (normal entry, named)");
        test_ble_scan_device_roundtrip(FEB_VEC_BLE_SCAN_DEVICE2, FEB_VEC_BLE_SCAN_DEVICE2_LEN,
                                        device2_addr, 0, NULL, 0, "random",
                                        "ble_scan DEVICE2 (no name, rssi_offset=0 boundary)");
    }
    test_ble_scan_result_payload();
    test_ble_scan_result_payload_stream();
    test_ble_scan_command_payload();
    test_ble_scan_status_payload();

    test_wardriving_command_payload();
    test_wardriving_record_roundtrip();
    test_wardriving_status_result_payload();
    test_wardriving_status_result_payload_stream();

    test_gps_command_payload();
    test_gps_status_payload();

    test_meshcore_node_roundtrip();
    test_meshcore_command_payload();
    test_meshcore_status_result_payload();

    test_meshtastic_node_roundtrip();
    test_meshtastic_command_payload();
    test_meshtastic_status_result_payload();

    test_mesh_log_record_roundtrip();
    test_mesh_log_status_result_payload();
    test_shared_strictness_vectors();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

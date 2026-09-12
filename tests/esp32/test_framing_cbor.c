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

/* docs/CODE_REVIEW_FIX_PLAN.md W1-W3: feb_cbor_skip_value() itself has zero direct test
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
   envelope decoder (docs/CODE_REVIEW_FIX_PLAN.md W1-W3, W6). */
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
             result.altitude_dm_offset == 1000529;
    }
    check(ok, "gps status (fix): result decodes fix_quality/satellites/hdop_e1/utc_timestamp_s/"
              "altitude_dm_offset");

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
    test_ble_scan_command_payload();
    test_ble_scan_status_payload();

    test_wardriving_command_payload();
    test_wardriving_record_roundtrip();
    test_wardriving_status_result_payload();

    test_gps_command_payload();
    test_gps_status_payload();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

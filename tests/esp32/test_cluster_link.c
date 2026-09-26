/* Host-native test driver for components/feb_cluster_link (docs/CLUSTER.md's "Inter-board
   protocol" frame spec, frozen 2026-09-26). Mirrors test_framing_cbor.c's shape/harness:
   plain check()/printf, no test framework, exit code carries pass/fail. */
#include <stdio.h>
#include <string.h>

#include "cluster_link.h"
#include "crc16.h"

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

/* Crafts a frame directly at the byte level, bypassing the per-message encode_*
   functions, so malformed/boundary frames (oversized length, a length that lies about
   the payload actually present) can be constructed for negative tests. CRC is computed
   correctly over [msg_type, payload_len, payload] unless `corrupt_crc` is set, in which
   case the transmitted CRC is deliberately wrong. `declared_len` is what's written into
   the length field; `payload`/`payload_len` is what's actually appended after it (the two
   may differ, to build a frame whose declared length doesn't match its actual bytes). */
static size_t build_raw_frame(uint8_t *out, uint8_t msg_type, uint16_t declared_len,
                                const uint8_t *payload, size_t payload_len, int corrupt_crc)
{
    size_t pos = 0;
    uint16_t crc;

    out[pos++] = FEB_CLUSTER_SOF0;
    out[pos++] = FEB_CLUSTER_SOF1;
    out[pos++] = msg_type;
    out[pos++] = (uint8_t)(declared_len & 0xFFu);
    out[pos++] = (uint8_t)((declared_len >> 8) & 0xFFu);
    if (payload_len > 0) {
        memcpy(out + pos, payload, payload_len);
        pos += payload_len;
    }

    crc = feb_cluster_crc16(out + 2, 3u + payload_len);
    if (corrupt_crc) {
        crc = (uint16_t)(crc ^ 0xFFFFu);
    }
    out[pos++] = (uint8_t)(crc & 0xFFu);
    out[pos++] = (uint8_t)((crc >> 8) & 0xFFu);
    return pos;
}

static void test_crc16_known_vector(void)
{
    static const uint8_t vec[] = "123456789";

    /* Standard CRC16/CCITT-FALSE check value, https://reveng.sourceforge.io/crc-catalogue/. */
    check(feb_cluster_crc16(vec, 9) == 0x29B1u, "crc16: CCITT-FALSE check value for '123456789'");
}

static void test_roundtrip_worker_hello(void)
{
    uint8_t buf[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t len;
    feb_cluster_worker_hello_t msg;
    feb_cluster_worker_hello_t decoded;
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t frame;
    feb_cluster_decode_result_t result = FEB_CLUSTER_DECODE_NEED_MORE;
    size_t i;

    memset(&frame, 0, sizeof(frame));
    msg.band = FEB_CLUSTER_BAND_5GHZ;
    len = feb_cluster_encode_worker_hello(buf, sizeof(buf), &msg);
    check(len == 2u + 1u + 2u + 1u + 2u, "worker_hello: encoded length matches frame layout");

    feb_cluster_decoder_init(&dec);
    for (i = 0; i < len; i++) {
        result = feb_cluster_decoder_feed_byte(&dec, buf[i], &frame);
    }
    check(result == FEB_CLUSTER_DECODE_FRAME_READY, "worker_hello: decoder reports frame ready");
    check(frame.msg_type == (uint8_t)FEB_CLUSTER_MSG_WORKER_HELLO, "worker_hello: msg_type decoded");
    check(feb_cluster_decode_worker_hello(&frame, &decoded) == 1 && decoded.band == FEB_CLUSTER_BAND_5GHZ,
          "worker_hello: round-trip band matches");
}

static void test_roundtrip_scan_config_set(void)
{
    uint8_t buf[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t len;
    feb_cluster_scan_config_t msg;
    feb_cluster_scan_config_t decoded;
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t frame;
    size_t produced;

    msg.mode = FEB_CLUSTER_SCAN_MODE_CONTINUOUS;
    msg.dwell_mode = FEB_CLUSTER_DWELL_SPEED_BASED;
    msg.band_filter = FEB_CLUSTER_BAND_FILTER_FULL;
    len = feb_cluster_encode_scan_config_set(buf, sizeof(buf), &msg);
    check(len == 2u + 1u + 2u + 3u + 2u, "scan_config_set: encoded length matches frame layout");

    feb_cluster_decoder_init(&dec);
    produced = feb_cluster_decoder_feed(&dec, buf, len, &frame, 1);
    check(produced == 1, "scan_config_set: decoder_feed produces one frame");
    check(feb_cluster_decode_scan_config_set(&frame, &decoded) == 1 &&
              decoded.mode == FEB_CLUSTER_SCAN_MODE_CONTINUOUS &&
              decoded.dwell_mode == FEB_CLUSTER_DWELL_SPEED_BASED &&
              decoded.band_filter == FEB_CLUSTER_BAND_FILTER_FULL,
          "scan_config_set: round-trip fields match");
}

static void test_roundtrip_scan_result(void)
{
    uint8_t buf[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t len;
    feb_cluster_scan_result_t msg;
    feb_cluster_scan_result_t decoded;
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t frame;
    size_t produced;
    static const uint8_t bssid[FEB_CLUSTER_SCAN_RESULT_BSSID_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

    memset(&msg, 0, sizeof(msg));
    memcpy(msg.ssid, "cluster-test-ap", 15);
    msg.ssid_len = 15;
    memcpy(msg.bssid, bssid, sizeof(bssid));
    msg.rssi = -67;
    msg.channel = 11;
    msg.phy = FEB_CLUSTER_PHY_11AX;
    msg.auth = FEB_CLUSTER_AUTH_WPA2_PSK;

    len = feb_cluster_encode_scan_result(buf, sizeof(buf), &msg);
    check(len == 2u + 1u + 2u + (1u + 15u + 6u + 1u + 1u + 1u + 1u) + 2u,
          "scan_result: encoded length matches frame layout");

    feb_cluster_decoder_init(&dec);
    produced = feb_cluster_decoder_feed(&dec, buf, len, &frame, 1);
    check(produced == 1, "scan_result: decoder_feed produces one frame");
    check(feb_cluster_decode_scan_result(&frame, &decoded) == 1 &&
              decoded.ssid_len == 15 && memcmp(decoded.ssid, "cluster-test-ap", 15) == 0 &&
              memcmp(decoded.bssid, bssid, sizeof(bssid)) == 0 &&
              decoded.rssi == -67 && decoded.channel == 11 &&
              decoded.phy == FEB_CLUSTER_PHY_11AX && decoded.auth == FEB_CLUSTER_AUTH_WPA2_PSK,
          "scan_result: round-trip fields match");
}

static void test_roundtrip_scan_batch_done(void)
{
    uint8_t buf[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t len;
    feb_cluster_scan_batch_done_t msg;
    feb_cluster_scan_batch_done_t decoded;
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t frame;
    size_t produced;

    msg.count = 4321;
    len = feb_cluster_encode_scan_batch_done(buf, sizeof(buf), &msg);
    check(len == 2u + 1u + 2u + 2u + 2u, "scan_batch_done: encoded length matches frame layout");

    feb_cluster_decoder_init(&dec);
    produced = feb_cluster_decoder_feed(&dec, buf, len, &frame, 1);
    check(produced == 1, "scan_batch_done: decoder_feed produces one frame");
    check(feb_cluster_decode_scan_batch_done(&frame, &decoded) == 1 && decoded.count == 4321,
          "scan_batch_done: round-trip count matches");
}

static void test_wrong_msg_type_rejected(void)
{
    uint8_t buf[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t len;
    feb_cluster_worker_hello_t hello_msg;
    feb_cluster_scan_config_t decoded_config;
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t frame;
    size_t produced;

    hello_msg.band = FEB_CLUSTER_BAND_24GHZ;
    len = feb_cluster_encode_worker_hello(buf, sizeof(buf), &hello_msg);
    feb_cluster_decoder_init(&dec);
    produced = feb_cluster_decoder_feed(&dec, buf, len, &frame, 1);
    check(produced == 1 && feb_cluster_decode_scan_config_set(&frame, &decoded_config) == 0,
          "decode_scan_config_set rejects a worker_hello frame (wrong msg_type)");
}

static void test_crc_rejection(void)
{
    uint8_t buf[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t len;
    uint8_t payload[3] = {1, 2, 3};
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t frame;
    feb_cluster_decode_result_t last = FEB_CLUSTER_DECODE_NEED_MORE;
    size_t i;

    len = build_raw_frame(buf, (uint8_t)FEB_CLUSTER_MSG_SCAN_CONFIG_SET, 3, payload, 3, /*corrupt_crc=*/1);

    feb_cluster_decoder_init(&dec);
    for (i = 0; i < len; i++) {
        last = feb_cluster_decoder_feed_byte(&dec, buf[i], &frame);
    }
    check(last == FEB_CLUSTER_DECODE_RESYNC, "corrupted CRC: decoder reports resync, not frame_ready");
}

static void test_resync_after_garbage_bytes(void)
{
    uint8_t stream[64];
    size_t pos = 0;
    uint8_t frame_buf[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t frame_len;
    feb_cluster_worker_hello_t msg;
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t frames[4];
    size_t produced;
    feb_cluster_worker_hello_t decoded;

    /* Garbage that includes a lone 0xFE not followed by 0xED (exercises the SEEK_SOF1
       "not a real SOF, and not a fresh SOF0 candidate either" branch) plus assorted
       unrelated noise bytes, none of which should ever be mistaken for a frame. */
    static const uint8_t garbage[] = {0x00, 0xAA, FEB_CLUSTER_SOF0, 0x11, 0x7F, 0x55};

    memcpy(stream + pos, garbage, sizeof(garbage));
    pos += sizeof(garbage);

    msg.band = FEB_CLUSTER_BAND_5GHZ;
    frame_len = feb_cluster_encode_worker_hello(frame_buf, sizeof(frame_buf), &msg);
    memcpy(stream + pos, frame_buf, frame_len);
    pos += frame_len;

    feb_cluster_decoder_init(&dec);
    produced = feb_cluster_decoder_feed(&dec, stream, pos, frames, 4);
    check(produced == 1, "resync after garbage: exactly one frame recovered");
    check(produced == 1 && feb_cluster_decode_worker_hello(&frames[0], &decoded) == 1 &&
              decoded.band == FEB_CLUSTER_BAND_5GHZ,
          "resync after garbage: recovered frame's fields are intact");
}

static void test_resync_after_oversized_length(void)
{
    uint8_t stream[64];
    size_t pos = 0;
    uint8_t bad_len_frame[16];
    size_t bad_len_frame_len;
    uint8_t good_frame[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t good_frame_len;
    feb_cluster_scan_batch_done_t msg;
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t frames[4];
    size_t produced;
    feb_cluster_scan_batch_done_t decoded;
    static const uint8_t junk_payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    /* Declares payload_len = 0xFFFF (far past FEB_CLUSTER_MAX_PAYLOAD) but only appends a
       few junk bytes plus a bogus CRC -- this must be rejected the instant the length
       field is read, without the decoder waiting to consume 0xFFFF payload bytes that
       were never sent. */
    bad_len_frame_len = build_raw_frame(bad_len_frame, (uint8_t)FEB_CLUSTER_MSG_SCAN_RESULT,
                                         0xFFFFu, junk_payload, sizeof(junk_payload), 0);
    memcpy(stream + pos, bad_len_frame, bad_len_frame_len);
    pos += bad_len_frame_len;

    msg.count = 7;
    good_frame_len = feb_cluster_encode_scan_batch_done(good_frame, sizeof(good_frame), &msg);
    memcpy(stream + pos, good_frame, good_frame_len);
    pos += good_frame_len;

    feb_cluster_decoder_init(&dec);
    produced = feb_cluster_decoder_feed(&dec, stream, pos, frames, 4);
    check(produced == 1, "resync after oversized length: exactly one frame recovered");
    check(produced == 1 && feb_cluster_decode_scan_batch_done(&frames[0], &decoded) == 1 &&
              decoded.count == 7,
          "resync after oversized length: recovered frame's fields are intact");
}

static void test_max_payload_boundary(void)
{
    uint8_t frame[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t frame_len;
    uint8_t payload[FEB_CLUSTER_MAX_PAYLOAD];
    feb_cluster_decoder_t dec;
    feb_cluster_frame_t decoded_frame;
    feb_cluster_decode_result_t result = FEB_CLUSTER_DECODE_NEED_MORE;
    size_t i;

    memset(&decoded_frame, 0, sizeof(decoded_frame));
    memset(payload, 0x42, sizeof(payload));

    /* Exactly FEB_CLUSTER_MAX_PAYLOAD (512): must be accepted, not treated as oversized. */
    frame_len = build_raw_frame(frame, (uint8_t)FEB_CLUSTER_MSG_SCAN_RESULT,
                                 (uint16_t)FEB_CLUSTER_MAX_PAYLOAD, payload, FEB_CLUSTER_MAX_PAYLOAD, 0);
    feb_cluster_decoder_init(&dec);
    for (i = 0; i < frame_len; i++) {
        result = feb_cluster_decoder_feed_byte(&dec, frame[i], &decoded_frame);
    }
    check(result == FEB_CLUSTER_DECODE_FRAME_READY && decoded_frame.payload_len == FEB_CLUSTER_MAX_PAYLOAD &&
              bytes_eq(decoded_frame.payload, decoded_frame.payload_len, payload, sizeof(payload)),
          "max payload boundary: exactly FEB_CLUSTER_MAX_PAYLOAD bytes accepted");

    /* FEB_CLUSTER_MAX_PAYLOAD + 1 (513): must be rejected immediately (resync), matching
       the oversized-length behavior, not silently truncated or accepted. */
    frame_len = build_raw_frame(frame, (uint8_t)FEB_CLUSTER_MSG_SCAN_RESULT,
                                 (uint16_t)(FEB_CLUSTER_MAX_PAYLOAD + 1u), payload, FEB_CLUSTER_MAX_PAYLOAD, 0);
    feb_cluster_decoder_init(&dec);
    result = FEB_CLUSTER_DECODE_NEED_MORE;
    for (i = 0; i < frame_len; i++) {
        feb_cluster_decode_result_t r = feb_cluster_decoder_feed_byte(&dec, frame[i], &decoded_frame);

        if (r == FEB_CLUSTER_DECODE_RESYNC) {
            result = r;
            break;
        }
    }
    check(result == FEB_CLUSTER_DECODE_RESYNC, "max payload boundary: FEB_CLUSTER_MAX_PAYLOAD+1 rejected");

    /* Encoder side of the same boundary: scan_result's ssid_len capped at
       FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN (32); 33 must be rejected (returns 0). */
    {
        feb_cluster_scan_result_t msg;
        size_t len;

        memset(&msg, 0, sizeof(msg));
        msg.ssid_len = FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN;
        len = feb_cluster_encode_scan_result(frame, sizeof(frame), &msg);
        check(len > 0, "encoder: ssid_len == max (32) accepted");

        msg.ssid_len = FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN + 1u;
        len = feb_cluster_encode_scan_result(frame, sizeof(frame), &msg);
        check(len == 0, "encoder: ssid_len == max+1 (33) rejected");
    }

    /* Buffer-too-small: an exact-size buffer succeeds, one byte smaller fails. */
    {
        feb_cluster_scan_batch_done_t msg;
        size_t exact_len;
        uint8_t small_buf[FEB_CLUSTER_MAX_FRAME_SIZE];

        msg.count = 1;
        exact_len = feb_cluster_encode_scan_batch_done(small_buf, 2u + 1u + 2u + 2u + 2u, &msg);
        check(exact_len == 2u + 1u + 2u + 2u + 2u, "encoder: exact-size out_cap succeeds");

        exact_len = feb_cluster_encode_scan_batch_done(small_buf, 2u + 1u + 2u + 2u + 2u - 1u, &msg);
        check(exact_len == 0, "encoder: out_cap one byte short is rejected");
    }
}

int main(void)
{
    test_crc16_known_vector();
    test_roundtrip_worker_hello();
    test_roundtrip_scan_config_set();
    test_roundtrip_scan_result();
    test_roundtrip_scan_batch_done();
    test_wrong_msg_type_rejected();
    test_crc_rejection();
    test_resync_after_garbage_bytes();
    test_resync_after_oversized_length();
    test_max_payload_boundary();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

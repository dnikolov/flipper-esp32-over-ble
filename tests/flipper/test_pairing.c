/* Host-native test for flipper/pairing_crypto.c and flipper/pairing.c against the shared
   vectors in tests/vectors/vectors.h. Built and run with MSVC (cl.exe); see
   build_pairing.ps1. Follows test_flipper_codec.c's CHECK()/g_total/g_failed pattern. */
#include <stdio.h>
#include <string.h>

#include "pairing_crypto.h"
#include "pairing.h"
#include "vectors.h"

static int g_total = 0;
static int g_failed = 0;

#define CHECK(cond, desc)                                  \
    do {                                                    \
        g_total++;                                          \
        if(cond) {                                            \
            printf("PASS: %s\n", desc);                        \
        } else {                                                \
            g_failed++;                                          \
            printf("FAIL: %s (line %d)\n", desc, __LINE__);       \
        }                                                          \
    } while(0)

static int bytes_equal(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

/* ---- X25519 ---- */

static void test_x25519_rfc7748_tc(void) {
    uint8_t out[FEB_X25519_KEY_LEN];

    feb_x25519(out, FEB_VEC_X25519_TC1_SCALAR, FEB_VEC_X25519_TC1_U);
    CHECK(
        bytes_equal(out, sizeof(out), FEB_VEC_X25519_TC1_OUTPUT, FEB_VEC_X25519_TC1_OUTPUT_LEN),
        "X25519 TC1: output matches RFC 7748 5.2 vector");

    feb_x25519(out, FEB_VEC_X25519_TC2_SCALAR, FEB_VEC_X25519_TC2_U);
    CHECK(
        bytes_equal(out, sizeof(out), FEB_VEC_X25519_TC2_OUTPUT, FEB_VEC_X25519_TC2_OUTPUT_LEN),
        "X25519 TC2: output matches RFC 7748 5.2 vector");
}

static void test_x25519_diffie_hellman(void) {
    uint8_t alice_pub[FEB_X25519_KEY_LEN];
    uint8_t bob_pub[FEB_X25519_KEY_LEN];
    uint8_t shared_from_alice[FEB_X25519_KEY_LEN];
    uint8_t shared_from_bob[FEB_X25519_KEY_LEN];

    feb_x25519_base(alice_pub, FEB_VEC_X25519_ALICE_PRIVATE);
    CHECK(
        bytes_equal(alice_pub, sizeof(alice_pub), FEB_VEC_X25519_ALICE_PUBLIC, FEB_VEC_X25519_ALICE_PUBLIC_LEN),
        "X25519 DH: Alice's derived public key matches RFC 7748 6.1 vector");

    feb_x25519_base(bob_pub, FEB_VEC_X25519_BOB_PRIVATE);
    CHECK(
        bytes_equal(bob_pub, sizeof(bob_pub), FEB_VEC_X25519_BOB_PUBLIC, FEB_VEC_X25519_BOB_PUBLIC_LEN),
        "X25519 DH: Bob's derived public key matches RFC 7748 6.1 vector");

    feb_x25519(shared_from_alice, FEB_VEC_X25519_ALICE_PRIVATE, FEB_VEC_X25519_BOB_PUBLIC);
    CHECK(
        bytes_equal(
            shared_from_alice, sizeof(shared_from_alice), FEB_VEC_X25519_SHARED, FEB_VEC_X25519_SHARED_LEN),
        "X25519 DH: Alice's computed shared secret matches RFC 7748 6.1 vector");

    feb_x25519(shared_from_bob, FEB_VEC_X25519_BOB_PRIVATE, FEB_VEC_X25519_ALICE_PUBLIC);
    CHECK(
        bytes_equal(
            shared_from_bob, sizeof(shared_from_bob), FEB_VEC_X25519_SHARED, FEB_VEC_X25519_SHARED_LEN),
        "X25519 DH: Bob's computed shared secret matches RFC 7748 6.1 vector");

    CHECK(
        bytes_equal(shared_from_alice, sizeof(shared_from_alice), shared_from_bob, sizeof(shared_from_bob)),
        "X25519 DH: Alice and Bob agree on the shared secret");
}

static void test_x25519_zero_point_and_all_zero_check(void) {
    uint8_t out[FEB_X25519_KEY_LEN];

    feb_x25519(out, FEB_VEC_X25519_TC1_SCALAR, FEB_VEC_X25519_ZERO_U);
    CHECK(
        bytes_equal(out, sizeof(out), FEB_VEC_X25519_ZERO_OUTPUT, FEB_VEC_X25519_ZERO_OUTPUT_LEN),
        "X25519: all-zero u-coordinate yields all-zero output for any scalar");

    CHECK(
        feb_is_all_zero(FEB_VEC_X25519_ZERO_OUTPUT, FEB_VEC_X25519_ZERO_OUTPUT_LEN) == 1,
        "feb_is_all_zero: flags the all-zero X25519 output");
    CHECK(
        feb_is_all_zero(FEB_VEC_X25519_SHARED, FEB_VEC_X25519_SHARED_LEN) == 0,
        "feb_is_all_zero: does not flag a real (non-zero) shared secret");
}

/* ---- SHA-256 / HMAC-SHA-256 / HKDF-SHA-256 ---- */

static void test_sha256(void) {
    uint8_t out[FEB_SHA256_LEN];
    feb_sha256(FEB_VEC_SHA256_INPUT, FEB_VEC_SHA256_INPUT_LEN, out);
    CHECK(
        bytes_equal(out, sizeof(out), FEB_VEC_SHA256_DIGEST, FEB_VEC_SHA256_DIGEST_LEN),
        "SHA-256: digest matches FIPS 180-4 short-message vector");
}

static void test_hmac_sha256(void) {
    uint8_t out[FEB_HMAC_SHA256_LEN];
    feb_hmac_sha256(FEB_VEC_HMAC_KEY, FEB_VEC_HMAC_KEY_LEN, FEB_VEC_HMAC_DATA, FEB_VEC_HMAC_DATA_LEN, out);
    CHECK(
        bytes_equal(out, sizeof(out), FEB_VEC_HMAC_MAC, FEB_VEC_HMAC_MAC_LEN),
        "HMAC-SHA-256: MAC matches RFC 4231 test case 1");
}

static void test_hkdf_sha256(void) {
    uint8_t out[32];
    int rc = feb_hkdf_sha256(
        FEB_VEC_HKDF_SALT,
        FEB_VEC_HKDF_SALT_LEN,
        FEB_VEC_HKDF_IKM,
        FEB_VEC_HKDF_IKM_LEN,
        FEB_VEC_HKDF_INFO,
        FEB_VEC_HKDF_INFO_LEN,
        out,
        sizeof(out));
    CHECK(rc == 0, "HKDF-SHA-256: L=32 call succeeds");
    CHECK(
        bytes_equal(out, sizeof(out), FEB_VEC_HKDF_OKM_L32, FEB_VEC_HKDF_OKM_L32_LEN),
        "HKDF-SHA-256: OKM matches RFC 5869 test case 1 (L=32 prefix)");

    CHECK(feb_hkdf_sha256(NULL, 0, FEB_VEC_HKDF_IKM, FEB_VEC_HKDF_IKM_LEN, NULL, 0, out, 0) != 0,
        "HKDF-SHA-256: out_len == 0 is rejected");
    CHECK(
        feb_hkdf_sha256(NULL, 0, FEB_VEC_HKDF_IKM, FEB_VEC_HKDF_IKM_LEN, NULL, 0, out, FEB_HKDF_MAX_LEN + 1) != 0,
        "HKDF-SHA-256: out_len > FEB_HKDF_MAX_LEN is rejected");
}

/* ---- constant-time helpers ---- */

static void test_consttime_equal(void) {
    static const uint8_t a[4] = {1, 2, 3, 4};
    static const uint8_t b_same[4] = {1, 2, 3, 4};
    static const uint8_t b_diff[4] = {1, 2, 3, 5};

    CHECK(feb_consttime_equal(a, b_same, 4) == 1, "feb_consttime_equal: equal buffers match");
    CHECK(feb_consttime_equal(a, b_diff, 4) == 0, "feb_consttime_equal: differing buffers don't match");
    CHECK(feb_consttime_equal(a, b_same, 0) == 0, "feb_consttime_equal: len == 0 returns 0");
}

/* ---- golden end-to-end pairing vector ---- */

static void test_golden_pairing_vector(void) {
    uint8_t transcript[FEB_PAIRING_MAX_TRANSCRIPT_LEN];
    size_t transcript_len;
    feb_pairing_transcript_t t;

    memset(&t, 0, sizeof(t));
    t.version = 2;
    memcpy(t.service_uuid, FEB_VEC_PAIR_SERVICE_UUID, FEB_PAIRING_SERVICE_UUID_LEN);
    t.board_id = FEB_VEC_PAIR_BOARD_ID;
    t.board_id_len = FEB_VEC_PAIR_BOARD_ID_LEN;
    memcpy(t.pairing_epoch, FEB_VEC_PAIR_EPOCH, FEB_PAIRING_EPOCH_LEN);
    memcpy(t.client_nonce, FEB_VEC_PAIR_CLIENT_NONCE, FEB_PAIRING_NONCE_LEN);
    memcpy(t.device_nonce, FEB_VEC_PAIR_DEVICE_NONCE, FEB_PAIRING_NONCE_LEN);
    memcpy(t.esp32_public_key, FEB_VEC_PAIR_ESP32_PUBLIC, FEB_PAIRING_PUBKEY_LEN);
    memcpy(t.flipper_public_key, FEB_VEC_PAIR_FLIPPER_PUBLIC, FEB_PAIRING_PUBKEY_LEN);

    transcript_len = feb_pairing_encode_transcript(transcript, sizeof(transcript), &t);
    CHECK(transcript_len > 0, "golden: feb_pairing_encode_transcript succeeds");
    CHECK(
        bytes_equal(transcript, transcript_len, FEB_VEC_PAIR_TRANSCRIPT, FEB_VEC_PAIR_TRANSCRIPT_LEN),
        "golden: T byte-identical to FEB_VEC_PAIR_TRANSCRIPT");

    uint8_t k_shared_from_esp32[FEB_PAIRING_KSHARED_LEN];
    uint8_t k_shared_from_flipper[FEB_PAIRING_KSHARED_LEN];
    feb_x25519(k_shared_from_esp32, FEB_VEC_PAIR_ESP32_PRIVATE, FEB_VEC_PAIR_FLIPPER_PUBLIC);
    feb_x25519(k_shared_from_flipper, FEB_VEC_PAIR_FLIPPER_PRIVATE, FEB_VEC_PAIR_ESP32_PUBLIC);
    CHECK(
        bytes_equal(
            k_shared_from_esp32, sizeof(k_shared_from_esp32), FEB_VEC_PAIR_K_SHARED, FEB_VEC_PAIR_K_SHARED_LEN),
        "golden: K_shared computed from ESP32 private key matches FEB_VEC_PAIR_K_SHARED");
    CHECK(
        bytes_equal(
            k_shared_from_flipper,
            sizeof(k_shared_from_flipper),
            FEB_VEC_PAIR_K_SHARED,
            FEB_VEC_PAIR_K_SHARED_LEN),
        "golden: K_shared computed from Flipper private key matches FEB_VEC_PAIR_K_SHARED");

    uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN];
    feb_pairing_derive_kconfirm(FEB_VEC_PAIR_K_SHARED, FEB_VEC_PAIR_EPOCH, k_confirm);
    CHECK(
        bytes_equal(k_confirm, sizeof(k_confirm), FEB_VEC_PAIR_K_CONFIRM, FEB_VEC_PAIR_K_CONFIRM_LEN),
        "golden: K_confirm matches FEB_VEC_PAIR_K_CONFIRM");

    uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN];
    feb_pairing_derive_secret(
        FEB_VEC_PAIR_K_SHARED,
        FEB_VEC_PAIR_EPOCH,
        FEB_VEC_PAIR_CLIENT_NONCE,
        FEB_VEC_PAIR_DEVICE_NONCE,
        FEB_VEC_PAIR_BOARD_ID,
        FEB_VEC_PAIR_BOARD_ID_LEN,
        pairing_secret);
    CHECK(
        bytes_equal(pairing_secret, sizeof(pairing_secret), FEB_VEC_PAIR_SECRET, FEB_VEC_PAIR_SECRET_LEN),
        "golden: pairing_secret matches FEB_VEC_PAIR_SECRET");

    uint8_t flipper_confirm[FEB_PAIRING_REPLY_CONFIRM_LEN];
    uint8_t esp32_confirm[FEB_PAIRING_REPLY_CONFIRM_LEN];
    uint8_t complete_tag[FEB_PAIRING_COMPLETE_TAG_LEN];
    feb_pairing_flipper_confirm(k_confirm, transcript, transcript_len, flipper_confirm);
    feb_pairing_esp32_confirm(k_confirm, transcript, transcript_len, esp32_confirm);
    feb_pairing_complete_tag(k_confirm, transcript, transcript_len, complete_tag);
    CHECK(
        bytes_equal(
            flipper_confirm,
            sizeof(flipper_confirm),
            FEB_VEC_PAIR_FLIPPER_CONFIRM,
            FEB_VEC_PAIR_FLIPPER_CONFIRM_LEN),
        "golden: flipper-confirm tag matches FEB_VEC_PAIR_FLIPPER_CONFIRM");
    CHECK(
        bytes_equal(
            esp32_confirm, sizeof(esp32_confirm), FEB_VEC_PAIR_ESP32_CONFIRM, FEB_VEC_PAIR_ESP32_CONFIRM_LEN),
        "golden: esp32-confirm tag matches FEB_VEC_PAIR_ESP32_CONFIRM");
    CHECK(
        bytes_equal(
            complete_tag, sizeof(complete_tag), FEB_VEC_PAIR_COMPLETE_TAG, FEB_VEC_PAIR_COMPLETE_TAG_LEN),
        "golden: complete tag matches FEB_VEC_PAIR_COMPLETE_TAG");
}

/* ---- payload/envelope encode+decode round trip against golden vectors ---- */

static void test_pair_init_codec(void) {
    feb_pair_init_payload_t p;
    uint8_t payload_buf[128];
    size_t payload_len;
    feb_pairing_envelope_t env;
    uint8_t record_buf[256];
    size_t record_len;

    memset(&p, 0, sizeof(p));
    memcpy(p.pairing_epoch, FEB_VEC_PAIR_EPOCH, FEB_PAIRING_EPOCH_LEN);
    memcpy(p.device_nonce, FEB_VEC_PAIR_DEVICE_NONCE, FEB_PAIRING_NONCE_LEN);
    memcpy(p.esp32_public_key, FEB_VEC_PAIR_ESP32_PUBLIC, FEB_PAIRING_PUBKEY_LEN);

    payload_len = feb_cbor_encode_pair_init_payload(payload_buf, sizeof(payload_buf), &p);
    CHECK(payload_len > 0, "pair_init: encode succeeds");
    CHECK(
        bytes_equal(payload_buf, payload_len, FEB_VEC_PAIR_INIT_PAYLOAD, FEB_VEC_PAIR_INIT_PAYLOAD_LEN),
        "pair_init: payload byte-identical to FEB_VEC_PAIR_INIT_PAYLOAD");

    {
        feb_pair_init_payload_t decoded;
        feb_cbor_status_t status =
            feb_cbor_decode_pair_init_payload(FEB_VEC_PAIR_INIT_PAYLOAD, FEB_VEC_PAIR_INIT_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "pair_init: decode status OK");
        CHECK(
            memcmp(decoded.pairing_epoch, FEB_VEC_PAIR_EPOCH, FEB_PAIRING_EPOCH_LEN) == 0 &&
                memcmp(decoded.device_nonce, FEB_VEC_PAIR_DEVICE_NONCE, FEB_PAIRING_NONCE_LEN) == 0 &&
                memcmp(decoded.esp32_public_key, FEB_VEC_PAIR_ESP32_PUBLIC, FEB_PAIRING_PUBKEY_LEN) == 0,
            "pair_init: decoded fields match golden vector");
    }

    memset(&env, 0, sizeof(env));
    env.version = 2;
    env.type = FEB_PAIR_INIT_TYPE;
    env.type_len = strlen(FEB_PAIR_INIT_TYPE);
    env.board_id = FEB_VEC_PAIR_BOARD_ID;
    env.board_id_len = FEB_VEC_PAIR_BOARD_ID_LEN;
    env.payload_span = payload_buf;
    env.payload_span_len = payload_len;

    record_len = feb_cbor_encode_pairing_envelope(record_buf, sizeof(record_buf), &env);
    CHECK(record_len > 0, "pair_init: envelope encode succeeds");
    CHECK(
        bytes_equal(record_buf, record_len, FEB_VEC_PAIR_INIT_RECORD, FEB_VEC_PAIR_INIT_RECORD_LEN),
        "pair_init: envelope byte-identical to FEB_VEC_PAIR_INIT_RECORD");

    {
        feb_pairing_envelope_t decoded_env;
        feb_cbor_status_t status = feb_cbor_decode_pairing_envelope(
            FEB_VEC_PAIR_INIT_RECORD, FEB_VEC_PAIR_INIT_RECORD_LEN, &decoded_env);
        CHECK(status == FEB_CBOR_OK, "pair_init: envelope decode status OK");
        CHECK(decoded_env.version == 2, "pair_init: envelope decoded version == 2");
        CHECK(
            decoded_env.type_len == strlen(FEB_PAIR_INIT_TYPE) &&
                memcmp(decoded_env.type, FEB_PAIR_INIT_TYPE, decoded_env.type_len) == 0,
            "pair_init: envelope decoded type == \"pair_init\"");
        CHECK(
            decoded_env.board_id_len == FEB_VEC_PAIR_BOARD_ID_LEN &&
                memcmp(decoded_env.board_id, FEB_VEC_PAIR_BOARD_ID, decoded_env.board_id_len) == 0,
            "pair_init: envelope decoded board_id matches golden vector");
        CHECK(
            bytes_equal(
                decoded_env.payload_span,
                decoded_env.payload_span_len,
                FEB_VEC_PAIR_INIT_PAYLOAD,
                FEB_VEC_PAIR_INIT_PAYLOAD_LEN),
            "pair_init: envelope decoded payload_span matches golden payload");
    }
}

static void test_pair_reply_codec(void) {
    feb_pair_reply_payload_t p;
    uint8_t payload_buf[160];
    size_t payload_len;
    feb_pairing_envelope_t env;
    uint8_t record_buf[256];
    size_t record_len;

    memset(&p, 0, sizeof(p));
    memcpy(p.client_nonce, FEB_VEC_PAIR_CLIENT_NONCE, FEB_PAIRING_NONCE_LEN);
    memcpy(p.flipper_public_key, FEB_VEC_PAIR_FLIPPER_PUBLIC, FEB_PAIRING_PUBKEY_LEN);
    memcpy(p.confirmation, FEB_VEC_PAIR_FLIPPER_CONFIRM, FEB_PAIRING_REPLY_CONFIRM_LEN);

    payload_len = feb_cbor_encode_pair_reply_payload(payload_buf, sizeof(payload_buf), &p);
    CHECK(payload_len > 0, "pair_reply: encode succeeds");
    CHECK(
        bytes_equal(payload_buf, payload_len, FEB_VEC_PAIR_REPLY_PAYLOAD, FEB_VEC_PAIR_REPLY_PAYLOAD_LEN),
        "pair_reply: payload byte-identical to FEB_VEC_PAIR_REPLY_PAYLOAD");

    {
        feb_pair_reply_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_pair_reply_payload(
            FEB_VEC_PAIR_REPLY_PAYLOAD, FEB_VEC_PAIR_REPLY_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "pair_reply: decode status OK");
        CHECK(
            memcmp(decoded.client_nonce, FEB_VEC_PAIR_CLIENT_NONCE, FEB_PAIRING_NONCE_LEN) == 0 &&
                memcmp(decoded.flipper_public_key, FEB_VEC_PAIR_FLIPPER_PUBLIC, FEB_PAIRING_PUBKEY_LEN) == 0 &&
                memcmp(decoded.confirmation, FEB_VEC_PAIR_FLIPPER_CONFIRM, FEB_PAIRING_REPLY_CONFIRM_LEN) == 0,
            "pair_reply: decoded fields match golden vector");
    }

    memset(&env, 0, sizeof(env));
    env.version = 2;
    env.type = FEB_PAIR_REPLY_TYPE;
    env.type_len = strlen(FEB_PAIR_REPLY_TYPE);
    env.board_id = FEB_VEC_PAIR_BOARD_ID;
    env.board_id_len = FEB_VEC_PAIR_BOARD_ID_LEN;
    env.payload_span = payload_buf;
    env.payload_span_len = payload_len;

    record_len = feb_cbor_encode_pairing_envelope(record_buf, sizeof(record_buf), &env);
    CHECK(record_len > 0, "pair_reply: envelope encode succeeds");
    CHECK(
        bytes_equal(record_buf, record_len, FEB_VEC_PAIR_REPLY_RECORD, FEB_VEC_PAIR_REPLY_RECORD_LEN),
        "pair_reply: envelope byte-identical to FEB_VEC_PAIR_REPLY_RECORD");

    {
        feb_pairing_envelope_t decoded_env;
        feb_cbor_status_t status = feb_cbor_decode_pairing_envelope(
            FEB_VEC_PAIR_REPLY_RECORD, FEB_VEC_PAIR_REPLY_RECORD_LEN, &decoded_env);
        CHECK(status == FEB_CBOR_OK, "pair_reply: envelope decode status OK");
        CHECK(
            bytes_equal(
                decoded_env.payload_span,
                decoded_env.payload_span_len,
                FEB_VEC_PAIR_REPLY_PAYLOAD,
                FEB_VEC_PAIR_REPLY_PAYLOAD_LEN),
            "pair_reply: envelope decoded payload_span matches golden payload");
    }
}

static void test_pair_confirm_codec(void) {
    feb_pair_confirm_payload_t p;
    uint8_t payload_buf[64];
    size_t payload_len;
    feb_pairing_envelope_t env;
    uint8_t record_buf[256];
    size_t record_len;

    memset(&p, 0, sizeof(p));
    memcpy(p.confirmation, FEB_VEC_PAIR_ESP32_CONFIRM, FEB_PAIRING_REPLY_CONFIRM_LEN);

    payload_len = feb_cbor_encode_pair_confirm_payload(payload_buf, sizeof(payload_buf), &p);
    CHECK(payload_len > 0, "pair_confirm: encode succeeds");
    CHECK(
        bytes_equal(
            payload_buf, payload_len, FEB_VEC_PAIR_CONFIRM_PAYLOAD, FEB_VEC_PAIR_CONFIRM_PAYLOAD_LEN),
        "pair_confirm: payload byte-identical to FEB_VEC_PAIR_CONFIRM_PAYLOAD");

    {
        feb_pair_confirm_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_pair_confirm_payload(
            FEB_VEC_PAIR_CONFIRM_PAYLOAD, FEB_VEC_PAIR_CONFIRM_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "pair_confirm: decode status OK");
        CHECK(
            memcmp(decoded.confirmation, FEB_VEC_PAIR_ESP32_CONFIRM, FEB_PAIRING_REPLY_CONFIRM_LEN) == 0,
            "pair_confirm: decoded confirmation matches golden vector");
    }

    memset(&env, 0, sizeof(env));
    env.version = 2;
    env.type = FEB_PAIR_CONFIRM_TYPE;
    env.type_len = strlen(FEB_PAIR_CONFIRM_TYPE);
    env.board_id = FEB_VEC_PAIR_BOARD_ID;
    env.board_id_len = FEB_VEC_PAIR_BOARD_ID_LEN;
    env.payload_span = payload_buf;
    env.payload_span_len = payload_len;

    record_len = feb_cbor_encode_pairing_envelope(record_buf, sizeof(record_buf), &env);
    CHECK(record_len > 0, "pair_confirm: envelope encode succeeds");
    CHECK(
        bytes_equal(
            record_buf, record_len, FEB_VEC_PAIR_CONFIRM_RECORD, FEB_VEC_PAIR_CONFIRM_RECORD_LEN),
        "pair_confirm: envelope byte-identical to FEB_VEC_PAIR_CONFIRM_RECORD");
}

static void test_pair_complete_codec(void) {
    feb_pair_complete_payload_t p;
    uint8_t payload_buf[64];
    size_t payload_len;
    feb_pairing_envelope_t env;
    uint8_t record_buf[256];
    size_t record_len;

    memset(&p, 0, sizeof(p));
    memcpy(p.confirmation, FEB_VEC_PAIR_COMPLETE_TAG, FEB_PAIRING_COMPLETE_TAG_LEN);

    payload_len = feb_cbor_encode_pair_complete_payload(payload_buf, sizeof(payload_buf), &p);
    CHECK(payload_len > 0, "pair_complete: encode succeeds");
    CHECK(
        bytes_equal(
            payload_buf, payload_len, FEB_VEC_PAIR_COMPLETE_PAYLOAD, FEB_VEC_PAIR_COMPLETE_PAYLOAD_LEN),
        "pair_complete: payload byte-identical to FEB_VEC_PAIR_COMPLETE_PAYLOAD");

    {
        feb_pair_complete_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_pair_complete_payload(
            FEB_VEC_PAIR_COMPLETE_PAYLOAD, FEB_VEC_PAIR_COMPLETE_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "pair_complete: decode status OK");
        CHECK(
            memcmp(decoded.confirmation, FEB_VEC_PAIR_COMPLETE_TAG, FEB_PAIRING_COMPLETE_TAG_LEN) == 0,
            "pair_complete: decoded confirmation matches golden vector");
    }

    memset(&env, 0, sizeof(env));
    env.version = 2;
    env.type = FEB_PAIR_COMPLETE_TYPE;
    env.type_len = strlen(FEB_PAIR_COMPLETE_TYPE);
    env.board_id = FEB_VEC_PAIR_BOARD_ID;
    env.board_id_len = FEB_VEC_PAIR_BOARD_ID_LEN;
    env.payload_span = payload_buf;
    env.payload_span_len = payload_len;

    record_len = feb_cbor_encode_pairing_envelope(record_buf, sizeof(record_buf), &env);
    CHECK(record_len > 0, "pair_complete: envelope encode succeeds");
    CHECK(
        bytes_equal(
            record_buf, record_len, FEB_VEC_PAIR_COMPLETE_RECORD, FEB_VEC_PAIR_COMPLETE_RECORD_LEN),
        "pair_complete: envelope byte-identical to FEB_VEC_PAIR_COMPLETE_RECORD");
}

/* ---- pairing-phase error envelope + malformed-input rejection ---- */

static void test_pair_error_codec(void) {
    feb_error_payload_t payload;
    feb_cbor_status_t status =
        feb_cbor_decode_error_payload(FEB_VEC_PAIR_ERROR_PAYLOAD, FEB_VEC_PAIR_ERROR_PAYLOAD_LEN, &payload);
    CHECK(status == FEB_CBOR_OK, "pair error: payload decode status OK");
    CHECK(
        payload.code_len == strlen("pairing_failed") &&
            memcmp(payload.code, "pairing_failed", payload.code_len) == 0,
        "pair error: code == \"pairing_failed\"");

    feb_pairing_envelope_t env;
    status = feb_cbor_decode_pairing_envelope(FEB_VEC_PAIR_ERROR_RECORD, FEB_VEC_PAIR_ERROR_RECORD_LEN, &env);
    CHECK(status == FEB_CBOR_OK, "pair error: envelope decode status OK");
    CHECK(
        env.type_len == strlen("error") && memcmp(env.type, "error", env.type_len) == 0,
        "pair error: envelope type == \"error\"");
}

static void test_envelope_rejects_short_board_id_bytes(void) {
    /* Reuse FEB_VEC_PAIR_INIT_RECORD as a decode sanity check on a truncated buffer: the
       decoder must fail closed (not read past `in_len`), matching cbor_codec.c's
       truncation handling. */
    feb_pairing_envelope_t env;
    feb_cbor_status_t status =
        feb_cbor_decode_pairing_envelope(FEB_VEC_PAIR_INIT_RECORD, FEB_VEC_PAIR_INIT_RECORD_LEN - 1, &env);
    CHECK(status != FEB_CBOR_OK, "envelope: truncated golden record is rejected, not accepted");
}

static void test_error_code_strings(void) {
    CHECK(
        strcmp(feb_pairing_error_code_str(FEB_PAIRING_ERR_DISABLED), "pairing_disabled") == 0,
        "error code string: FEB_PAIRING_ERR_DISABLED");
    CHECK(
        strcmp(feb_pairing_error_code_str(FEB_PAIRING_ERR_EXPIRED), "pairing_expired") == 0,
        "error code string: FEB_PAIRING_ERR_EXPIRED");
    CHECK(
        strcmp(feb_pairing_error_code_str(FEB_PAIRING_ERR_FAILED), "pairing_failed") == 0,
        "error code string: FEB_PAIRING_ERR_FAILED");
}

int main(void) {
    test_x25519_rfc7748_tc();
    test_x25519_diffie_hellman();
    test_x25519_zero_point_and_all_zero_check();

    test_sha256();
    test_hmac_sha256();
    test_hkdf_sha256();
    test_consttime_equal();

    test_golden_pairing_vector();

    test_pair_init_codec();
    test_pair_reply_codec();
    test_pair_confirm_codec();
    test_pair_complete_codec();
    test_pair_error_codec();
    test_envelope_rejects_short_board_id_bytes();
    test_error_code_strings();

    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}

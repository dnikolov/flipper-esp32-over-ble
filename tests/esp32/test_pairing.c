/* Host-native test driver for esp32/main/pairing_crypto.c + esp32/main/pairing.c
   (plus their esp32/main/cbor_codec.c dependency), compiled directly (not copies)
   against the shared vectors in tests/vectors/vectors.h. See docs/PLAN.md step 5
   and tests/esp32/test_framing_cbor.c for the established pattern this mirrors. */
#include <stdio.h>
#include <string.h>

#include "cbor_codec.h"
#include "pairing.h"
#include "pairing_crypto.h"
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

static void test_x25519_rfc7748_cases(void)
{
    uint8_t out[FEB_X25519_KEY_LEN];

    feb_x25519(out, FEB_VEC_X25519_TC1_SCALAR, FEB_VEC_X25519_TC1_U);
    check(bytes_eq(out, sizeof(out), FEB_VEC_X25519_TC1_OUTPUT, FEB_VEC_X25519_TC1_OUTPUT_LEN),
          "X25519 RFC 7748 5.2 test case 1");

    feb_x25519(out, FEB_VEC_X25519_TC2_SCALAR, FEB_VEC_X25519_TC2_U);
    check(bytes_eq(out, sizeof(out), FEB_VEC_X25519_TC2_OUTPUT, FEB_VEC_X25519_TC2_OUTPUT_LEN),
          "X25519 RFC 7748 5.2 test case 2");
}

static void test_x25519_diffie_hellman(void)
{
    uint8_t alice_pub[FEB_X25519_KEY_LEN];
    uint8_t bob_pub[FEB_X25519_KEY_LEN];
    uint8_t shared_from_alice[FEB_X25519_KEY_LEN];
    uint8_t shared_from_bob[FEB_X25519_KEY_LEN];

    feb_x25519_base(alice_pub, FEB_VEC_X25519_ALICE_PRIVATE);
    check(bytes_eq(alice_pub, sizeof(alice_pub), FEB_VEC_X25519_ALICE_PUBLIC, FEB_VEC_X25519_ALICE_PUBLIC_LEN),
          "X25519 RFC 7748 6.1: Alice public key from base point");

    feb_x25519_base(bob_pub, FEB_VEC_X25519_BOB_PRIVATE);
    check(bytes_eq(bob_pub, sizeof(bob_pub), FEB_VEC_X25519_BOB_PUBLIC, FEB_VEC_X25519_BOB_PUBLIC_LEN),
          "X25519 RFC 7748 6.1: Bob public key from base point");

    feb_x25519(shared_from_alice, FEB_VEC_X25519_ALICE_PRIVATE, FEB_VEC_X25519_BOB_PUBLIC);
    check(bytes_eq(shared_from_alice, sizeof(shared_from_alice), FEB_VEC_X25519_SHARED, FEB_VEC_X25519_SHARED_LEN),
          "X25519 RFC 7748 6.1: Alice-side shared secret");

    feb_x25519(shared_from_bob, FEB_VEC_X25519_BOB_PRIVATE, FEB_VEC_X25519_ALICE_PUBLIC);
    check(bytes_eq(shared_from_bob, sizeof(shared_from_bob), FEB_VEC_X25519_SHARED, FEB_VEC_X25519_SHARED_LEN),
          "X25519 RFC 7748 6.1: Bob-side shared secret");
}

static void test_x25519_zero_and_is_all_zero(void)
{
    uint8_t out[FEB_X25519_KEY_LEN];

    feb_x25519(out, FEB_VEC_X25519_TC1_SCALAR, FEB_VEC_X25519_ZERO_U);
    check(bytes_eq(out, sizeof(out), FEB_VEC_X25519_ZERO_OUTPUT, FEB_VEC_X25519_ZERO_OUTPUT_LEN),
          "X25519 with all-zero u-coordinate yields all-zero output");

    check(feb_is_all_zero(FEB_VEC_X25519_ZERO_OUTPUT, FEB_VEC_X25519_ZERO_OUTPUT_LEN) == 1,
          "feb_is_all_zero flags the all-zero X25519 output");
    check(feb_is_all_zero(FEB_VEC_X25519_SHARED, FEB_VEC_X25519_SHARED_LEN) == 0,
          "feb_is_all_zero does not flag a real shared secret");
    check(feb_is_all_zero(FEB_VEC_PAIR_K_SHARED, FEB_VEC_PAIR_K_SHARED_LEN) == 0,
          "feb_is_all_zero does not flag the golden vector's K_shared");
}

static void test_sha256(void)
{
    uint8_t out[FEB_SHA256_LEN];

    feb_sha256(FEB_VEC_SHA256_INPUT, FEB_VEC_SHA256_INPUT_LEN, out);
    check(bytes_eq(out, sizeof(out), FEB_VEC_SHA256_DIGEST, FEB_VEC_SHA256_DIGEST_LEN),
          "SHA-256 FIPS 180-4 short message example");
}

static void test_hmac_sha256(void)
{
    uint8_t out[FEB_HMAC_SHA256_LEN];

    feb_hmac_sha256(FEB_VEC_HMAC_KEY, FEB_VEC_HMAC_KEY_LEN, FEB_VEC_HMAC_DATA, FEB_VEC_HMAC_DATA_LEN, out);
    check(bytes_eq(out, sizeof(out), FEB_VEC_HMAC_MAC, FEB_VEC_HMAC_MAC_LEN),
          "HMAC-SHA-256 RFC 4231 test case 1");
}

static void test_hkdf_sha256(void)
{
    uint8_t out[FEB_VEC_HKDF_OKM_L32_LEN];
    int rc;

    rc = feb_hkdf_sha256(FEB_VEC_HKDF_SALT, FEB_VEC_HKDF_SALT_LEN, FEB_VEC_HKDF_IKM, FEB_VEC_HKDF_IKM_LEN,
                          FEB_VEC_HKDF_INFO, FEB_VEC_HKDF_INFO_LEN, out, sizeof(out));
    check(rc == 0 && bytes_eq(out, sizeof(out), FEB_VEC_HKDF_OKM_L32, FEB_VEC_HKDF_OKM_L32_LEN),
          "HKDF-SHA-256 RFC 5869 test case 1 inputs, L=32 output");
}

static void test_consttime_equal(void)
{
    static const uint8_t a[4] = {1, 2, 3, 4};
    static const uint8_t b[4] = {1, 2, 3, 4};
    static const uint8_t c[4] = {1, 2, 3, 5};

    check(feb_consttime_equal(a, b, sizeof(a)) == 1, "feb_consttime_equal: equal buffers");
    check(feb_consttime_equal(a, c, sizeof(a)) == 0, "feb_consttime_equal: differing buffers");
    check(feb_consttime_equal(a, b, 0) == 0, "feb_consttime_equal: zero length is not equal");
}

static uint8_t g_transcript[FEB_PAIRING_MAX_TRANSCRIPT_LEN];
static size_t g_transcript_len;

static void test_golden_transcript(void)
{
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

    g_transcript_len = feb_pairing_encode_transcript(g_transcript, sizeof(g_transcript), &t);
    check(g_transcript_len > 0 &&
              bytes_eq(g_transcript, g_transcript_len, FEB_VEC_PAIR_TRANSCRIPT, FEB_VEC_PAIR_TRANSCRIPT_LEN),
          "feb_pairing_encode_transcript matches FEB_VEC_PAIR_TRANSCRIPT");
}

static uint8_t g_k_shared[FEB_PAIRING_KSHARED_LEN];
static uint8_t g_k_confirm[FEB_PAIRING_KCONFIRM_LEN];
static uint8_t g_pairing_secret[FEB_PAIRING_SECRET_LEN];

static void test_golden_kshared(void)
{
    uint8_t from_esp32[FEB_PAIRING_KSHARED_LEN];
    uint8_t from_flipper[FEB_PAIRING_KSHARED_LEN];

    feb_x25519(from_esp32, FEB_VEC_PAIR_ESP32_PRIVATE, FEB_VEC_PAIR_FLIPPER_PUBLIC);
    feb_x25519(from_flipper, FEB_VEC_PAIR_FLIPPER_PRIVATE, FEB_VEC_PAIR_ESP32_PUBLIC);

    check(bytes_eq(from_esp32, sizeof(from_esp32), FEB_VEC_PAIR_K_SHARED, FEB_VEC_PAIR_K_SHARED_LEN),
          "golden vector: K_shared computed from ESP32 private key matches");
    check(bytes_eq(from_flipper, sizeof(from_flipper), FEB_VEC_PAIR_K_SHARED, FEB_VEC_PAIR_K_SHARED_LEN),
          "golden vector: K_shared computed from Flipper private key matches");

    memcpy(g_k_shared, FEB_VEC_PAIR_K_SHARED, FEB_PAIRING_KSHARED_LEN);
}

static void test_golden_kconfirm_and_secret(void)
{
    feb_pairing_derive_kconfirm(g_k_shared, FEB_VEC_PAIR_EPOCH, g_k_confirm);
    check(bytes_eq(g_k_confirm, sizeof(g_k_confirm), FEB_VEC_PAIR_K_CONFIRM, FEB_VEC_PAIR_K_CONFIRM_LEN),
          "feb_pairing_derive_kconfirm matches FEB_VEC_PAIR_K_CONFIRM");

    feb_pairing_derive_secret(g_k_shared, FEB_VEC_PAIR_EPOCH, FEB_VEC_PAIR_CLIENT_NONCE, FEB_VEC_PAIR_DEVICE_NONCE,
                               FEB_VEC_PAIR_BOARD_ID, FEB_VEC_PAIR_BOARD_ID_LEN, g_pairing_secret);
    check(bytes_eq(g_pairing_secret, sizeof(g_pairing_secret), FEB_VEC_PAIR_SECRET, FEB_VEC_PAIR_SECRET_LEN),
          "feb_pairing_derive_secret matches FEB_VEC_PAIR_SECRET");
}

static void test_golden_confirmation_tags(void)
{
    uint8_t flipper_confirm[FEB_PAIRING_REPLY_CONFIRM_LEN];
    uint8_t esp32_confirm[FEB_PAIRING_REPLY_CONFIRM_LEN];
    uint8_t complete_tag[FEB_PAIRING_COMPLETE_TAG_LEN];

    feb_pairing_flipper_confirm(g_k_confirm, g_transcript, g_transcript_len, flipper_confirm);
    check(bytes_eq(flipper_confirm, sizeof(flipper_confirm), FEB_VEC_PAIR_FLIPPER_CONFIRM, FEB_VEC_PAIR_FLIPPER_CONFIRM_LEN),
          "feb_pairing_flipper_confirm matches FEB_VEC_PAIR_FLIPPER_CONFIRM");

    feb_pairing_esp32_confirm(g_k_confirm, g_transcript, g_transcript_len, esp32_confirm);
    check(bytes_eq(esp32_confirm, sizeof(esp32_confirm), FEB_VEC_PAIR_ESP32_CONFIRM, FEB_VEC_PAIR_ESP32_CONFIRM_LEN),
          "feb_pairing_esp32_confirm matches FEB_VEC_PAIR_ESP32_CONFIRM");

    feb_pairing_complete_tag(g_k_confirm, g_transcript, g_transcript_len, complete_tag);
    check(bytes_eq(complete_tag, sizeof(complete_tag), FEB_VEC_PAIR_COMPLETE_TAG, FEB_VEC_PAIR_COMPLETE_TAG_LEN),
          "feb_pairing_complete_tag matches FEB_VEC_PAIR_COMPLETE_TAG");
}

static void test_pair_init_payload_roundtrip(void)
{
    feb_pair_init_payload_t decoded;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    st = feb_cbor_decode_pair_init_payload(FEB_VEC_PAIR_INIT_PAYLOAD, FEB_VEC_PAIR_INIT_PAYLOAD_LEN, &decoded);
    ok = (st == FEB_CBOR_OK);
    ok = ok && memcmp(decoded.pairing_epoch, FEB_VEC_PAIR_EPOCH, FEB_PAIRING_EPOCH_LEN) == 0;
    ok = ok && memcmp(decoded.device_nonce, FEB_VEC_PAIR_DEVICE_NONCE, FEB_PAIRING_NONCE_LEN) == 0;
    ok = ok && memcmp(decoded.esp32_public_key, FEB_VEC_PAIR_ESP32_PUBLIC, FEB_PAIRING_PUBKEY_LEN) == 0;
    check(ok, "pair_init payload decodes to golden vector fields");

    encoded_len = feb_cbor_encode_pair_init_payload(encode_buf, sizeof(encode_buf), &decoded);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_PAIR_INIT_PAYLOAD, FEB_VEC_PAIR_INIT_PAYLOAD_LEN),
          "pair_init payload encode round-trip byte-identical");
}

static void test_pair_reply_payload_roundtrip(void)
{
    feb_pair_reply_payload_t decoded;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    st = feb_cbor_decode_pair_reply_payload(FEB_VEC_PAIR_REPLY_PAYLOAD, FEB_VEC_PAIR_REPLY_PAYLOAD_LEN, &decoded);
    ok = (st == FEB_CBOR_OK);
    ok = ok && memcmp(decoded.client_nonce, FEB_VEC_PAIR_CLIENT_NONCE, FEB_PAIRING_NONCE_LEN) == 0;
    ok = ok && memcmp(decoded.flipper_public_key, FEB_VEC_PAIR_FLIPPER_PUBLIC, FEB_PAIRING_PUBKEY_LEN) == 0;
    ok = ok && memcmp(decoded.confirmation, FEB_VEC_PAIR_FLIPPER_CONFIRM, FEB_PAIRING_REPLY_CONFIRM_LEN) == 0;
    check(ok, "pair_reply payload decodes to golden vector fields");

    encoded_len = feb_cbor_encode_pair_reply_payload(encode_buf, sizeof(encode_buf), &decoded);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_PAIR_REPLY_PAYLOAD, FEB_VEC_PAIR_REPLY_PAYLOAD_LEN),
          "pair_reply payload encode round-trip byte-identical");
}

static void test_pair_confirm_payload_roundtrip(void)
{
    feb_pair_confirm_payload_t decoded;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    st = feb_cbor_decode_pair_confirm_payload(FEB_VEC_PAIR_CONFIRM_PAYLOAD, FEB_VEC_PAIR_CONFIRM_PAYLOAD_LEN, &decoded);
    ok = (st == FEB_CBOR_OK);
    ok = ok && memcmp(decoded.confirmation, FEB_VEC_PAIR_ESP32_CONFIRM, FEB_PAIRING_REPLY_CONFIRM_LEN) == 0;
    check(ok, "pair_confirm payload decodes to golden vector fields");

    encoded_len = feb_cbor_encode_pair_confirm_payload(encode_buf, sizeof(encode_buf), &decoded);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_PAIR_CONFIRM_PAYLOAD, FEB_VEC_PAIR_CONFIRM_PAYLOAD_LEN),
          "pair_confirm payload encode round-trip byte-identical");
}

static void test_pair_complete_payload_roundtrip(void)
{
    feb_pair_complete_payload_t decoded;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    st = feb_cbor_decode_pair_complete_payload(FEB_VEC_PAIR_COMPLETE_PAYLOAD, FEB_VEC_PAIR_COMPLETE_PAYLOAD_LEN, &decoded);
    ok = (st == FEB_CBOR_OK);
    ok = ok && memcmp(decoded.confirmation, FEB_VEC_PAIR_COMPLETE_TAG, FEB_PAIRING_COMPLETE_TAG_LEN) == 0;
    check(ok, "pair_complete payload decodes to golden vector fields");

    encoded_len = feb_cbor_encode_pair_complete_payload(encode_buf, sizeof(encode_buf), &decoded);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_PAIR_COMPLETE_PAYLOAD, FEB_VEC_PAIR_COMPLETE_PAYLOAD_LEN),
          "pair_complete payload encode round-trip byte-identical");
}

static void test_envelope_roundtrip(const uint8_t *record, size_t record_len,
                                     const char *expected_type,
                                     const uint8_t *expected_payload, size_t expected_payload_len,
                                     const char *name)
{
    feb_pairing_envelope_t env;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD + 64];
    size_t encoded_len;
    int ok;
    char check_name[128];

    st = feb_cbor_decode_pairing_envelope(record, record_len, &env);
    ok = (st == FEB_CBOR_OK);
    ok = ok && env.version == 2;
    ok = ok && env.type_len == strlen(expected_type) && memcmp(env.type, expected_type, env.type_len) == 0;
    ok = ok && env.board_id_len == FEB_VEC_PAIR_BOARD_ID_LEN &&
         memcmp(env.board_id, FEB_VEC_PAIR_BOARD_ID, FEB_VEC_PAIR_BOARD_ID_LEN) == 0;
    ok = ok && bytes_eq(env.payload_span, env.payload_span_len, expected_payload, expected_payload_len);
    snprintf(check_name, sizeof(check_name), "%s: decode matches golden vector fields", name);
    check(ok, check_name);

    encoded_len = feb_cbor_encode_pairing_envelope(encode_buf, sizeof(encode_buf), &env);
    snprintf(check_name, sizeof(check_name), "%s: encode round-trip byte-identical", name);
    check(bytes_eq(encode_buf, encoded_len, record, record_len), check_name);
}

static void test_error_payload_and_envelope_roundtrip(void)
{
    feb_pairing_envelope_t env;
    feb_error_payload_t payload;
    feb_cbor_status_t st;
    int ok;
    uint8_t payload_buf[FEB_CBOR_MAX_PAYLOAD];
    uint8_t record_buf[FEB_CBOR_MAX_PAYLOAD + 64];
    size_t payload_len;
    size_t record_len;

    st = feb_cbor_decode_pairing_envelope(FEB_VEC_PAIR_ERROR_RECORD, FEB_VEC_PAIR_ERROR_RECORD_LEN, &env);
    ok = (st == FEB_CBOR_OK);
    ok = ok && env.type_len == 5 && memcmp(env.type, "error", 5) == 0;
    check(ok, "pairing-phase error envelope decodes");

    st = feb_cbor_decode_error_payload(env.payload_span, env.payload_span_len, &payload);
    ok = (st == FEB_CBOR_OK);
    ok = ok && payload.code_len == 14 && memcmp(payload.code, "pairing_failed", 14) == 0;
    ok = ok && payload.has_message && payload.message_len == 16 &&
         memcmp(payload.message, "bad confirmation", 16) == 0;
    ok = ok && payload.has_request_id && payload.request_id == 0;
    check(ok, "pairing-phase error payload decodes to golden vector fields");

    payload_len = feb_cbor_encode_error_payload(payload_buf, sizeof(payload_buf), &payload);
    check(bytes_eq(payload_buf, payload_len, FEB_VEC_PAIR_ERROR_PAYLOAD, FEB_VEC_PAIR_ERROR_PAYLOAD_LEN),
          "pairing-phase error payload encode round-trip byte-identical");

    env.payload_span = payload_buf;
    env.payload_span_len = payload_len;
    record_len = feb_cbor_encode_pairing_envelope(record_buf, sizeof(record_buf), &env);
    check(bytes_eq(record_buf, record_len, FEB_VEC_PAIR_ERROR_RECORD, FEB_VEC_PAIR_ERROR_RECORD_LEN),
          "pairing-phase error envelope encode round-trip byte-identical");
}

int main(void)
{
    test_x25519_rfc7748_cases();
    test_x25519_diffie_hellman();
    test_x25519_zero_and_is_all_zero();
    test_sha256();
    test_hmac_sha256();
    test_hkdf_sha256();
    test_consttime_equal();

    test_golden_transcript();
    test_golden_kshared();
    test_golden_kconfirm_and_secret();
    test_golden_confirmation_tags();

    test_pair_init_payload_roundtrip();
    test_pair_reply_payload_roundtrip();
    test_pair_confirm_payload_roundtrip();
    test_pair_complete_payload_roundtrip();

    test_envelope_roundtrip(FEB_VEC_PAIR_INIT_RECORD, FEB_VEC_PAIR_INIT_RECORD_LEN, FEB_PAIR_INIT_TYPE,
                             FEB_VEC_PAIR_INIT_PAYLOAD, FEB_VEC_PAIR_INIT_PAYLOAD_LEN, "pair_init envelope");
    test_envelope_roundtrip(FEB_VEC_PAIR_REPLY_RECORD, FEB_VEC_PAIR_REPLY_RECORD_LEN, FEB_PAIR_REPLY_TYPE,
                             FEB_VEC_PAIR_REPLY_PAYLOAD, FEB_VEC_PAIR_REPLY_PAYLOAD_LEN, "pair_reply envelope");
    test_envelope_roundtrip(FEB_VEC_PAIR_CONFIRM_RECORD, FEB_VEC_PAIR_CONFIRM_RECORD_LEN, FEB_PAIR_CONFIRM_TYPE,
                             FEB_VEC_PAIR_CONFIRM_PAYLOAD, FEB_VEC_PAIR_CONFIRM_PAYLOAD_LEN, "pair_confirm envelope");
    test_envelope_roundtrip(FEB_VEC_PAIR_COMPLETE_RECORD, FEB_VEC_PAIR_COMPLETE_RECORD_LEN, FEB_PAIR_COMPLETE_TYPE,
                             FEB_VEC_PAIR_COMPLETE_PAYLOAD, FEB_VEC_PAIR_COMPLETE_PAYLOAD_LEN, "pair_complete envelope");

    test_error_payload_and_envelope_roundtrip();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

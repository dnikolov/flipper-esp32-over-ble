/* Host-native test driver for esp32/main/session_crypto.c + esp32/main/session.c
   (plus their split cbor_*.c codec dependency (docs/OPTIMIZATION.md item 1) +
   esp32/main/pairing_crypto.c dependency), compiled directly (not copies) against the
   shared vectors in tests/vectors/vectors.h.
   See docs/PLAN.md step 6 and tests/esp32/test_pairing.c for the established pattern
   this mirrors. */
#include <stdio.h>
#include <string.h>

#include "cbor_codec.h"
#include "framing.h"
#include "pairing.h"
#include "pairing_crypto.h"
#include "session.h"
#include "session_crypto.h"
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

static void test_gcm_kat_encrypt(void)
{
    uint8_t ciphertext[FEB_VEC_GCM_PLAINTEXT_LEN];
    uint8_t tag[FEB_SESSION_GCM_TAG_LEN];

    feb_gcm_encrypt(FEB_VEC_GCM_KEY, FEB_VEC_GCM_IV, FEB_VEC_GCM_AAD, FEB_VEC_GCM_AAD_LEN,
                    FEB_VEC_GCM_PLAINTEXT, FEB_VEC_GCM_PLAINTEXT_LEN, ciphertext, tag);
    check(bytes_eq(ciphertext, sizeof(ciphertext), FEB_VEC_GCM_CIPHERTEXT, FEB_VEC_GCM_CIPHERTEXT_LEN),
          "AES-256-GCM KAT (GCM spec Test Case 16): ciphertext matches");
    check(bytes_eq(tag, sizeof(tag), FEB_VEC_GCM_TAG, FEB_VEC_GCM_TAG_LEN),
          "AES-256-GCM KAT (GCM spec Test Case 16): tag matches");
}

static void test_gcm_kat_decrypt(void)
{
    uint8_t plaintext[FEB_VEC_GCM_CIPHERTEXT_LEN];
    int ok;

    ok = feb_gcm_decrypt(FEB_VEC_GCM_KEY, FEB_VEC_GCM_IV, FEB_VEC_GCM_AAD, FEB_VEC_GCM_AAD_LEN,
                         FEB_VEC_GCM_CIPHERTEXT, FEB_VEC_GCM_CIPHERTEXT_LEN, FEB_VEC_GCM_TAG, plaintext);
    check(ok == 1 && bytes_eq(plaintext, sizeof(plaintext), FEB_VEC_GCM_PLAINTEXT, FEB_VEC_GCM_PLAINTEXT_LEN),
          "AES-256-GCM KAT: decrypt recovers plaintext and verifies tag");
}

static void test_gcm_kat_tamper_rejected(void)
{
    uint8_t tampered_ciphertext[FEB_VEC_GCM_CIPHERTEXT_LEN];
    uint8_t tampered_tag[FEB_VEC_GCM_TAG_LEN];
    uint8_t plaintext[FEB_VEC_GCM_CIPHERTEXT_LEN];
    int ok;

    memcpy(tampered_ciphertext, FEB_VEC_GCM_CIPHERTEXT, sizeof(tampered_ciphertext));
    tampered_ciphertext[0] ^= 0x01u;
    ok = feb_gcm_decrypt(FEB_VEC_GCM_KEY, FEB_VEC_GCM_IV, FEB_VEC_GCM_AAD, FEB_VEC_GCM_AAD_LEN,
                         tampered_ciphertext, sizeof(tampered_ciphertext), FEB_VEC_GCM_TAG, plaintext);
    check(ok == 0, "AES-256-GCM: tampered ciphertext rejected");

    memcpy(tampered_tag, FEB_VEC_GCM_TAG, sizeof(tampered_tag));
    tampered_tag[0] ^= 0x01u;
    ok = feb_gcm_decrypt(FEB_VEC_GCM_KEY, FEB_VEC_GCM_IV, FEB_VEC_GCM_AAD, FEB_VEC_GCM_AAD_LEN,
                         FEB_VEC_GCM_CIPHERTEXT, FEB_VEC_GCM_CIPHERTEXT_LEN, tampered_tag, plaintext);
    check(ok == 0, "AES-256-GCM: tampered tag rejected");
}

static uint8_t g_transcript[FEB_SESSION_MAX_TRANSCRIPT_LEN];
static size_t g_transcript_len;

static void test_golden_transcript(void)
{
    feb_session_transcript_t s;

    memset(&s, 0, sizeof(s));
    s.version = 2;
    s.board_id = FEB_VEC_SESS_BOARD_ID;
    s.board_id_len = FEB_VEC_SESS_BOARD_ID_LEN;
    memcpy(s.session_id, FEB_VEC_SESS_SESSION_ID, FEB_SESSION_ID_LEN);
    memcpy(s.client_nonce, FEB_VEC_SESS_CLIENT_NONCE, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(s.device_nonce, FEB_VEC_SESS_DEVICE_NONCE, FEB_SESSION_NONCE_FIELD_LEN);

    g_transcript_len = feb_session_encode_transcript(g_transcript, sizeof(g_transcript), &s);
    check(g_transcript_len > 0 &&
              bytes_eq(g_transcript, g_transcript_len, FEB_VEC_SESS_TRANSCRIPT, FEB_VEC_SESS_TRANSCRIPT_LEN),
          "feb_session_encode_transcript matches FEB_VEC_SESS_TRANSCRIPT");
}

static void test_golden_proofs(void)
{
    uint8_t flipper_proof[FEB_SESSION_PROOF_LEN];
    uint8_t esp32_proof[FEB_SESSION_PROOF_LEN];

    feb_session_flipper_proof(FEB_VEC_PAIR_SECRET, g_transcript, g_transcript_len, flipper_proof);
    check(bytes_eq(flipper_proof, sizeof(flipper_proof), FEB_VEC_SESS_FLIPPER_PROOF, FEB_VEC_SESS_FLIPPER_PROOF_LEN),
          "feb_session_flipper_proof matches FEB_VEC_SESS_FLIPPER_PROOF");

    feb_session_esp32_proof(FEB_VEC_PAIR_SECRET, g_transcript, g_transcript_len, esp32_proof);
    check(bytes_eq(esp32_proof, sizeof(esp32_proof), FEB_VEC_SESS_ESP32_PROOF, FEB_VEC_SESS_ESP32_PROOF_LEN),
          "feb_session_esp32_proof matches FEB_VEC_SESS_ESP32_PROOF");
}

static void test_golden_session_key(void)
{
    uint8_t key[FEB_SESSION_KEY_LEN];

    feb_session_derive_key(FEB_VEC_PAIR_SECRET, FEB_VEC_SESS_CLIENT_NONCE, FEB_VEC_SESS_DEVICE_NONCE,
                           FEB_VEC_SESS_BOARD_ID, FEB_VEC_SESS_BOARD_ID_LEN, FEB_VEC_SESS_SESSION_ID, key);
    check(bytes_eq(key, sizeof(key), FEB_VEC_SESS_KEY, FEB_VEC_SESS_KEY_LEN),
          "feb_session_derive_key matches FEB_VEC_SESS_KEY");
}

static void test_hello_payload_roundtrip(void)
{
    feb_hello_payload_t decoded;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    st = feb_cbor_decode_hello_payload(FEB_VEC_SESS_HELLO_PAYLOAD, FEB_VEC_SESS_HELLO_PAYLOAD_LEN, &decoded);
    ok = (st == FEB_CBOR_OK);
    ok = ok && memcmp(decoded.client_nonce, FEB_VEC_SESS_CLIENT_NONCE, FEB_SESSION_NONCE_FIELD_LEN) == 0;
    check(ok, "hello payload decodes to golden vector fields");

    encoded_len = feb_cbor_encode_hello_payload(encode_buf, sizeof(encode_buf), &decoded);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_SESS_HELLO_PAYLOAD, FEB_VEC_SESS_HELLO_PAYLOAD_LEN),
          "hello payload encode round-trip byte-identical");
}

static void test_hello_ack_payload_roundtrip(void)
{
    feb_hello_ack_payload_t decoded;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    st = feb_cbor_decode_hello_ack_payload(FEB_VEC_SESS_HELLO_ACK_PAYLOAD, FEB_VEC_SESS_HELLO_ACK_PAYLOAD_LEN, &decoded);
    ok = (st == FEB_CBOR_OK);
    ok = ok && memcmp(decoded.device_nonce, FEB_VEC_SESS_DEVICE_NONCE, FEB_SESSION_NONCE_FIELD_LEN) == 0;
    ok = ok && memcmp(decoded.proof, FEB_VEC_SESS_FLIPPER_PROOF, FEB_SESSION_PROOF_LEN) == 0;
    check(ok, "hello_ack payload decodes to golden vector fields");

    encoded_len = feb_cbor_encode_hello_ack_payload(encode_buf, sizeof(encode_buf), &decoded);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_SESS_HELLO_ACK_PAYLOAD, FEB_VEC_SESS_HELLO_ACK_PAYLOAD_LEN),
          "hello_ack payload encode round-trip byte-identical");
}

static void test_client_auth_payload_roundtrip(void)
{
    feb_client_auth_payload_t decoded;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD];
    size_t encoded_len;
    int ok;

    st = feb_cbor_decode_client_auth_payload(FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD, FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD_LEN, &decoded);
    ok = (st == FEB_CBOR_OK);
    ok = ok && memcmp(decoded.proof, FEB_VEC_SESS_ESP32_PROOF, FEB_SESSION_PROOF_LEN) == 0;
    check(ok, "client_auth payload decodes to golden vector fields");

    encoded_len = feb_cbor_encode_client_auth_payload(encode_buf, sizeof(encode_buf), &decoded);
    check(bytes_eq(encode_buf, encoded_len, FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD, FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD_LEN),
          "client_auth payload encode round-trip byte-identical");
}

static void test_unencrypted_record_roundtrip(const uint8_t *record, size_t record_len,
                                              const char *expected_type,
                                              const uint8_t *expected_payload, size_t expected_payload_len,
                                              const char *name)
{
    feb_unencrypted_record_t env;
    feb_cbor_status_t st;
    uint8_t encode_buf[FEB_CBOR_MAX_PAYLOAD + 64];
    size_t encoded_len;
    int ok;
    char check_name[128];

    st = feb_cbor_decode_unencrypted(record, record_len, &env);
    ok = (st == FEB_CBOR_OK);
    ok = ok && env.version == 2;
    ok = ok && env.type_len == strlen(expected_type) && memcmp(env.type, expected_type, env.type_len) == 0;
    ok = ok && memcmp(env.session_id, FEB_VEC_SESS_SESSION_ID, FEB_SESSION_ID_LEN) == 0;
    ok = ok && env.board_id_len == FEB_VEC_SESS_BOARD_ID_LEN &&
         memcmp(env.board_id, FEB_VEC_SESS_BOARD_ID, FEB_VEC_SESS_BOARD_ID_LEN) == 0;
    ok = ok && bytes_eq(env.payload_span, env.payload_span_len, expected_payload, expected_payload_len);
    snprintf(check_name, sizeof(check_name), "%s: decode matches golden vector fields", name);
    check(ok, check_name);

    encoded_len = feb_cbor_encode_unencrypted(encode_buf, sizeof(encode_buf), &env);
    snprintf(check_name, sizeof(check_name), "%s: encode round-trip byte-identical", name);
    check(bytes_eq(encode_buf, encoded_len, record, record_len), check_name);
}

static void test_aad_and_nonce(void)
{
    feb_session_aad_t aad_fields;
    uint8_t aad_buf[FEB_SESSION_MAX_AAD_LEN];
    size_t aad_len;
    uint8_t nonce[FEB_SESSION_NONCE_LEN];

    memset(&aad_fields, 0, sizeof(aad_fields));
    aad_fields.version = 2;
    aad_fields.type = "error";
    aad_fields.type_len = strlen("error");
    memcpy(aad_fields.session_id, FEB_VEC_SESS_SESSION_ID, FEB_SESSION_ID_LEN);
    aad_fields.sequence = 1;
    aad_fields.board_id = FEB_VEC_SESS_BOARD_ID;
    aad_fields.board_id_len = FEB_VEC_SESS_BOARD_ID_LEN;

    aad_len = feb_session_encode_aad(aad_buf, sizeof(aad_buf), &aad_fields);
    check(aad_len > 0 && bytes_eq(aad_buf, aad_len, FEB_VEC_SESS_PROT1_AAD, FEB_VEC_SESS_PROT1_AAD_LEN),
          "feb_session_encode_aad matches FEB_VEC_SESS_PROT1_AAD");

    feb_session_build_nonce(FEB_VEC_SESS_SESSION_ID, FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, 1, nonce);
    check(bytes_eq(nonce, sizeof(nonce), FEB_VEC_SESS_PROT1_NONCE, FEB_VEC_SESS_PROT1_NONCE_LEN),
          "feb_session_build_nonce matches FEB_VEC_SESS_PROT1_NONCE");
}

static void test_encrypt_record_matches_golden(void)
{
    uint8_t ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];
    uint8_t out[FEB_MAX_RECORD_SIZE];
    size_t out_len;

    out_len = feb_session_encrypt_record(
        FEB_VEC_SESS_KEY, 2, "error", strlen("error"),
        FEB_VEC_SESS_SESSION_ID, FEB_VEC_SESS_BOARD_ID, FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, 1,
        FEB_VEC_SESS_PROT1_PAYLOAD, FEB_VEC_SESS_PROT1_PAYLOAD_LEN,
        ciphertext_scratch, sizeof(ciphertext_scratch),
        out, sizeof(out));

    check(out_len > 0 && bytes_eq(out, out_len, FEB_VEC_SESS_PROT1_RECORD, FEB_VEC_SESS_PROT1_RECORD_LEN),
          "feb_session_encrypt_record(seq=1) matches FEB_VEC_SESS_PROT1_RECORD byte-for-byte");
}

static void test_decrypt_record_prot1_and_prot2(void)
{
    uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
    feb_session_decrypted_record_t record;
    feb_cbor_status_t st;

    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_SESS_PROT1_RECORD, FEB_VEC_SESS_PROT1_RECORD_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_OK && record.sequence == 1 &&
              bytes_eq(record.plaintext, record.plaintext_len, FEB_VEC_SESS_PROT1_PAYLOAD, FEB_VEC_SESS_PROT1_PAYLOAD_LEN),
          "feb_session_decrypt_record recovers FEB_VEC_SESS_PROT1_PAYLOAD at sequence 1");

    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_SESS_PROT2_RECORD, FEB_VEC_SESS_PROT2_RECORD_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_OK && record.sequence == 2 &&
              bytes_eq(record.plaintext, record.plaintext_len, FEB_VEC_SESS_PROT2_PAYLOAD, FEB_VEC_SESS_PROT2_PAYLOAD_LEN),
          "feb_session_decrypt_record recovers FEB_VEC_SESS_PROT2_PAYLOAD at sequence 2");
}

static void test_decrypt_record_tamper_rejected(void)
{
    uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
    feb_session_decrypted_record_t record;
    feb_cbor_status_t st;

    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_SESS_PROT1_RECORD_BAD_CIPHERTEXT,
                                    FEB_VEC_SESS_PROT1_RECORD_BAD_CIPHERTEXT_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_ERR_AUTH_FAILED,
          "feb_session_decrypt_record rejects FEB_VEC_SESS_PROT1_RECORD_BAD_CIPHERTEXT with FEB_CBOR_ERR_AUTH_FAILED");

    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_SESS_PROT1_RECORD_BAD_AAD,
                                    FEB_VEC_SESS_PROT1_RECORD_BAD_AAD_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_ERR_AUTH_FAILED,
          "feb_session_decrypt_record rejects FEB_VEC_SESS_PROT1_RECORD_BAD_AAD with FEB_CBOR_ERR_AUTH_FAILED");
}

/* docs/PLAN.md "Wi-Fi scan capability" step: end-to-end protected-record wraps of the
   wifi_scan command/status payloads, continuing the golden session's per-direction sequence
   counters after FEB_VEC_SESS_PROT1/PROT2 (sequence 3/4). Placed here rather than
   tests/esp32/test_framing_cbor.c because these three vectors require AES-256-GCM
   encrypt/decrypt (session.c/session_crypto.c + mbedtls), which build.ps1 for
   test_framing_cbor.c does not link -- test_framing_cbor.c instead covers every
   wifi_scan codec vector that needs only the split cbor_*.c codec (the
   ap-result/result/command/status payload shapes themselves). See the esp32-developer
   report for this step. */
static void test_wifi_scan_command_record(void)
{
    uint8_t ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];
    uint8_t out[FEB_MAX_RECORD_SIZE];
    size_t out_len;
    uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
    feb_session_decrypted_record_t record;
    feb_cbor_status_t st;

    out_len = feb_session_encrypt_record(
        FEB_VEC_SESS_KEY, 2, "command", strlen("command"),
        FEB_VEC_SESS_SESSION_ID, FEB_VEC_SESS_BOARD_ID, FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32, 1,
        FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD, FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD_LEN,
        ciphertext_scratch, sizeof(ciphertext_scratch),
        out, sizeof(out));
    check(out_len > 0 && bytes_eq(out, out_len, FEB_VEC_WIFI_SCAN_CMD_RECORD, FEB_VEC_WIFI_SCAN_CMD_RECORD_LEN),
          "feb_session_encrypt_record(command, seq=1) matches FEB_VEC_WIFI_SCAN_CMD_RECORD byte-for-byte");

    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_WIFI_SCAN_CMD_RECORD, FEB_VEC_WIFI_SCAN_CMD_RECORD_LEN,
                                    FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_OK && record.sequence == 1 &&
              bytes_eq(record.plaintext, record.plaintext_len,
                       FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD, FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD_LEN),
          "feb_session_decrypt_record recovers the wifi_scan command payload at sequence 1");
}

static void test_wifi_scan_status_records(void)
{
    uint8_t ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];
    uint8_t out[FEB_MAX_RECORD_SIZE];
    size_t out_len;
    uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
    feb_session_decrypted_record_t record;
    feb_cbor_status_t st;

    out_len = feb_session_encrypt_record(
        FEB_VEC_SESS_KEY, 2, "status", strlen("status"),
        FEB_VEC_SESS_SESSION_ID, FEB_VEC_SESS_BOARD_ID, FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, 3,
        FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD_LEN,
        ciphertext_scratch, sizeof(ciphertext_scratch),
        out, sizeof(out));
    check(out_len > 0 && bytes_eq(out, out_len, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD,
                                  FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD_LEN),
          "feb_session_encrypt_record(status partial, seq=3) matches FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD");

    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD,
                                    FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_OK && record.sequence == 3 &&
              bytes_eq(record.plaintext, record.plaintext_len,
                       FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD_LEN),
          "feb_session_decrypt_record recovers the wifi_scan status(partial) payload at sequence 3");

    out_len = feb_session_encrypt_record(
        FEB_VEC_SESS_KEY, 2, "status", strlen("status"),
        FEB_VEC_SESS_SESSION_ID, FEB_VEC_SESS_BOARD_ID, FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, 4,
        FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD_LEN,
        ciphertext_scratch, sizeof(ciphertext_scratch),
        out, sizeof(out));
    check(out_len > 0 && bytes_eq(out, out_len, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD,
                                  FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD_LEN),
          "feb_session_encrypt_record(status complete, seq=4) matches FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD");

    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD,
                                    FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_OK && record.sequence == 4 &&
              bytes_eq(record.plaintext, record.plaintext_len,
                       FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD_LEN),
          "feb_session_decrypt_record recovers the wifi_scan status(complete) payload at sequence 4");
}

static int all_zero(const uint8_t *p, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* HP-28: an authenticated plaintext must be exactly one CBOR value; anything else is
   rejected and the plaintext buffer zeroized. HP-25: a 9-byte `version` is TOO_LARGE. */
static void test_decrypt_record_plaintext_strictness(void)
{
    uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
    feb_session_decrypted_record_t record;
    feb_cbor_status_t st;

    memset(plaintext, 0xA5, sizeof(plaintext));
    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_SESS_PROT_TRAILING_PLAINTEXT_RECORD,
                                    FEB_VEC_SESS_PROT_TRAILING_PLAINTEXT_RECORD_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_ERR_UNEXPECTED_TYPE && all_zero(plaintext, FEB_VEC_SESS_PROT1_PAYLOAD_LEN + 1u),
          "decrypt rejects trailing plaintext bytes (FEB_CBOR_ERR_UNEXPECTED_TYPE) and zeroizes plaintext");

    memset(plaintext, 0xA5, sizeof(plaintext));
    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_SESS_PROT_TRUNCATED_PLAINTEXT_RECORD,
                                    FEB_VEC_SESS_PROT_TRUNCATED_PLAINTEXT_RECORD_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_ERR_TRUNCATED && all_zero(plaintext, FEB_VEC_SESS_PROT1_PAYLOAD_LEN),
          "decrypt rejects truncated plaintext (FEB_CBOR_ERR_TRUNCATED) and zeroizes plaintext");

    st = feb_session_decrypt_record(FEB_VEC_SESS_KEY, FEB_VEC_SESS_PROT_RECORD_VERSION_2POW32_PLUS_2,
                                    FEB_VEC_SESS_PROT_RECORD_VERSION_2POW32_PLUS_2_LEN,
                                    FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, plaintext, sizeof(plaintext), &record);
    check(st == FEB_CBOR_ERR_TOO_LARGE, "decrypt rejects version 2^32+2 with FEB_CBOR_ERR_TOO_LARGE");

    {
        uint8_t deep_ct[FEB_CBOR_MAX_PAYLOAD];
        uint8_t deep_rec[FEB_MAX_RECORD_SIZE];
        uint8_t deep_pt[FEB_CBOR_MAX_PAYLOAD];
        feb_session_decrypted_record_t deep;
        size_t deep_len = feb_session_encrypt_record(
            FEB_VEC_SESS_KEY, 2, "status", strlen("status"), FEB_VEC_SESS_SESSION_ID, FEB_VEC_SESS_BOARD_ID,
            FEB_VEC_SESS_BOARD_ID_LEN, FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, 9,
            FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD, FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD_LEN, deep_ct,
            sizeof(deep_ct), deep_rec, sizeof(deep_rec));

        check(deep_len > 0 &&
                  feb_session_decrypt_record(FEB_VEC_SESS_KEY, deep_rec, deep_len,
                                             FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, deep_pt, sizeof(deep_pt),
                                             &deep) == FEB_CBOR_OK &&
                  bytes_eq(deep.plaintext, deep.plaintext_len, FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD,
                           FEB_VEC_WARDRIVING_STATUS_DATA_PAYLOAD_LEN),
              "decrypt accepts the 5-level wardriving status payload (single-value check has no depth cap)");
    }
}

/* HP-39: the nonce carries only 24 bits of sequence, so s and s + 2^24 would share a nonce.
   feb_session_encrypt_record() refuses every sequence >= FEB_SESSION_SEQUENCE_MAX, which
   makes that aliasing unreachable through the API. */
static void test_encrypt_record_sequence_cap(void)
{
    uint8_t ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];
    uint8_t out[FEB_MAX_RECORD_SIZE];
    uint8_t nonce_a[FEB_SESSION_NONCE_LEN];
    uint8_t nonce_b[FEB_SESSION_NONCE_LEN];
    static const uint64_t refused[] = {FEB_SESSION_SEQUENCE_MAX, FEB_SESSION_SEQUENCE_MAX + 1u,
                                       (1ull << 24) + 1u, 1ull << 32, UINT64_MAX};
    size_t i;
    int all_refused = 1;

    feb_session_build_nonce(FEB_VEC_SESS_SESSION_ID, FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, 1, nonce_a);
    feb_session_build_nonce(FEB_VEC_SESS_SESSION_ID, FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, (1ull << 24) + 1u,
                            nonce_b);
    check(memcmp(nonce_a, nonce_b, sizeof(nonce_a)) == 0,
          "nonce layout: sequence s and s + 2^24 alias (why the cap must be enforced)");

    check(feb_session_encrypt_record(FEB_VEC_SESS_KEY, 2, "error", strlen("error"), FEB_VEC_SESS_SESSION_ID,
                                     FEB_VEC_SESS_BOARD_ID, FEB_VEC_SESS_BOARD_ID_LEN,
                                     FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, FEB_SESSION_SEQUENCE_MAX - 1u,
                                     FEB_VEC_SESS_PROT1_PAYLOAD, FEB_VEC_SESS_PROT1_PAYLOAD_LEN,
                                     ciphertext_scratch, sizeof(ciphertext_scratch), out, sizeof(out)) > 0,
          "encrypt accepts sequence FEB_SESSION_SEQUENCE_MAX - 1");
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (feb_session_encrypt_record(FEB_VEC_SESS_KEY, 2, "error", strlen("error"), FEB_VEC_SESS_SESSION_ID,
                                       FEB_VEC_SESS_BOARD_ID, FEB_VEC_SESS_BOARD_ID_LEN,
                                       FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER, refused[i],
                                       FEB_VEC_SESS_PROT1_PAYLOAD, FEB_VEC_SESS_PROT1_PAYLOAD_LEN,
                                       ciphertext_scratch, sizeof(ciphertext_scratch), out, sizeof(out)) != 0) {
            all_refused = 0;
        }
    }
    check(all_refused, "encrypt refuses every sequence >= FEB_SESSION_SEQUENCE_MAX (incl. 2^24 + 1)");
}

int main(void)
{
    test_gcm_kat_encrypt();
    test_gcm_kat_decrypt();
    test_gcm_kat_tamper_rejected();

    test_golden_transcript();
    test_golden_proofs();
    test_golden_session_key();

    test_hello_payload_roundtrip();
    test_hello_ack_payload_roundtrip();
    test_client_auth_payload_roundtrip();

    test_unencrypted_record_roundtrip(FEB_VEC_SESS_HELLO_RECORD, FEB_VEC_SESS_HELLO_RECORD_LEN, FEB_HELLO_TYPE,
                                      FEB_VEC_SESS_HELLO_PAYLOAD, FEB_VEC_SESS_HELLO_PAYLOAD_LEN, "hello record");
    test_unencrypted_record_roundtrip(FEB_VEC_SESS_HELLO_ACK_RECORD, FEB_VEC_SESS_HELLO_ACK_RECORD_LEN, FEB_HELLO_ACK_TYPE,
                                      FEB_VEC_SESS_HELLO_ACK_PAYLOAD, FEB_VEC_SESS_HELLO_ACK_PAYLOAD_LEN, "hello_ack record");
    test_unencrypted_record_roundtrip(FEB_VEC_SESS_CLIENT_AUTH_RECORD, FEB_VEC_SESS_CLIENT_AUTH_RECORD_LEN, FEB_CLIENT_AUTH_TYPE,
                                      FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD, FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD_LEN, "client_auth record");

    test_aad_and_nonce();
    test_encrypt_record_matches_golden();
    test_decrypt_record_prot1_and_prot2();
    test_decrypt_record_tamper_rejected();
    test_decrypt_record_plaintext_strictness();
    test_encrypt_record_sequence_cap();

    test_wifi_scan_command_record();
    test_wifi_scan_status_records();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

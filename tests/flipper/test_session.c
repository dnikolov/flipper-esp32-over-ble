/* Host-native test for flipper/session_crypto.c and flipper/session.c against the shared
   vectors in tests/vectors/vectors.h. Built and run with MSVC (cl.exe); see
   build_session.ps1. Follows test_pairing.c's CHECK()/g_total/g_failed pattern. */
#include <stdio.h>
#include <string.h>

#include "session_crypto.h"
#include "session.h"
#include "pairing_crypto.h"
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

/* ---- AES-256-GCM known-answer vector (GCM spec Test Case 16) ---- */

static void test_gcm_kat(void) {
    static uint8_t ciphertext[FEB_VEC_GCM_PLAINTEXT_LEN];
    static uint8_t tag[FEB_SESSION_GCM_TAG_LEN];
    static uint8_t plaintext[FEB_VEC_GCM_CIPHERTEXT_LEN];

    feb_gcm_encrypt(
        FEB_VEC_GCM_KEY,
        FEB_VEC_GCM_IV,
        FEB_VEC_GCM_AAD,
        FEB_VEC_GCM_AAD_LEN,
        FEB_VEC_GCM_PLAINTEXT,
        FEB_VEC_GCM_PLAINTEXT_LEN,
        ciphertext,
        tag);
    CHECK(
        bytes_equal(ciphertext, sizeof(ciphertext), FEB_VEC_GCM_CIPHERTEXT, FEB_VEC_GCM_CIPHERTEXT_LEN),
        "AES-256-GCM: encrypt ciphertext matches GCM spec Test Case 16");
    CHECK(
        bytes_equal(tag, sizeof(tag), FEB_VEC_GCM_TAG, FEB_VEC_GCM_TAG_LEN),
        "AES-256-GCM: encrypt tag matches GCM spec Test Case 16");

    int ok = feb_gcm_decrypt(
        FEB_VEC_GCM_KEY,
        FEB_VEC_GCM_IV,
        FEB_VEC_GCM_AAD,
        FEB_VEC_GCM_AAD_LEN,
        FEB_VEC_GCM_CIPHERTEXT,
        FEB_VEC_GCM_CIPHERTEXT_LEN,
        FEB_VEC_GCM_TAG,
        plaintext);
    CHECK(ok == 1, "AES-256-GCM: decrypt of known ciphertext/tag verifies");
    CHECK(
        bytes_equal(plaintext, sizeof(plaintext), FEB_VEC_GCM_PLAINTEXT, FEB_VEC_GCM_PLAINTEXT_LEN),
        "AES-256-GCM: decrypted plaintext matches GCM spec Test Case 16");

    {
        static uint8_t bad_tag[FEB_SESSION_GCM_TAG_LEN];
        memcpy(bad_tag, FEB_VEC_GCM_TAG, sizeof(bad_tag));
        bad_tag[0] ^= 0x01;
        int bad_ok = feb_gcm_decrypt(
            FEB_VEC_GCM_KEY,
            FEB_VEC_GCM_IV,
            FEB_VEC_GCM_AAD,
            FEB_VEC_GCM_AAD_LEN,
            FEB_VEC_GCM_CIPHERTEXT,
            FEB_VEC_GCM_CIPHERTEXT_LEN,
            bad_tag,
            plaintext);
        CHECK(bad_ok == 0, "AES-256-GCM: decrypt rejects a tampered tag");
    }
}

/* ---- golden session transcript / proofs / key derivation ---- */

static feb_session_transcript_t g_transcript;
static uint8_t g_transcript_buf[FEB_SESSION_MAX_TRANSCRIPT_LEN];
static size_t g_transcript_len;

static void test_golden_transcript_and_proofs(void) {
    memset(&g_transcript, 0, sizeof(g_transcript));
    g_transcript.version = 2;
    g_transcript.board_id = FEB_VEC_SESS_BOARD_ID;
    g_transcript.board_id_len = FEB_VEC_SESS_BOARD_ID_LEN;
    memcpy(g_transcript.session_id, FEB_VEC_SESS_SESSION_ID, FEB_SESSION_ID_LEN);
    memcpy(g_transcript.client_nonce, FEB_VEC_SESS_CLIENT_NONCE, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(g_transcript.device_nonce, FEB_VEC_SESS_DEVICE_NONCE, FEB_SESSION_NONCE_FIELD_LEN);

    g_transcript_len =
        feb_session_encode_transcript(g_transcript_buf, sizeof(g_transcript_buf), &g_transcript);
    CHECK(g_transcript_len > 0, "golden: feb_session_encode_transcript succeeds");
    CHECK(
        bytes_equal(g_transcript_buf, g_transcript_len, FEB_VEC_SESS_TRANSCRIPT, FEB_VEC_SESS_TRANSCRIPT_LEN),
        "golden: S byte-identical to FEB_VEC_SESS_TRANSCRIPT");

    uint8_t flipper_proof[FEB_SESSION_PROOF_LEN];
    feb_session_flipper_proof(FEB_VEC_PAIR_SECRET, g_transcript_buf, g_transcript_len, flipper_proof);
    CHECK(
        bytes_equal(
            flipper_proof, sizeof(flipper_proof), FEB_VEC_SESS_FLIPPER_PROOF, FEB_VEC_SESS_FLIPPER_PROOF_LEN),
        "golden: flipper runtime proof matches FEB_VEC_SESS_FLIPPER_PROOF");

    uint8_t esp32_proof[FEB_SESSION_PROOF_LEN];
    feb_session_esp32_proof(FEB_VEC_PAIR_SECRET, g_transcript_buf, g_transcript_len, esp32_proof);
    CHECK(
        bytes_equal(esp32_proof, sizeof(esp32_proof), FEB_VEC_SESS_ESP32_PROOF, FEB_VEC_SESS_ESP32_PROOF_LEN),
        "golden: esp32 runtime proof matches FEB_VEC_SESS_ESP32_PROOF");

    uint8_t session_key[FEB_SESSION_KEY_LEN];
    feb_session_derive_key(
        FEB_VEC_PAIR_SECRET,
        FEB_VEC_SESS_CLIENT_NONCE,
        FEB_VEC_SESS_DEVICE_NONCE,
        FEB_VEC_SESS_BOARD_ID,
        FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_VEC_SESS_SESSION_ID,
        session_key);
    CHECK(
        bytes_equal(session_key, sizeof(session_key), FEB_VEC_SESS_KEY, FEB_VEC_SESS_KEY_LEN),
        "golden: derived session key matches FEB_VEC_SESS_KEY");
}

/* ---- hello / hello_ack / client_auth payload + record codec round trips ---- */

static void test_hello_codec(void) {
    feb_hello_payload_t p;
    static uint8_t payload_buf[64];
    size_t payload_len;

    memset(&p, 0, sizeof(p));
    memcpy(p.client_nonce, FEB_VEC_SESS_CLIENT_NONCE, FEB_SESSION_NONCE_FIELD_LEN);

    payload_len = feb_cbor_encode_hello_payload(payload_buf, sizeof(payload_buf), &p);
    CHECK(payload_len > 0, "hello: encode succeeds");
    CHECK(
        bytes_equal(payload_buf, payload_len, FEB_VEC_SESS_HELLO_PAYLOAD, FEB_VEC_SESS_HELLO_PAYLOAD_LEN),
        "hello: payload byte-identical to FEB_VEC_SESS_HELLO_PAYLOAD");

    {
        feb_hello_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_hello_payload(
            FEB_VEC_SESS_HELLO_PAYLOAD, FEB_VEC_SESS_HELLO_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "hello: decode status OK");
        CHECK(
            memcmp(decoded.client_nonce, FEB_VEC_SESS_CLIENT_NONCE, FEB_SESSION_NONCE_FIELD_LEN) == 0,
            "hello: decoded client_nonce matches golden vector");
    }

    static feb_unencrypted_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = 2;
    record.type = FEB_HELLO_TYPE;
    record.type_len = strlen(FEB_HELLO_TYPE);
    memcpy(record.session_id, FEB_VEC_SESS_SESSION_ID, FEB_SESSION_ID_LEN);
    record.board_id = FEB_VEC_SESS_BOARD_ID;
    record.board_id_len = FEB_VEC_SESS_BOARD_ID_LEN;
    record.payload_span = payload_buf;
    record.payload_span_len = payload_len;

    static uint8_t record_buf[160];
    size_t record_len = feb_cbor_encode_unencrypted(record_buf, sizeof(record_buf), &record);
    CHECK(record_len > 0, "hello: record encode succeeds");
    CHECK(
        bytes_equal(record_buf, record_len, FEB_VEC_SESS_HELLO_RECORD, FEB_VEC_SESS_HELLO_RECORD_LEN),
        "hello: record byte-identical to FEB_VEC_SESS_HELLO_RECORD");

    {
        feb_unencrypted_record_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_unencrypted(
            FEB_VEC_SESS_HELLO_RECORD, FEB_VEC_SESS_HELLO_RECORD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "hello: record decode status OK");
        CHECK(
            decoded.type_len == strlen(FEB_HELLO_TYPE) &&
                memcmp(decoded.type, FEB_HELLO_TYPE, decoded.type_len) == 0,
            "hello: decoded type == \"hello\"");
        CHECK(
            decoded.board_id_len == FEB_VEC_SESS_BOARD_ID_LEN &&
                memcmp(decoded.board_id, FEB_VEC_SESS_BOARD_ID, decoded.board_id_len) == 0,
            "hello: decoded board_id matches golden vector");
    }
}

static void test_hello_ack_codec(void) {
    feb_hello_ack_payload_t p;
    static uint8_t payload_buf[64];
    size_t payload_len;

    memset(&p, 0, sizeof(p));
    memcpy(p.device_nonce, FEB_VEC_SESS_DEVICE_NONCE, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(p.proof, FEB_VEC_SESS_FLIPPER_PROOF, FEB_SESSION_PROOF_LEN);

    payload_len = feb_cbor_encode_hello_ack_payload(payload_buf, sizeof(payload_buf), &p);
    CHECK(payload_len > 0, "hello_ack: encode succeeds");
    CHECK(
        bytes_equal(
            payload_buf, payload_len, FEB_VEC_SESS_HELLO_ACK_PAYLOAD, FEB_VEC_SESS_HELLO_ACK_PAYLOAD_LEN),
        "hello_ack: payload byte-identical to FEB_VEC_SESS_HELLO_ACK_PAYLOAD");

    {
        feb_hello_ack_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_hello_ack_payload(
            FEB_VEC_SESS_HELLO_ACK_PAYLOAD, FEB_VEC_SESS_HELLO_ACK_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "hello_ack: decode status OK");
        CHECK(
            memcmp(decoded.device_nonce, FEB_VEC_SESS_DEVICE_NONCE, FEB_SESSION_NONCE_FIELD_LEN) == 0 &&
                memcmp(decoded.proof, FEB_VEC_SESS_FLIPPER_PROOF, FEB_SESSION_PROOF_LEN) == 0,
            "hello_ack: decoded fields match golden vector");
    }

    static feb_unencrypted_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = 2;
    record.type = FEB_HELLO_ACK_TYPE;
    record.type_len = strlen(FEB_HELLO_ACK_TYPE);
    memcpy(record.session_id, FEB_VEC_SESS_SESSION_ID, FEB_SESSION_ID_LEN);
    record.board_id = FEB_VEC_SESS_BOARD_ID;
    record.board_id_len = FEB_VEC_SESS_BOARD_ID_LEN;
    record.payload_span = payload_buf;
    record.payload_span_len = payload_len;

    static uint8_t record_buf[160];
    size_t record_len = feb_cbor_encode_unencrypted(record_buf, sizeof(record_buf), &record);
    CHECK(record_len > 0, "hello_ack: record encode succeeds");
    CHECK(
        bytes_equal(record_buf, record_len, FEB_VEC_SESS_HELLO_ACK_RECORD, FEB_VEC_SESS_HELLO_ACK_RECORD_LEN),
        "hello_ack: record byte-identical to FEB_VEC_SESS_HELLO_ACK_RECORD");
}

static void test_client_auth_codec(void) {
    feb_client_auth_payload_t p;
    static uint8_t payload_buf[64];
    size_t payload_len;

    memset(&p, 0, sizeof(p));
    memcpy(p.proof, FEB_VEC_SESS_ESP32_PROOF, FEB_SESSION_PROOF_LEN);

    payload_len = feb_cbor_encode_client_auth_payload(payload_buf, sizeof(payload_buf), &p);
    CHECK(payload_len > 0, "client_auth: encode succeeds");
    CHECK(
        bytes_equal(
            payload_buf, payload_len, FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD, FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD_LEN),
        "client_auth: payload byte-identical to FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD");

    {
        feb_client_auth_payload_t decoded;
        feb_cbor_status_t status = feb_cbor_decode_client_auth_payload(
            FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD, FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD_LEN, &decoded);
        CHECK(status == FEB_CBOR_OK, "client_auth: decode status OK");
        CHECK(
            memcmp(decoded.proof, FEB_VEC_SESS_ESP32_PROOF, FEB_SESSION_PROOF_LEN) == 0,
            "client_auth: decoded proof matches golden vector");
    }

    static feb_unencrypted_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = 2;
    record.type = FEB_CLIENT_AUTH_TYPE;
    record.type_len = strlen(FEB_CLIENT_AUTH_TYPE);
    memcpy(record.session_id, FEB_VEC_SESS_SESSION_ID, FEB_SESSION_ID_LEN);
    record.board_id = FEB_VEC_SESS_BOARD_ID;
    record.board_id_len = FEB_VEC_SESS_BOARD_ID_LEN;
    record.payload_span = payload_buf;
    record.payload_span_len = payload_len;

    static uint8_t record_buf[160];
    size_t record_len = feb_cbor_encode_unencrypted(record_buf, sizeof(record_buf), &record);
    CHECK(record_len > 0, "client_auth: record encode succeeds");
    CHECK(
        bytes_equal(
            record_buf, record_len, FEB_VEC_SESS_CLIENT_AUTH_RECORD, FEB_VEC_SESS_CLIENT_AUTH_RECORD_LEN),
        "client_auth: record byte-identical to FEB_VEC_SESS_CLIENT_AUTH_RECORD");
}

/* ---- protected-record encrypt/decrypt against the golden session vector ---- */

static void test_protected_record_encrypt(void) {
    static uint8_t ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];
    static uint8_t record_buf[768];

    size_t record_len = feb_session_encrypt_record(
        FEB_VEC_SESS_KEY,
        2,
        "error",
        strlen("error"),
        FEB_VEC_SESS_SESSION_ID,
        FEB_VEC_SESS_BOARD_ID,
        FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
        1,
        FEB_VEC_SESS_PROT1_PAYLOAD,
        FEB_VEC_SESS_PROT1_PAYLOAD_LEN,
        ciphertext_scratch,
        sizeof(ciphertext_scratch),
        record_buf,
        sizeof(record_buf));
    CHECK(record_len > 0, "protected record: encrypt+encode succeeds (sequence 1)");
    CHECK(
        bytes_equal(ciphertext_scratch, FEB_VEC_SESS_PROT1_PAYLOAD_LEN, FEB_VEC_SESS_PROT1_CIPHERTEXT, FEB_VEC_SESS_PROT1_CIPHERTEXT_LEN),
        "protected record: ciphertext matches FEB_VEC_SESS_PROT1_CIPHERTEXT");
    CHECK(
        bytes_equal(record_buf, record_len, FEB_VEC_SESS_PROT1_RECORD, FEB_VEC_SESS_PROT1_RECORD_LEN),
        "protected record: full record byte-identical to FEB_VEC_SESS_PROT1_RECORD");
}

static void test_protected_record_decrypt(void) {
    static uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
    feb_session_decrypted_record_t record;

    feb_cbor_status_t status = feb_session_decrypt_record(
        FEB_VEC_SESS_KEY,
        FEB_VEC_SESS_PROT1_RECORD,
        FEB_VEC_SESS_PROT1_RECORD_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
        plaintext,
        sizeof(plaintext),
        &record);
    CHECK(status == FEB_CBOR_OK, "protected record: decrypt of sequence-1 record succeeds");
    CHECK(record.sequence == 1, "protected record: decoded sequence == 1");
    CHECK(
        bytes_equal(record.plaintext, record.plaintext_len, FEB_VEC_SESS_PROT1_PAYLOAD, FEB_VEC_SESS_PROT1_PAYLOAD_LEN),
        "protected record: decrypted plaintext matches FEB_VEC_SESS_PROT1_PAYLOAD");

    status = feb_session_decrypt_record(
        FEB_VEC_SESS_KEY,
        FEB_VEC_SESS_PROT2_RECORD,
        FEB_VEC_SESS_PROT2_RECORD_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
        plaintext,
        sizeof(plaintext),
        &record);
    CHECK(status == FEB_CBOR_OK, "protected record: decrypt of sequence-2 record succeeds");
    CHECK(record.sequence == 2, "protected record: decoded sequence == 2");
    CHECK(
        bytes_equal(record.plaintext, record.plaintext_len, FEB_VEC_SESS_PROT2_PAYLOAD, FEB_VEC_SESS_PROT2_PAYLOAD_LEN),
        "protected record: decrypted plaintext matches FEB_VEC_SESS_PROT2_PAYLOAD");
}

static void test_protected_record_tamper_rejection(void) {
    static uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
    feb_session_decrypted_record_t record;

    feb_cbor_status_t status = feb_session_decrypt_record(
        FEB_VEC_SESS_KEY,
        FEB_VEC_SESS_PROT1_RECORD_BAD_CIPHERTEXT,
        FEB_VEC_SESS_PROT1_RECORD_BAD_CIPHERTEXT_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
        plaintext,
        sizeof(plaintext),
        &record);
    CHECK(
        status == FEB_CBOR_ERR_AUTH_FAILED,
        "protected record: tampered ciphertext rejected with FEB_CBOR_ERR_AUTH_FAILED");

    status = feb_session_decrypt_record(
        FEB_VEC_SESS_KEY,
        FEB_VEC_SESS_PROT1_RECORD_BAD_AAD,
        FEB_VEC_SESS_PROT1_RECORD_BAD_AAD_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
        plaintext,
        sizeof(plaintext),
        &record);
    CHECK(
        status == FEB_CBOR_ERR_AUTH_FAILED,
        "protected record: tampered AAD (sequence field) rejected with FEB_CBOR_ERR_AUTH_FAILED");
}

/* ---- wifi_scan command/status wrapped as protected records under the golden session
   (docs/PLAN.md's Wi-Fi scan capability follow-on step) -- `command` is the session's first
   Flipper->ESP32 protected record (sequence 1); the two `status` records continue the
   ESP32->Flipper counter after FEB_VEC_SESS_PROT1/PROT2 (sequence 3, 4). ---- */

static void test_wifi_scan_command_record(void) {
    static uint8_t ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];
    static uint8_t record_buf[768];

    size_t record_len = feb_session_encrypt_record(
        FEB_VEC_SESS_KEY,
        2,
        "command",
        strlen("command"),
        FEB_VEC_SESS_SESSION_ID,
        FEB_VEC_SESS_BOARD_ID,
        FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
        1,
        FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD,
        FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD_LEN,
        ciphertext_scratch,
        sizeof(ciphertext_scratch),
        record_buf,
        sizeof(record_buf));
    CHECK(record_len > 0, "wifi_scan command record: encrypt+encode succeeds (sequence 1)");
    CHECK(
        bytes_equal(record_buf, record_len, FEB_VEC_WIFI_SCAN_CMD_RECORD, FEB_VEC_WIFI_SCAN_CMD_RECORD_LEN),
        "wifi_scan command record: byte-identical to FEB_VEC_WIFI_SCAN_CMD_RECORD");

    static uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
    feb_session_decrypted_record_t decoded;
    feb_cbor_status_t status = feb_session_decrypt_record(
        FEB_VEC_SESS_KEY,
        FEB_VEC_WIFI_SCAN_CMD_RECORD,
        FEB_VEC_WIFI_SCAN_CMD_RECORD_LEN,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
        plaintext,
        sizeof(plaintext),
        &decoded);
    CHECK(status == FEB_CBOR_OK, "wifi_scan command record: decrypt succeeds");
    CHECK(decoded.sequence == 1, "wifi_scan command record: decoded sequence == 1");
    CHECK(
        bytes_equal(
            decoded.plaintext, decoded.plaintext_len, FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD,
            FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD_LEN),
        "wifi_scan command record: decrypted plaintext matches FEB_VEC_WIFI_SCAN_COMMAND_PAYLOAD");
}

static void test_wifi_scan_status_records(void) {
    static uint8_t ciphertext_scratch[FEB_CBOR_MAX_PAYLOAD];
    static uint8_t record_buf[768];

    size_t partial_len = feb_session_encrypt_record(
        FEB_VEC_SESS_KEY,
        2,
        "status",
        strlen("status"),
        FEB_VEC_SESS_SESSION_ID,
        FEB_VEC_SESS_BOARD_ID,
        FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
        3,
        FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD,
        FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD_LEN,
        ciphertext_scratch,
        sizeof(ciphertext_scratch),
        record_buf,
        sizeof(record_buf));
    CHECK(partial_len > 0, "wifi_scan status partial record: encrypt+encode succeeds (sequence 3)");
    CHECK(
        bytes_equal(
            record_buf, partial_len, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD,
            FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD_LEN),
        "wifi_scan status partial record: byte-identical to FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD");

    {
        static uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
        feb_session_decrypted_record_t decoded;
        feb_cbor_status_t status = feb_session_decrypt_record(
            FEB_VEC_SESS_KEY,
            FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD,
            FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_RECORD_LEN,
            FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
            plaintext,
            sizeof(plaintext),
            &decoded);
        CHECK(status == FEB_CBOR_OK, "wifi_scan status partial record: decrypt succeeds");
        CHECK(decoded.sequence == 3, "wifi_scan status partial record: decoded sequence == 3");
        CHECK(
            bytes_equal(
                decoded.plaintext, decoded.plaintext_len, FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD,
                FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD_LEN),
            "wifi_scan status partial record: decrypted plaintext matches vector payload");
    }

    size_t complete_len = feb_session_encrypt_record(
        FEB_VEC_SESS_KEY,
        2,
        "status",
        strlen("status"),
        FEB_VEC_SESS_SESSION_ID,
        FEB_VEC_SESS_BOARD_ID,
        FEB_VEC_SESS_BOARD_ID_LEN,
        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
        4,
        FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD,
        FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD_LEN,
        ciphertext_scratch,
        sizeof(ciphertext_scratch),
        record_buf,
        sizeof(record_buf));
    CHECK(complete_len > 0, "wifi_scan status complete record: encrypt+encode succeeds (sequence 4)");
    CHECK(
        bytes_equal(
            record_buf, complete_len, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD,
            FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD_LEN),
        "wifi_scan status complete record: byte-identical to FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD");

    {
        static uint8_t plaintext[FEB_CBOR_MAX_PAYLOAD];
        feb_session_decrypted_record_t decoded;
        feb_cbor_status_t status = feb_session_decrypt_record(
            FEB_VEC_SESS_KEY,
            FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD,
            FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_RECORD_LEN,
            FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
            plaintext,
            sizeof(plaintext),
            &decoded);
        CHECK(status == FEB_CBOR_OK, "wifi_scan status complete record: decrypt succeeds");
        CHECK(decoded.sequence == 4, "wifi_scan status complete record: decoded sequence == 4");
        CHECK(
            bytes_equal(
                decoded.plaintext, decoded.plaintext_len, FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD,
                FEB_VEC_WIFI_SCAN_STATUS_COMPLETE_PAYLOAD_LEN),
            "wifi_scan status complete record: decrypted plaintext matches vector payload");
    }
}

int main(void) {
    test_gcm_kat();
    test_golden_transcript_and_proofs();
    test_hello_codec();
    test_hello_ack_codec();
    test_client_auth_codec();
    test_protected_record_encrypt();
    test_protected_record_decrypt();
    test_protected_record_tamper_rejection();
    test_wifi_scan_command_record();
    test_wifi_scan_status_records();

    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}

/* Implements flipper/session.h (frozen contract, do not change the header). Field order,
   error handling, and helper style deliberately mirror flipper/pairing.c (map-header count
   checks, canonical field-order enforcement via decode_expected_key, duplicate-key
   rejection, exact-length checks on fixed-size byte fields) -- see that file for the
   established pattern this one follows. Per docs/PLAN.md step 6 (matching step 5's
   pairing.c note), the small helpers below (text_matches/decode_expected_key/
   decode_fixed_bytes) are intentionally duplicated here rather than exposed from
   cbor_codec.c's private surface.

   Stack-budget note (session.h's own top comment, docs/SESSION_MEMORY.md's step 3/5
   entries -- this bug class has recurred three times): every scratch buffer this file
   needs internally is file-scope `static`, not stack-local, from the start -- reachable
   from profile_event_handler() on the Flipper's 1280-byte BleEventWorker stack. Safe as
   static for the same reason every other static in pairing.c/pairing_crypto.c is: BLE
   event dispatch is synchronous and single-in-flight against one active connection, so
   there is never a concurrent or reentrant call into any function here. */
#include "session.h"

#include <string.h>

/* ---- shared field-order helpers (mirrors pairing.c's private helpers) ---- */

static int text_matches(const char* data, size_t len, const char* literal) {
    size_t literal_len = strlen(literal);
    return len == literal_len && memcmp(data, literal, literal_len) == 0;
}

static size_t decode_expected_key(
    const uint8_t* in,
    size_t in_len,
    const char* expected,
    const uint8_t** seen_ptrs,
    size_t* seen_lens,
    size_t seen_count,
    feb_cbor_status_t* status) {
    const char* key_data;
    size_t key_len;
    size_t n = feb_cbor_decode_text(in, in_len, &key_data, &key_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) {
        return 0;
    }
    for(size_t j = 0; j < seen_count; j++) {
        if(seen_lens[j] == key_len && memcmp(seen_ptrs[j], key_data, key_len) == 0) {
            *status = FEB_CBOR_ERR_DUPLICATE_KEY;
            return 0;
        }
    }
    if(!text_matches(key_data, key_len, expected)) {
        *status = FEB_CBOR_ERR_OUT_OF_ORDER;
        return 0;
    }
    seen_ptrs[seen_count] = (const uint8_t*)key_data;
    seen_lens[seen_count] = key_len;
    *status = FEB_CBOR_OK;
    return n;
}

static size_t decode_fixed_bytes(
    const uint8_t* in,
    size_t in_len,
    uint8_t* field,
    size_t field_len,
    feb_cbor_status_t* status) {
    const uint8_t* data;
    size_t len;
    size_t n = feb_cbor_decode_bytes(in, in_len, &data, &len, field_len, status);
    if(n == 0) {
        return 0;
    }
    if(len != field_len) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memcpy(field, data, field_len);
    return n;
}

/* ---- hello ---- */

size_t feb_cbor_encode_hello_payload(uint8_t* out, size_t out_cap, const feb_hello_payload_t* p) {
    if(out == NULL || p == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "client_nonce", sizeof("client_nonce") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_hello_payload(const uint8_t* in, size_t in_len, feb_hello_payload_t* p) {
    if(in == NULL || p == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 1) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[1];
    size_t seen_lens[1];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "client_nonce", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(in + pos, in_len - pos, p->client_nonce, FEB_SESSION_NONCE_FIELD_LEN, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

/* ---- hello_ack ---- */

size_t
    feb_cbor_encode_hello_ack_payload(uint8_t* out, size_t out_cap, const feb_hello_ack_payload_t* p) {
    if(out == NULL || p == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 2);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "device_nonce", sizeof("device_nonce") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->device_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "proof", sizeof("proof") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->proof, FEB_SESSION_PROOF_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_hello_ack_payload(const uint8_t* in, size_t in_len, feb_hello_ack_payload_t* p) {
    if(in == NULL || p == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 2) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 2) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[2];
    size_t seen_lens[2];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "device_nonce", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(in + pos, in_len - pos, p->device_nonce, FEB_SESSION_NONCE_FIELD_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "proof", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(in + pos, in_len - pos, p->proof, FEB_SESSION_PROOF_LEN, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

/* ---- client_auth ---- */

size_t
    feb_cbor_encode_client_auth_payload(uint8_t* out, size_t out_cap, const feb_client_auth_payload_t* p) {
    if(out == NULL || p == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "proof", sizeof("proof") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->proof, FEB_SESSION_PROOF_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_client_auth_payload(const uint8_t* in, size_t in_len, feb_client_auth_payload_t* p) {
    if(in == NULL || p == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 1) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[1];
    size_t seen_lens[1];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "proof", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(in + pos, in_len - pos, p->proof, FEB_SESSION_PROOF_LEN, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

/* ---- transcript S ---- */

size_t feb_session_encode_transcript(uint8_t* out, size_t out_cap, const feb_session_transcript_t* s) {
    if(out == NULL || s == NULL) {
        return 0;
    }
    if(s->board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 5);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", sizeof("version") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, s->version);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", sizeof("board_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, s->board_id, s->board_id_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", sizeof("session_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, s->session_id, FEB_SESSION_ID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "client_nonce", sizeof("client_nonce") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, s->client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "device_nonce", sizeof("device_nonce") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, s->device_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    if(n == 0) return 0;
    pos += n;

    return pos;
}

/* ---- runtime proofs ---- */

#define FEB_SESSION_LABEL_MAX_LEN 16u

/* Computes HMAC-SHA-256(pairing_secret, label || s), truncated to FEB_SESSION_PROOF_LEN
   bytes. `s` is S as produced by feb_session_encode_transcript(), so label_len + s_len is
   always within this function's fixed-size local buffer given FEB_SESSION_LABEL_MAX_LEN
   and FEB_SESSION_MAX_TRANSCRIPT_LEN; both are clamped defensively regardless. */
static void session_proof_tag(
    const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN],
    const char* label,
    size_t label_len,
    const uint8_t* s,
    size_t s_len,
    uint8_t out[FEB_SESSION_PROOF_LEN]) {
    /* static, not stack-local: sized off FEB_SESSION_MAX_TRANSCRIPT_LEN (160 bytes for
       `buf` alone), reachable from profile_event_handler on the Flipper's 1280-byte
       BleEventWorker thread. Safe as static: single-in-flight BLE event dispatch, not
       reentrant/recursive; `buf` is fully written up to `total` bytes before being read
       (feb_hmac_sha256() is only ever passed that same `total` length) and zeroized below
       on every call regardless of path. */
    static uint8_t buf[FEB_SESSION_LABEL_MAX_LEN + FEB_SESSION_MAX_TRANSCRIPT_LEN];
    static uint8_t mac[FEB_HMAC_SHA256_LEN];
    size_t total;

    if(label_len > FEB_SESSION_LABEL_MAX_LEN) {
        label_len = FEB_SESSION_LABEL_MAX_LEN;
    }
    if(s_len > FEB_SESSION_MAX_TRANSCRIPT_LEN) {
        s_len = FEB_SESSION_MAX_TRANSCRIPT_LEN;
    }

    memcpy(buf, label, label_len);
    memcpy(buf + label_len, s, s_len);
    total = label_len + s_len;

    feb_hmac_sha256(pairing_secret, FEB_PAIRING_SECRET_LEN, buf, total, mac);
    memcpy(out, mac, FEB_SESSION_PROOF_LEN);

    feb_secure_zero(buf, sizeof(buf));
    feb_secure_zero(mac, sizeof(mac));
}

void feb_session_flipper_proof(
    const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN],
    const uint8_t* s,
    size_t s_len,
    uint8_t out[FEB_SESSION_PROOF_LEN]) {
    static const char label[] = "runtime-flipper";
    session_proof_tag(pairing_secret, label, sizeof(label) - 1, s, s_len, out);
}

void feb_session_esp32_proof(
    const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN],
    const uint8_t* s,
    size_t s_len,
    uint8_t out[FEB_SESSION_PROOF_LEN]) {
    static const char label[] = "runtime-esp32";
    session_proof_tag(pairing_secret, label, sizeof(label) - 1, s, s_len, out);
}

/* ---- session key derivation ---- */

void feb_session_derive_key(
    const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN],
    const uint8_t client_nonce[FEB_SESSION_NONCE_FIELD_LEN],
    const uint8_t device_nonce[FEB_SESSION_NONCE_FIELD_LEN],
    const char* board_id,
    size_t board_id_len,
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    uint8_t out[FEB_SESSION_KEY_LEN]) {
    static const char info_prefix[] = "flipper-esp32-over-ble/v2/aes-256-gcm";
    /* static, not stack-local: reachable from profile_event_handler on the Flipper's
       1280-byte BleEventWorker thread. Safe as static: single-in-flight BLE event
       dispatch, not reentrant/recursive; `salt` is fully written by two memcpy()s below
       before use, `info` is fully written up to `info_len` before being read
       (feb_hkdf_sha256() is only ever passed that same `info_len`), and both are zeroized
       below on every call. */
    static uint8_t salt[FEB_SESSION_NONCE_FIELD_LEN * 2];
    static uint8_t info[sizeof(info_prefix) - 1 + FEB_PAIRING_BOARD_ID_MAX_LEN + FEB_SESSION_ID_LEN];
    size_t info_len;

    memcpy(salt, client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(salt + FEB_SESSION_NONCE_FIELD_LEN, device_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    memcpy(info, info_prefix, sizeof(info_prefix) - 1);
    info_len = sizeof(info_prefix) - 1;
    /* Callers must uphold board_id_len <= FEB_PAIRING_BOARD_ID_MAX_LEN (enforced upstream
       by the pairing/session envelope decoders); this clamp is a defensive backstop
       against buffer overflow, not a substitute for that check. */
    if(board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        board_id_len = FEB_PAIRING_BOARD_ID_MAX_LEN;
    }
    if(board_id_len > 0) {
        memcpy(info + info_len, board_id, board_id_len);
    }
    info_len += board_id_len;
    memcpy(info + info_len, session_id, FEB_SESSION_ID_LEN);
    info_len += FEB_SESSION_ID_LEN;

    (void)feb_hkdf_sha256(
        salt, sizeof(salt), pairing_secret, FEB_PAIRING_SECRET_LEN, info, info_len, out, FEB_SESSION_KEY_LEN);

    feb_secure_zero(salt, sizeof(salt));
    feb_secure_zero(info, sizeof(info));
}

/* ---- AAD ---- */

size_t feb_session_encode_aad(uint8_t* out, size_t out_cap, const feb_session_aad_t* aad) {
    if(out == NULL || aad == NULL) {
        return 0;
    }
    if(aad->type_len > FEB_CBOR_MAX_TEXT_LEN || aad->board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 5);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", sizeof("version") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, aad->version);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", sizeof("type") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, aad->type, aad->type_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", sizeof("session_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, aad->session_id, FEB_SESSION_ID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "sequence", sizeof("sequence") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, aad->sequence);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", sizeof("board_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, aad->board_id, aad->board_id_len);
    if(n == 0) return 0;
    pos += n;

    return pos;
}

/* ---- nonce ---- */

void feb_session_build_nonce(
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    uint8_t direction,
    uint64_t sequence,
    uint8_t out[FEB_SESSION_NONCE_LEN]) {
    memcpy(out, session_id, FEB_SESSION_ID_LEN);
    out[FEB_SESSION_ID_LEN] = direction;
    out[FEB_SESSION_ID_LEN + 1] = (uint8_t)((sequence >> 16) & 0xFFu);
    out[FEB_SESSION_ID_LEN + 2] = (uint8_t)((sequence >> 8) & 0xFFu);
    out[FEB_SESSION_ID_LEN + 3] = (uint8_t)(sequence & 0xFFu);
}

/* ---- protected-record encrypt/decrypt ----
   session_aad_buf/session_nonce_buf/session_tag_buf are shared scratch across both
   directions (static, not stack-local -- see this file's top comment): only one BLE event
   is ever in flight at a time, and a protected record is never simultaneously being
   encrypted and decrypted, so one set of buffers is sufficient. */

static uint8_t session_aad_buf[FEB_SESSION_MAX_AAD_LEN];
static uint8_t session_nonce_buf[FEB_SESSION_NONCE_LEN];
static uint8_t session_tag_buf[FEB_SESSION_GCM_TAG_LEN];

size_t feb_session_encrypt_record(
    const uint8_t session_key[FEB_SESSION_KEY_LEN],
    uint32_t version,
    const char* type,
    size_t type_len,
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    const char* board_id,
    size_t board_id_len,
    uint8_t direction,
    uint64_t sequence,
    const uint8_t* payload,
    size_t payload_len,
    uint8_t* ciphertext_scratch,
    size_t ciphertext_scratch_cap,
    uint8_t* out,
    size_t out_cap) {
    if(payload_len > FEB_CBOR_MAX_PAYLOAD || ciphertext_scratch_cap < payload_len) {
        return 0;
    }

    /* static, not stack-local: matches feb_session_decrypt_record()'s protected_record
       below -- both structs carry several pointer/length fields, and once step 7 wires
       this function into profile_event_handler() they land on the same 1280-byte
       BleEventWorker stack. Safe as static for the same single-in-flight-dispatch reason
       as every other static in this module; both are fully overwritten by the memset()+
       field-assignment sequence immediately below before being read. */
    static feb_session_aad_t aad;
    memset(&aad, 0, sizeof(aad));
    aad.version = version;
    aad.type = type;
    aad.type_len = type_len;
    memcpy(aad.session_id, session_id, FEB_SESSION_ID_LEN);
    aad.sequence = sequence;
    aad.board_id = board_id;
    aad.board_id_len = board_id_len;

    size_t aad_len = feb_session_encode_aad(session_aad_buf, sizeof(session_aad_buf), &aad);
    if(aad_len == 0) {
        return 0;
    }

    feb_session_build_nonce(session_id, direction, sequence, session_nonce_buf);

    feb_gcm_encrypt(
        session_key,
        session_nonce_buf,
        session_aad_buf,
        aad_len,
        payload,
        payload_len,
        ciphertext_scratch,
        session_tag_buf);

    static feb_protected_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = version;
    record.type = type;
    record.type_len = type_len;
    memcpy(record.session_id, session_id, FEB_SESSION_ID_LEN);
    record.board_id = board_id;
    record.board_id_len = board_id_len;
    record.sequence = sequence;
    record.ciphertext = ciphertext_scratch;
    record.ciphertext_len = payload_len;
    memcpy(record.tag, session_tag_buf, FEB_GCM_TAG_LEN);

    return feb_cbor_encode_protected(out, out_cap, &record);
}

feb_cbor_status_t feb_session_decrypt_record(
    const uint8_t session_key[FEB_SESSION_KEY_LEN],
    const uint8_t* in,
    size_t in_len,
    uint8_t direction,
    uint8_t* plaintext_out,
    size_t plaintext_out_cap,
    feb_session_decrypted_record_t* record) {
    if(in == NULL || plaintext_out == NULL || record == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }

    /* static, not stack-local: feb_protected_record_t carries several pointer/length
       fields plus a 16-byte tag array, reachable from profile_event_handler on the
       Flipper's 1280-byte BleEventWorker thread. Safe as static: single-in-flight BLE
       event dispatch, fully overwritten by feb_cbor_decode_protected() before being read. */
    static feb_protected_record_t protected_record;
    feb_cbor_status_t status = feb_cbor_decode_protected(in, in_len, &protected_record);
    if(status != FEB_CBOR_OK) {
        return status;
    }
    if(protected_record.ciphertext_len > plaintext_out_cap) {
        return FEB_CBOR_ERR_TOO_LARGE;
    }

    static feb_session_aad_t aad;
    memset(&aad, 0, sizeof(aad));
    aad.version = protected_record.version;
    aad.type = protected_record.type;
    aad.type_len = protected_record.type_len;
    memcpy(aad.session_id, protected_record.session_id, FEB_SESSION_ID_LEN);
    aad.sequence = protected_record.sequence;
    aad.board_id = protected_record.board_id;
    aad.board_id_len = protected_record.board_id_len;

    /* AAD is always rebuilt from the fields this decode just parsed, never trusted from a
       cached/prior value -- this is what makes a tampered outer `sequence` (or any other
       AAD-covered field) fail authentication instead of silently validating against a
       stale AAD (docs/PROTOCOL.md's "reject modified AAD" requirement; exercised by
       tests/vectors/vectors.h's FEB_VEC_SESS_PROT1_RECORD_BAD_AAD). */
    size_t aad_len = feb_session_encode_aad(session_aad_buf, sizeof(session_aad_buf), &aad);
    if(aad_len == 0) {
        return FEB_CBOR_ERR_TOO_LARGE;
    }

    feb_session_build_nonce(
        protected_record.session_id, direction, protected_record.sequence, session_nonce_buf);

    int ok = feb_gcm_decrypt(
        session_key,
        session_nonce_buf,
        session_aad_buf,
        aad_len,
        protected_record.ciphertext,
        protected_record.ciphertext_len,
        protected_record.tag,
        plaintext_out);
    if(!ok) {
        return FEB_CBOR_ERR_AUTH_FAILED;
    }

    record->version = protected_record.version;
    record->type = protected_record.type;
    record->type_len = protected_record.type_len;
    memcpy(record->session_id, protected_record.session_id, FEB_SESSION_ID_LEN);
    record->board_id = protected_record.board_id;
    record->board_id_len = protected_record.board_id_len;
    record->sequence = protected_record.sequence;
    record->plaintext = plaintext_out;
    record->plaintext_len = protected_record.ciphertext_len;

    return FEB_CBOR_OK;
}

#include "session.h"

#include <string.h>

#define KLEN(literal) (sizeof(literal) - 1u)

/* Longest label used below is "runtime-flipper" (15 bytes); rounded up for margin. */
#define FEB_SESSION_LABEL_MAX_LEN 16u
#define FEB_SESSION_HMAC_BUF_LEN (FEB_SESSION_LABEL_MAX_LEN + FEB_SESSION_MAX_TRANSCRIPT_LEN)

size_t feb_cbor_encode_hello_payload(uint8_t *out, size_t out_cap, const feb_hello_payload_t *p)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || p == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "client_nonce", KLEN("client_nonce"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_hello_payload(const uint8_t *in, size_t in_len, feb_hello_payload_t *p)
{
    static const char *const names[1] = {"client_nonce"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[1] = {0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || p == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    if (count == 0) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    pos = consumed;

    for (i = 0; i < count; i++) {
        const char *key_data;
        size_t key_len;
        size_t key_consumed;
        int found;
        size_t j;
        size_t field_index;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);
        if (key_consumed == 0) {
            return status;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 1; j++) {
            if (key_len == strlen(names[j]) && memcmp(key_data, names[j], key_len) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        field_index = (size_t)found;
        if (seen[field_index]) {
            return FEB_CBOR_ERR_DUPLICATE_KEY;
        }
        if (field_index < next_min) {
            return FEB_CBOR_ERR_OUT_OF_ORDER;
        }

        {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_SESSION_NONCE_FIELD_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_SESSION_NONCE_FIELD_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->client_nonce, data, FEB_SESSION_NONCE_FIELD_LEN);
            pos += n;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0]) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_hello_ack_payload(uint8_t *out, size_t out_cap, const feb_hello_ack_payload_t *p)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || p == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 2);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "device_nonce", KLEN("device_nonce"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->device_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "proof", KLEN("proof"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->proof, FEB_SESSION_PROOF_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_hello_ack_payload(const uint8_t *in, size_t in_len, feb_hello_ack_payload_t *p)
{
    static const char *const names[2] = {"device_nonce", "proof"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[2] = {0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || p == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 2) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos = consumed;

    for (i = 0; i < count; i++) {
        const char *key_data;
        size_t key_len;
        size_t key_consumed;
        int found;
        size_t j;
        size_t field_index;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);
        if (key_consumed == 0) {
            return status;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 2; j++) {
            if (key_len == strlen(names[j]) && memcmp(key_data, names[j], key_len) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        field_index = (size_t)found;
        if (seen[field_index]) {
            return FEB_CBOR_ERR_DUPLICATE_KEY;
        }
        if (field_index < next_min) {
            return FEB_CBOR_ERR_OUT_OF_ORDER;
        }

        switch (field_index) {
        case 0: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_SESSION_NONCE_FIELD_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_SESSION_NONCE_FIELD_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->device_nonce, data, FEB_SESSION_NONCE_FIELD_LEN);
            pos += n;
            break;
        }
        case 1: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_SESSION_PROOF_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_SESSION_PROOF_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->proof, data, FEB_SESSION_PROOF_LEN);
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 2; i++) {
        if (!seen[i]) {
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_client_auth_payload(uint8_t *out, size_t out_cap, const feb_client_auth_payload_t *p)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || p == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "proof", KLEN("proof"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->proof, FEB_SESSION_PROOF_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_client_auth_payload(const uint8_t *in, size_t in_len, feb_client_auth_payload_t *p)
{
    static const char *const names[1] = {"proof"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[1] = {0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || p == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    if (count == 0) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    pos = consumed;

    for (i = 0; i < count; i++) {
        const char *key_data;
        size_t key_len;
        size_t key_consumed;
        int found;
        size_t j;
        size_t field_index;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);
        if (key_consumed == 0) {
            return status;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 1; j++) {
            if (key_len == strlen(names[j]) && memcmp(key_data, names[j], key_len) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        field_index = (size_t)found;
        if (seen[field_index]) {
            return FEB_CBOR_ERR_DUPLICATE_KEY;
        }
        if (field_index < next_min) {
            return FEB_CBOR_ERR_OUT_OF_ORDER;
        }

        {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_SESSION_PROOF_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_SESSION_PROOF_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->proof, data, FEB_SESSION_PROOF_LEN);
            pos += n;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0]) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    return FEB_CBOR_OK;
}

size_t feb_session_encode_transcript(uint8_t *out, size_t out_cap, const feb_session_transcript_t *s)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || s == NULL) {
        return 0;
    }
    if (s->board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 5);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", KLEN("version"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, s->version);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", KLEN("board_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, s->board_id, s->board_id_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", KLEN("session_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, s->session_id, FEB_SESSION_ID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "client_nonce", KLEN("client_nonce"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, s->client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "device_nonce", KLEN("device_nonce"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, s->device_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

static void feb_session_hmac_label(
    const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN],
    const char *label, size_t label_len,
    const uint8_t *s, size_t s_len,
    uint8_t out[FEB_HMAC_SHA256_LEN])
{
    uint8_t buf[FEB_SESSION_HMAC_BUF_LEN];

    if (label_len > FEB_SESSION_LABEL_MAX_LEN || s_len > FEB_SESSION_MAX_TRANSCRIPT_LEN) {
        memset(out, 0, FEB_HMAC_SHA256_LEN);
        return;
    }
    memcpy(buf, label, label_len);
    memcpy(buf + label_len, s, s_len);
    feb_hmac_sha256(pairing_secret, FEB_PAIRING_SECRET_LEN, buf, label_len + s_len, out);
}

void feb_session_flipper_proof(const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN], const uint8_t *s, size_t s_len, uint8_t out[FEB_SESSION_PROOF_LEN])
{
    static const char label[] = "runtime-flipper";
    uint8_t full[FEB_HMAC_SHA256_LEN];

    feb_session_hmac_label(pairing_secret, label, KLEN(label), s, s_len, full);
    memcpy(out, full, FEB_SESSION_PROOF_LEN);
}

void feb_session_esp32_proof(const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN], const uint8_t *s, size_t s_len, uint8_t out[FEB_SESSION_PROOF_LEN])
{
    static const char label[] = "runtime-esp32";
    uint8_t full[FEB_HMAC_SHA256_LEN];

    feb_session_hmac_label(pairing_secret, label, KLEN(label), s, s_len, full);
    memcpy(out, full, FEB_SESSION_PROOF_LEN);
}

void feb_session_derive_key(
    const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN],
    const uint8_t client_nonce[FEB_SESSION_NONCE_FIELD_LEN],
    const uint8_t device_nonce[FEB_SESSION_NONCE_FIELD_LEN],
    const char *board_id, size_t board_id_len,
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    uint8_t out[FEB_SESSION_KEY_LEN])
{
    static const char info_prefix[] = "flipper-esp32-over-ble/v2/aes-256-gcm";
    uint8_t salt[FEB_SESSION_NONCE_FIELD_LEN + FEB_SESSION_NONCE_FIELD_LEN];
    uint8_t info[KLEN(info_prefix) + FEB_PAIRING_BOARD_ID_MAX_LEN + FEB_SESSION_ID_LEN];
    size_t info_len;

    if (board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        memset(out, 0, FEB_SESSION_KEY_LEN);
        return;
    }

    memcpy(salt, client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(salt + FEB_SESSION_NONCE_FIELD_LEN, device_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    memcpy(info, info_prefix, KLEN(info_prefix));
    memcpy(info + KLEN(info_prefix), board_id, board_id_len);
    memcpy(info + KLEN(info_prefix) + board_id_len, session_id, FEB_SESSION_ID_LEN);
    info_len = KLEN(info_prefix) + board_id_len + FEB_SESSION_ID_LEN;

    if (feb_hkdf_sha256(salt, sizeof(salt), pairing_secret, FEB_PAIRING_SECRET_LEN, info, info_len,
                         out, FEB_SESSION_KEY_LEN) != 0) {
        memset(out, 0, FEB_SESSION_KEY_LEN);
    }
}

size_t feb_session_encode_aad(uint8_t *out, size_t out_cap, const feb_session_aad_t *aad)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || aad == NULL) {
        return 0;
    }
    if (aad->board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN || aad->type_len > FEB_CBOR_MAX_TEXT_LEN) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 5);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", KLEN("version"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, aad->version);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", KLEN("type"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, aad->type, aad->type_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", KLEN("session_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, aad->session_id, FEB_SESSION_ID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "sequence", KLEN("sequence"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, aad->sequence);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", KLEN("board_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, aad->board_id, aad->board_id_len);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

void feb_session_build_nonce(
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    uint8_t direction,
    uint64_t sequence,
    uint8_t out[FEB_SESSION_NONCE_LEN])
{
    memcpy(out, session_id, FEB_SESSION_ID_LEN);
    out[FEB_SESSION_ID_LEN] = direction;
    out[FEB_SESSION_ID_LEN + 1] = (uint8_t)((sequence >> 16) & 0xFFu);
    out[FEB_SESSION_ID_LEN + 2] = (uint8_t)((sequence >> 8) & 0xFFu);
    out[FEB_SESSION_ID_LEN + 3] = (uint8_t)(sequence & 0xFFu);
}

size_t feb_session_encrypt_record(
    const uint8_t session_key[FEB_SESSION_KEY_LEN],
    uint32_t version,
    const char *type, size_t type_len,
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    const char *board_id, size_t board_id_len,
    uint8_t direction,
    uint64_t sequence,
    const uint8_t *payload, size_t payload_len,
    uint8_t *ciphertext_scratch, size_t ciphertext_scratch_cap,
    uint8_t *out, size_t out_cap)
{
    feb_session_aad_t aad_fields;
    uint8_t aad_buf[FEB_SESSION_MAX_AAD_LEN];
    size_t aad_len;
    uint8_t nonce[FEB_SESSION_NONCE_LEN];
    uint8_t tag[FEB_SESSION_GCM_TAG_LEN];
    feb_protected_record_t record;

    if (session_key == NULL || type == NULL || session_id == NULL || board_id == NULL ||
        out == NULL) {
        return 0;
    }
    if (payload_len > FEB_CBOR_MAX_PAYLOAD) {
        return 0;
    }
    if (payload_len > 0 && (payload == NULL || ciphertext_scratch == NULL ||
                            ciphertext_scratch_cap < payload_len)) {
        return 0;
    }
    if (board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN || type_len > FEB_CBOR_MAX_TEXT_LEN) {
        return 0;
    }

    aad_fields.version = version;
    aad_fields.type = type;
    aad_fields.type_len = type_len;
    memcpy(aad_fields.session_id, session_id, FEB_SESSION_ID_LEN);
    aad_fields.sequence = sequence;
    aad_fields.board_id = board_id;
    aad_fields.board_id_len = board_id_len;

    aad_len = feb_session_encode_aad(aad_buf, sizeof(aad_buf), &aad_fields);
    if (aad_len == 0) {
        return 0;
    }

    feb_session_build_nonce(session_id, direction, sequence, nonce);

    feb_gcm_encrypt(session_key, nonce, aad_buf, aad_len, payload, payload_len,
                    ciphertext_scratch, tag);

    record.version = version;
    record.type = type;
    record.type_len = type_len;
    memcpy(record.session_id, session_id, FEB_SESSION_ID_LEN);
    record.board_id = board_id;
    record.board_id_len = board_id_len;
    record.sequence = sequence;
    record.ciphertext = ciphertext_scratch;
    record.ciphertext_len = payload_len;
    memcpy(record.tag, tag, FEB_SESSION_GCM_TAG_LEN);

    return feb_cbor_encode_protected(out, out_cap, &record);
}

feb_cbor_status_t feb_session_decrypt_record(
    const uint8_t session_key[FEB_SESSION_KEY_LEN],
    const uint8_t *in, size_t in_len,
    uint8_t direction,
    uint8_t *plaintext_out, size_t plaintext_out_cap,
    feb_session_decrypted_record_t *record)
{
    feb_protected_record_t decoded;
    feb_cbor_status_t status;
    feb_session_aad_t aad_fields;
    uint8_t aad_buf[FEB_SESSION_MAX_AAD_LEN];
    size_t aad_len;
    uint8_t nonce[FEB_SESSION_NONCE_LEN];

    if (session_key == NULL || in == NULL || plaintext_out == NULL || record == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    status = feb_cbor_decode_protected(in, in_len, &decoded);
    if (status != FEB_CBOR_OK) {
        return status;
    }
    if (decoded.ciphertext_len > plaintext_out_cap) {
        return FEB_CBOR_ERR_TOO_LARGE;
    }

    aad_fields.version = decoded.version;
    aad_fields.type = decoded.type;
    aad_fields.type_len = decoded.type_len;
    memcpy(aad_fields.session_id, decoded.session_id, FEB_SESSION_ID_LEN);
    aad_fields.sequence = decoded.sequence;
    aad_fields.board_id = decoded.board_id;
    aad_fields.board_id_len = decoded.board_id_len;

    aad_len = feb_session_encode_aad(aad_buf, sizeof(aad_buf), &aad_fields);
    if (aad_len == 0) {
        return FEB_CBOR_ERR_TOO_LARGE;
    }

    feb_session_build_nonce(decoded.session_id, direction, decoded.sequence, nonce);

    if (!feb_gcm_decrypt(session_key, nonce, aad_buf, aad_len,
                         decoded.ciphertext, decoded.ciphertext_len,
                         decoded.tag, plaintext_out)) {
        return FEB_CBOR_ERR_AUTH_FAILED;
    }

    record->version = decoded.version;
    record->type = decoded.type;
    record->type_len = decoded.type_len;
    memcpy(record->session_id, decoded.session_id, FEB_SESSION_ID_LEN);
    record->board_id = decoded.board_id;
    record->board_id_len = decoded.board_id_len;
    record->sequence = decoded.sequence;
    record->plaintext = plaintext_out;
    record->plaintext_len = decoded.ciphertext_len;

    return FEB_CBOR_OK;
}

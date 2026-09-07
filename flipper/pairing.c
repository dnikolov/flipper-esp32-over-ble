/* Implements flipper/pairing.h (frozen contract, do not change the header). Field order,
   error handling, and helper style deliberately mirror flipper/cbor_codec.c (map-header
   count checks, canonical field-order enforcement via decode_expected_key, duplicate-key
   rejection, exact-length checks on fixed-size byte fields) -- see that file for the
   established pattern this one follows. Per docs/PLAN.md step 5, framing.c/cbor_codec.c
   are not shared with pairing.c's ESP32 counterpart beyond the frozen .h contracts, so the
   small helpers below (text_matches/decode_expected_key) are intentionally duplicated
   here rather than exposed from cbor_codec.c's private surface. */
#include "pairing.h"

#include <string.h>

/* ---- shared field-order helpers (mirrors cbor_codec.c's private helpers) ---- */

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

/* Decodes a fixed-length byte field, rejecting any decoded length that does not exactly
   match `field_len` (docs/PLAN.md step 5 task: pairing_epoch/nonces/public keys/
   confirmation tags must reject a mismatched length rather than silently truncate or
   zero-pad). */
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

/* ---- error code strings ---- */

const char* feb_pairing_error_code_str(feb_pairing_error_t err) {
    switch(err) {
    case FEB_PAIRING_ERR_DISABLED:
        return "pairing_disabled";
    case FEB_PAIRING_ERR_EXPIRED:
        return "pairing_expired";
    case FEB_PAIRING_ERR_FAILED:
        return "pairing_failed";
    default:
        return "pairing_failed";
    }
}

/* ---- pairing record wire envelope ---- */

size_t feb_cbor_encode_pairing_envelope(
    uint8_t* out,
    size_t out_cap,
    const feb_pairing_envelope_t* record) {
    if(out == NULL || record == NULL) {
        return 0;
    }
    if(record->board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 4);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", sizeof("version") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", sizeof("type") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", sizeof("board_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", sizeof("payload") - 1);
    if(n == 0) return 0;
    pos += n;
    if(out_cap - pos < record->payload_span_len) {
        return 0;
    }
    if(record->payload_span_len > 0) {
        memcpy(out + pos, record->payload_span, record->payload_span_len);
    }
    pos += record->payload_span_len;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_pairing_envelope(const uint8_t* in, size_t in_len, feb_pairing_envelope_t* record) {
    if(in == NULL || record == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 4) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 4) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[4];
    size_t seen_lens[4];
    size_t n;

    n = decode_expected_key(in + pos, in_len - pos, "version", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    {
        uint64_t v;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &v, &status);
        if(n == 0) return status;
        record->version = (uint32_t)v;
        pos += n;
    }

    n = decode_expected_key(in + pos, in_len - pos, "type", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &record->type, &record->type_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "board_id", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos,
        in_len - pos,
        &record->board_id,
        &record->board_id_len,
        FEB_PAIRING_BOARD_ID_MAX_LEN,
        &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(in + pos, in_len - pos, "payload", seen_ptrs, seen_lens, 3, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_skip_value(
        in + pos, in_len - pos, 2, &record->payload_span, &record->payload_span_len, &status);
    if(n == 0) return status;
    pos += n;

    if(pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    return FEB_CBOR_OK;
}

/* ---- pair_init ---- */

size_t feb_cbor_encode_pair_init_payload(uint8_t* out, size_t out_cap, const feb_pair_init_payload_t* p) {
    if(out == NULL || p == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 3);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "pairing_epoch", sizeof("pairing_epoch") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "device_nonce", sizeof("device_nonce") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->device_nonce, FEB_PAIRING_NONCE_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "esp32_public_key", sizeof("esp32_public_key") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->esp32_public_key, FEB_PAIRING_PUBKEY_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_pair_init_payload(const uint8_t* in, size_t in_len, feb_pair_init_payload_t* p) {
    if(in == NULL || p == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 3) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = decode_expected_key(
        in + pos, in_len - pos, "pairing_epoch", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(in + pos, in_len - pos, p->pairing_epoch, FEB_PAIRING_EPOCH_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(
        in + pos, in_len - pos, "device_nonce", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(in + pos, in_len - pos, p->device_nonce, FEB_PAIRING_NONCE_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(
        in + pos, in_len - pos, "esp32_public_key", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(
        in + pos, in_len - pos, p->esp32_public_key, FEB_PAIRING_PUBKEY_LEN, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

/* ---- pair_reply ---- */

size_t
    feb_cbor_encode_pair_reply_payload(uint8_t* out, size_t out_cap, const feb_pair_reply_payload_t* p) {
    if(out == NULL || p == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 3);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "client_nonce", sizeof("client_nonce") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->client_nonce, FEB_PAIRING_NONCE_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "flipper_public_key", sizeof("flipper_public_key") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(
        out + pos, out_cap - pos, p->flipper_public_key, FEB_PAIRING_PUBKEY_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "confirmation", sizeof("confirmation") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(
        out + pos, out_cap - pos, p->confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_pair_reply_payload(const uint8_t* in, size_t in_len, feb_pair_reply_payload_t* p) {
    if(in == NULL || p == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 3) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = decode_expected_key(
        in + pos, in_len - pos, "client_nonce", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(in + pos, in_len - pos, p->client_nonce, FEB_PAIRING_NONCE_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(
        in + pos, in_len - pos, "flipper_public_key", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(
        in + pos, in_len - pos, p->flipper_public_key, FEB_PAIRING_PUBKEY_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = decode_expected_key(
        in + pos, in_len - pos, "confirmation", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(
        in + pos, in_len - pos, p->confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

/* ---- pair_confirm ---- */

size_t feb_cbor_encode_pair_confirm_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_pair_confirm_payload_t* p) {
    if(out == NULL || p == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "confirmation", sizeof("confirmation") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(
        out + pos, out_cap - pos, p->confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t feb_cbor_decode_pair_confirm_payload(
    const uint8_t* in,
    size_t in_len,
    feb_pair_confirm_payload_t* p) {
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

    n = decode_expected_key(
        in + pos, in_len - pos, "confirmation", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(
        in + pos, in_len - pos, p->confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

/* ---- pair_complete ---- */

size_t feb_cbor_encode_pair_complete_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_pair_complete_payload_t* p) {
    if(out == NULL || p == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "confirmation", sizeof("confirmation") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(
        out + pos, out_cap - pos, p->confirmation, FEB_PAIRING_COMPLETE_TAG_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t feb_cbor_decode_pair_complete_payload(
    const uint8_t* in,
    size_t in_len,
    feb_pair_complete_payload_t* p) {
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

    n = decode_expected_key(
        in + pos, in_len - pos, "confirmation", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = decode_fixed_bytes(
        in + pos, in_len - pos, p->confirmation, FEB_PAIRING_COMPLETE_TAG_LEN, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

/* ---- transcript T ---- */

size_t feb_pairing_encode_transcript(uint8_t* out, size_t out_cap, const feb_pairing_transcript_t* t) {
    if(out == NULL || t == NULL) {
        return 0;
    }
    if(t->board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 8);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", sizeof("version") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, t->version);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "service_uuid", sizeof("service_uuid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(
        out + pos, out_cap - pos, t->service_uuid, FEB_PAIRING_SERVICE_UUID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", sizeof("board_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, t->board_id, t->board_id_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "pairing_epoch", sizeof("pairing_epoch") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "client_nonce", sizeof("client_nonce") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->client_nonce, FEB_PAIRING_NONCE_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "device_nonce", sizeof("device_nonce") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->device_nonce, FEB_PAIRING_NONCE_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "esp32_public_key", sizeof("esp32_public_key") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->esp32_public_key, FEB_PAIRING_PUBKEY_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "flipper_public_key", sizeof("flipper_public_key") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(
        out + pos, out_cap - pos, t->flipper_public_key, FEB_PAIRING_PUBKEY_LEN);
    if(n == 0) return 0;
    pos += n;

    return pos;
}

/* ---- key derivation pipeline ---- */

void feb_pairing_derive_kconfirm(
    const uint8_t k_shared[FEB_PAIRING_KSHARED_LEN],
    const uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN],
    uint8_t out[FEB_PAIRING_KCONFIRM_LEN]) {
    static const char info[] = "flipper-esp32-over-ble/v2/pair-confirm";
    (void)feb_hkdf_sha256(
        pairing_epoch,
        FEB_PAIRING_EPOCH_LEN,
        k_shared,
        FEB_PAIRING_KSHARED_LEN,
        (const uint8_t*)info,
        sizeof(info) - 1,
        out,
        FEB_PAIRING_KCONFIRM_LEN);
}

void feb_pairing_derive_secret(
    const uint8_t k_shared[FEB_PAIRING_KSHARED_LEN],
    const uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN],
    const uint8_t client_nonce[FEB_PAIRING_NONCE_LEN],
    const uint8_t device_nonce[FEB_PAIRING_NONCE_LEN],
    const char* board_id,
    size_t board_id_len,
    uint8_t out[FEB_PAIRING_SECRET_LEN]) {
    static const char info_prefix[] = "flipper-esp32-over-ble/v2/x25519-pairing-secret";
    /* static, not stack-local: reachable from handle_pair_init on the Flipper's 1280-byte
       BleEventWorker thread (docs/SESSION_MEMORY.md's stack-overflow root cause). Safe as
       static: single-in-flight BLE event dispatch, not reentrant/recursive; `salt` is
       fully written by three memcpy()s below before use, `info` is fully written up to
       `info_len` before being read (feb_hkdf_sha256() is only ever passed that same
       `info_len`), and both are zeroized below on every call. */
    static uint8_t salt[FEB_PAIRING_EPOCH_LEN + FEB_PAIRING_NONCE_LEN + FEB_PAIRING_NONCE_LEN];
    static uint8_t info[sizeof(info_prefix) - 1 + FEB_PAIRING_BOARD_ID_MAX_LEN];
    size_t info_len;

    memcpy(salt, pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    memcpy(salt + FEB_PAIRING_EPOCH_LEN, client_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(
        salt + FEB_PAIRING_EPOCH_LEN + FEB_PAIRING_NONCE_LEN,
        device_nonce,
        FEB_PAIRING_NONCE_LEN);

    memcpy(info, info_prefix, sizeof(info_prefix) - 1);
    info_len = sizeof(info_prefix) - 1;
    /* Over-length board_id yields an all-zero pairing secret with no error signal (void
       return), matching the ESP32's behavior: an all-zero secret fails proof/confirmation
       HMAC checks immediately and visibly, whereas silently truncating board_id would
       derive a real but wrong secret that presents as an unexplained confirmation
       mismatch. Callers must uphold board_id_len <= FEB_PAIRING_BOARD_ID_MAX_LEN (enforced
       by feb_cbor_decode_pairing_envelope() and this module's own encoders); this is
       defense-in-depth for a shared primitive, not the primary enforcement point. */
    if(board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        feb_secure_zero(out, FEB_PAIRING_SECRET_LEN);
        feb_secure_zero(salt, sizeof(salt));
        feb_secure_zero(info, sizeof(info));
        return;
    }
    if(board_id_len > 0) {
        memcpy(info + info_len, board_id, board_id_len);
    }
    info_len += board_id_len;

    (void)feb_hkdf_sha256(
        salt,
        sizeof(salt),
        k_shared,
        FEB_PAIRING_KSHARED_LEN,
        info,
        info_len,
        out,
        FEB_PAIRING_SECRET_LEN);

    feb_secure_zero(salt, sizeof(salt));
    feb_secure_zero(info, sizeof(info));
}

#define FEB_PAIRING_LABEL_MAX_LEN 16u

/* Computes HMAC-SHA-256(k_confirm, label || t), truncated to out_len bytes. `t` is T as
   produced by feb_pairing_encode_transcript(), so label_len + t_len is always within this
   function's fixed-size local buffer given FEB_PAIRING_LABEL_MAX_LEN and
   FEB_PAIRING_MAX_TRANSCRIPT_LEN; both are clamped defensively regardless. */
static void pairing_confirm_tag(
    const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN],
    const char* label,
    size_t label_len,
    const uint8_t* t,
    size_t t_len,
    uint8_t* out,
    size_t out_len) {
    /* static, not stack-local: sized off FEB_PAIRING_MAX_TRANSCRIPT_LEN (304 bytes for
       `buf` alone), reachable from handle_pair_init/confirm/complete on the Flipper's
       1280-byte BleEventWorker thread (docs/SESSION_MEMORY.md's stack-overflow root
       cause). Safe as static: single-in-flight BLE event dispatch, not
       reentrant/recursive; `buf` is fully written up to `total` bytes before being read
       (feb_hmac_sha256() is only ever passed that same `total` length) and zeroized below
       on every call regardless of path. */
    static uint8_t buf[FEB_PAIRING_LABEL_MAX_LEN + FEB_PAIRING_MAX_TRANSCRIPT_LEN];
    static uint8_t mac[FEB_HMAC_SHA256_LEN];
    size_t total;

    if(label_len > FEB_PAIRING_LABEL_MAX_LEN) {
        label_len = FEB_PAIRING_LABEL_MAX_LEN;
    }
    if(t_len > FEB_PAIRING_MAX_TRANSCRIPT_LEN) {
        t_len = FEB_PAIRING_MAX_TRANSCRIPT_LEN;
    }

    memcpy(buf, label, label_len);
    memcpy(buf + label_len, t, t_len);
    total = label_len + t_len;

    feb_hmac_sha256(k_confirm, FEB_PAIRING_KCONFIRM_LEN, buf, total, mac);
    memcpy(out, mac, out_len);

    feb_secure_zero(buf, sizeof(buf));
    feb_secure_zero(mac, sizeof(mac));
}

void feb_pairing_flipper_confirm(
    const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN],
    const uint8_t* t,
    size_t t_len,
    uint8_t out[FEB_PAIRING_REPLY_CONFIRM_LEN]) {
    static const char label[] = "flipper-confirm";
    pairing_confirm_tag(
        k_confirm, label, sizeof(label) - 1, t, t_len, out, FEB_PAIRING_REPLY_CONFIRM_LEN);
}

void feb_pairing_esp32_confirm(
    const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN],
    const uint8_t* t,
    size_t t_len,
    uint8_t out[FEB_PAIRING_REPLY_CONFIRM_LEN]) {
    static const char label[] = "esp32-confirm";
    pairing_confirm_tag(
        k_confirm, label, sizeof(label) - 1, t, t_len, out, FEB_PAIRING_REPLY_CONFIRM_LEN);
}

void feb_pairing_complete_tag(
    const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN],
    const uint8_t* t,
    size_t t_len,
    uint8_t out[FEB_PAIRING_COMPLETE_TAG_LEN]) {
    static const char label[] = "complete";
    pairing_confirm_tag(
        k_confirm, label, sizeof(label) - 1, t, t_len, out, FEB_PAIRING_COMPLETE_TAG_LEN);
}

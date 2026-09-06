#include "pairing.h"

#include <string.h>

#define KLEN(literal) (sizeof(literal) - 1u)

/* Longest label used by feb_pairing_hmac_label() is "flipper-confirm" (15
   bytes); rounded up for margin. */
#define FEB_PAIRING_LABEL_MAX_LEN 16u
#define FEB_PAIRING_HMAC_BUF_LEN (FEB_PAIRING_LABEL_MAX_LEN + FEB_PAIRING_MAX_TRANSCRIPT_LEN)

const char *feb_pairing_error_code_str(feb_pairing_error_t err)
{
    switch (err) {
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

size_t feb_cbor_encode_pairing_envelope(uint8_t *out, size_t out_cap, const feb_pairing_envelope_t *record)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || record == NULL) {
        return 0;
    }
    if (record->board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 4);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", KLEN("version"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", KLEN("type"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", KLEN("board_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "payload", KLEN("payload"));
    if (n == 0) return 0;
    pos += n;
    if (out_cap - pos < record->payload_span_len) {
        return 0;
    }
    memcpy(out + pos, record->payload_span, record->payload_span_len);
    pos += record->payload_span_len;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_pairing_envelope(const uint8_t *in, size_t in_len, feb_pairing_envelope_t *record)
{
    static const char *const names[4] = {"version", "type", "board_id", "payload"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[4] = {0, 0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || record == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 4) {
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
        for (j = 0; j < 4; j++) {
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
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);

            if (n == 0) return status;
            record->version = (uint32_t)value;
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            record->type = data;
            record->type_len = len;
            pos += n;
            break;
        }
        case 2: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);

            if (n == 0) return status;
            if (len > FEB_PAIRING_BOARD_ID_MAX_LEN) return FEB_CBOR_ERR_TOO_LARGE;
            record->board_id = data;
            record->board_id_len = len;
            pos += n;
            break;
        }
        case 3: {
            const uint8_t *span;
            size_t span_len;
            size_t n = feb_cbor_skip_value(in + pos, in_len - pos, 2, &span, &span_len, &status);

            if (n == 0) return status;
            record->payload_span = span;
            record->payload_span_len = span_len;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 4; i++) {
        if (!seen[i]) {
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_pair_init_payload(uint8_t *out, size_t out_cap, const feb_pair_init_payload_t *p)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || p == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 3);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "pairing_epoch", KLEN("pairing_epoch"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "device_nonce", KLEN("device_nonce"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->device_nonce, FEB_PAIRING_NONCE_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "esp32_public_key", KLEN("esp32_public_key"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->esp32_public_key, FEB_PAIRING_PUBKEY_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_pair_init_payload(const uint8_t *in, size_t in_len, feb_pair_init_payload_t *p)
{
    static const char *const names[3] = {"pairing_epoch", "device_nonce", "esp32_public_key"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[3] = {0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || p == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 3) {
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
        for (j = 0; j < 3; j++) {
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
                                              FEB_PAIRING_EPOCH_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_PAIRING_EPOCH_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->pairing_epoch, data, FEB_PAIRING_EPOCH_LEN);
            pos += n;
            break;
        }
        case 1: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_PAIRING_NONCE_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_PAIRING_NONCE_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->device_nonce, data, FEB_PAIRING_NONCE_LEN);
            pos += n;
            break;
        }
        case 2: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_PAIRING_PUBKEY_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_PAIRING_PUBKEY_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->esp32_public_key, data, FEB_PAIRING_PUBKEY_LEN);
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 3; i++) {
        if (!seen[i]) {
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_pair_reply_payload(uint8_t *out, size_t out_cap, const feb_pair_reply_payload_t *p)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || p == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 3);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "client_nonce", KLEN("client_nonce"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->client_nonce, FEB_PAIRING_NONCE_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "flipper_public_key", KLEN("flipper_public_key"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->flipper_public_key, FEB_PAIRING_PUBKEY_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "confirmation", KLEN("confirmation"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_pair_reply_payload(const uint8_t *in, size_t in_len, feb_pair_reply_payload_t *p)
{
    static const char *const names[3] = {"client_nonce", "flipper_public_key", "confirmation"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[3] = {0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || p == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 3) {
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
        for (j = 0; j < 3; j++) {
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
                                              FEB_PAIRING_NONCE_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_PAIRING_NONCE_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->client_nonce, data, FEB_PAIRING_NONCE_LEN);
            pos += n;
            break;
        }
        case 1: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_PAIRING_PUBKEY_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_PAIRING_PUBKEY_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->flipper_public_key, data, FEB_PAIRING_PUBKEY_LEN);
            pos += n;
            break;
        }
        case 2: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_PAIRING_REPLY_CONFIRM_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_PAIRING_REPLY_CONFIRM_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->confirmation, data, FEB_PAIRING_REPLY_CONFIRM_LEN);
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 3; i++) {
        if (!seen[i]) {
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_pair_confirm_payload(uint8_t *out, size_t out_cap, const feb_pair_confirm_payload_t *p)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || p == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "confirmation", KLEN("confirmation"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_pair_confirm_payload(const uint8_t *in, size_t in_len, feb_pair_confirm_payload_t *p)
{
    static const char *const names[1] = {"confirmation"};
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
                                              FEB_PAIRING_REPLY_CONFIRM_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_PAIRING_REPLY_CONFIRM_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->confirmation, data, FEB_PAIRING_REPLY_CONFIRM_LEN);
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

size_t feb_cbor_encode_pair_complete_payload(uint8_t *out, size_t out_cap, const feb_pair_complete_payload_t *p)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || p == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "confirmation", KLEN("confirmation"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, p->confirmation, FEB_PAIRING_COMPLETE_TAG_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_pair_complete_payload(const uint8_t *in, size_t in_len, feb_pair_complete_payload_t *p)
{
    static const char *const names[1] = {"confirmation"};
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
                                              FEB_PAIRING_COMPLETE_TAG_LEN, &status);

            if (n == 0) return status;
            if (len != FEB_PAIRING_COMPLETE_TAG_LEN) return FEB_CBOR_ERR_UNEXPECTED_TYPE;
            memcpy(p->confirmation, data, FEB_PAIRING_COMPLETE_TAG_LEN);
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

size_t feb_pairing_encode_transcript(uint8_t *out, size_t out_cap, const feb_pairing_transcript_t *t)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || t == NULL) {
        return 0;
    }
    if (t->board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 8);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "version", KLEN("version"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, t->version);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "service_uuid", KLEN("service_uuid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->service_uuid, FEB_PAIRING_SERVICE_UUID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", KLEN("board_id"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, t->board_id, t->board_id_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "pairing_epoch", KLEN("pairing_epoch"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "client_nonce", KLEN("client_nonce"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->client_nonce, FEB_PAIRING_NONCE_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "device_nonce", KLEN("device_nonce"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->device_nonce, FEB_PAIRING_NONCE_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "esp32_public_key", KLEN("esp32_public_key"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->esp32_public_key, FEB_PAIRING_PUBKEY_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "flipper_public_key", KLEN("flipper_public_key"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, t->flipper_public_key, FEB_PAIRING_PUBKEY_LEN);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

void feb_pairing_derive_kconfirm(
    const uint8_t k_shared[FEB_PAIRING_KSHARED_LEN],
    const uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN],
    uint8_t out[FEB_PAIRING_KCONFIRM_LEN])
{
    static const char info[] = "flipper-esp32-over-ble/v2/pair-confirm";

    if (feb_hkdf_sha256(pairing_epoch, FEB_PAIRING_EPOCH_LEN, k_shared, FEB_PAIRING_KSHARED_LEN,
                         (const uint8_t *)info, KLEN(info), out, FEB_PAIRING_KCONFIRM_LEN) != 0) {
        memset(out, 0, FEB_PAIRING_KCONFIRM_LEN);
    }
}

void feb_pairing_derive_secret(
    const uint8_t k_shared[FEB_PAIRING_KSHARED_LEN],
    const uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN],
    const uint8_t client_nonce[FEB_PAIRING_NONCE_LEN],
    const uint8_t device_nonce[FEB_PAIRING_NONCE_LEN],
    const char *board_id, size_t board_id_len,
    uint8_t out[FEB_PAIRING_SECRET_LEN])
{
    static const char info_prefix[] = "flipper-esp32-over-ble/v2/x25519-pairing-secret";
    uint8_t salt[FEB_PAIRING_EPOCH_LEN + FEB_PAIRING_NONCE_LEN + FEB_PAIRING_NONCE_LEN];
    uint8_t info[KLEN(info_prefix) + FEB_PAIRING_BOARD_ID_MAX_LEN];
    size_t info_len;

    if (board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN) {
        memset(out, 0, FEB_PAIRING_SECRET_LEN);
        return;
    }

    memcpy(salt, pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    memcpy(salt + FEB_PAIRING_EPOCH_LEN, client_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(salt + FEB_PAIRING_EPOCH_LEN + FEB_PAIRING_NONCE_LEN, device_nonce, FEB_PAIRING_NONCE_LEN);

    memcpy(info, info_prefix, KLEN(info_prefix));
    memcpy(info + KLEN(info_prefix), board_id, board_id_len);
    info_len = KLEN(info_prefix) + board_id_len;

    if (feb_hkdf_sha256(salt, sizeof(salt), k_shared, FEB_PAIRING_KSHARED_LEN, info, info_len,
                         out, FEB_PAIRING_SECRET_LEN) != 0) {
        memset(out, 0, FEB_PAIRING_SECRET_LEN);
    }
}

static void feb_pairing_hmac_label(
    const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN],
    const char *label, size_t label_len,
    const uint8_t *t, size_t t_len,
    uint8_t out[FEB_HMAC_SHA256_LEN])
{
    uint8_t buf[FEB_PAIRING_HMAC_BUF_LEN];

    if (label_len > FEB_PAIRING_LABEL_MAX_LEN || t_len > FEB_PAIRING_MAX_TRANSCRIPT_LEN) {
        memset(out, 0, FEB_HMAC_SHA256_LEN);
        return;
    }
    memcpy(buf, label, label_len);
    memcpy(buf + label_len, t, t_len);
    feb_hmac_sha256(k_confirm, FEB_PAIRING_KCONFIRM_LEN, buf, label_len + t_len, out);
}

void feb_pairing_flipper_confirm(const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN], const uint8_t *t, size_t t_len, uint8_t out[FEB_PAIRING_REPLY_CONFIRM_LEN])
{
    static const char label[] = "flipper-confirm";
    uint8_t full[FEB_HMAC_SHA256_LEN];

    feb_pairing_hmac_label(k_confirm, label, KLEN(label), t, t_len, full);
    memcpy(out, full, FEB_PAIRING_REPLY_CONFIRM_LEN);
}

void feb_pairing_esp32_confirm(const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN], const uint8_t *t, size_t t_len, uint8_t out[FEB_PAIRING_REPLY_CONFIRM_LEN])
{
    static const char label[] = "esp32-confirm";
    uint8_t full[FEB_HMAC_SHA256_LEN];

    feb_pairing_hmac_label(k_confirm, label, KLEN(label), t, t_len, full);
    memcpy(out, full, FEB_PAIRING_REPLY_CONFIRM_LEN);
}

void feb_pairing_complete_tag(const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN], const uint8_t *t, size_t t_len, uint8_t out[FEB_PAIRING_COMPLETE_TAG_LEN])
{
    static const char label[] = "complete";
    uint8_t full[FEB_HMAC_SHA256_LEN];

    feb_pairing_hmac_label(k_confirm, label, KLEN(label), t, t_len, full);
    memcpy(out, full, FEB_PAIRING_COMPLETE_TAG_LEN);
}

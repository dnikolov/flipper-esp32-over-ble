#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

/* ---- Envelope shapes ---- */

size_t feb_cbor_encode_unencrypted(
    uint8_t* out,
    size_t out_cap,
    const feb_unencrypted_record_t* record) {
    if(out == NULL || record == NULL) {
        return 0;
    }
    if(record->payload_span_len > FEB_CBOR_MAX_PAYLOAD) {
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
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->version);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "type", sizeof("type") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->type, record->type_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", sizeof("session_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->session_id, FEB_SESSION_ID_LEN);
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

size_t
    feb_cbor_encode_protected(uint8_t* out, size_t out_cap, const feb_protected_record_t* record) {
    if(out == NULL || record == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;

    n = feb_cbor_encode_map_header(out, out_cap, 7);
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
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "session_id", sizeof("session_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->session_id, FEB_SESSION_ID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board_id", sizeof("board_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, record->board_id, record->board_id_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "sequence", sizeof("sequence") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, record->sequence);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ciphertext", sizeof("ciphertext") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->ciphertext, record->ciphertext_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "tag", sizeof("tag") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, record->tag, FEB_GCM_TAG_LEN);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_unencrypted(const uint8_t* in, size_t in_len, feb_unencrypted_record_t* record) {
    if(in == NULL || record == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 5) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 5) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[5];
    size_t seen_lens[5];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "version", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    {
        uint64_t v;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &v, &status);
        if(n == 0) return status;
        record->version = (uint32_t)v;
        pos += n;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "type", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &record->type, &record->type_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "session_id", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(
            in + pos, in_len - pos, &data, &len, FEB_SESSION_ID_LEN, &status);
        if(n == 0) return status;
        if(len != FEB_SESSION_ID_LEN) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        memcpy(record->session_id, data, FEB_SESSION_ID_LEN);
        pos += n;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "board_id", seen_ptrs, seen_lens, 3, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos,
        in_len - pos,
        &record->board_id,
        &record->board_id_len,
        FEB_CBOR_MAX_TEXT_LEN,
        &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "payload", seen_ptrs, seen_lens, 4, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_skip_value(
        in + pos, in_len - pos, 2, &record->payload_span, &record->payload_span_len, &status);
    if(n == 0) return status;
    if(record->payload_span_len > FEB_CBOR_MAX_PAYLOAD) {
        return FEB_CBOR_ERR_TOO_LARGE;
    }
    pos += n;

    if(pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    return FEB_CBOR_OK;
}

feb_cbor_status_t
    feb_cbor_decode_protected(const uint8_t* in, size_t in_len, feb_protected_record_t* record) {
    if(in == NULL || record == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 7) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 7) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[7];
    size_t seen_lens[7];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "version", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    {
        uint64_t v;
        n = feb_cbor_decode_uint(in + pos, in_len - pos, &v, &status);
        if(n == 0) return status;
        record->version = (uint32_t)v;
        pos += n;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "type", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &record->type, &record->type_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "session_id", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(
            in + pos, in_len - pos, &data, &len, FEB_SESSION_ID_LEN, &status);
        if(n == 0) return status;
        if(len != FEB_SESSION_ID_LEN) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        memcpy(record->session_id, data, FEB_SESSION_ID_LEN);
        pos += n;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "board_id", seen_ptrs, seen_lens, 3, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos,
        in_len - pos,
        &record->board_id,
        &record->board_id_len,
        FEB_CBOR_MAX_TEXT_LEN,
        &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "sequence", seen_ptrs, seen_lens, 4, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &record->sequence, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "ciphertext", seen_ptrs, seen_lens, 5, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_bytes(
        in + pos,
        in_len - pos,
        &record->ciphertext,
        &record->ciphertext_len,
        FEB_CBOR_MAX_BYTES_LEN,
        &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "tag", seen_ptrs, seen_lens, 6, &status);
    if(n == 0) return status;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_GCM_TAG_LEN, &status);
        if(n == 0) return status;
        if(len != FEB_GCM_TAG_LEN) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        memcpy(record->tag, data, FEB_GCM_TAG_LEN);
        pos += n;
    }

    if(pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    return FEB_CBOR_OK;
}

/* ---- `error` payload ---- */

size_t feb_cbor_encode_error_payload(uint8_t* out, size_t out_cap, const feb_error_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->code == NULL) {
        return 0;
    }
    size_t count = 1;
    if(payload->has_message) count++;
    if(payload->has_request_id) count++;

    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "code", sizeof("code") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->code, payload->code_len);
    if(n == 0) return 0;
    pos += n;

    if(payload->has_message) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "message", sizeof("message") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->message, payload->message_len);
        if(n == 0) return 0;
        pos += n;
    }
    if(payload->has_request_id) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", sizeof("request_id") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_error_payload(const uint8_t* in, size_t in_len, feb_error_payload_t* payload) {
    if(in == NULL || payload == NULL) {
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
    if(count > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    memset(payload, 0, sizeof(*payload));

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "code", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->code, &payload->code_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    int seen_message = 0;
    int seen_request_id = 0;
    for(size_t i = 1; i < count; i++) {
        const char* key_data;
        size_t key_len;
        n = feb_cbor_decode_text(
            in + pos, in_len - pos, &key_data, &key_len, FEB_CBOR_MAX_TEXT_LEN, &status);
        if(n == 0) return status;
        for(size_t j = 0; j < i; j++) {
            if(seen_lens[j] == key_len && memcmp(seen_ptrs[j], key_data, key_len) == 0) {
                return FEB_CBOR_ERR_DUPLICATE_KEY;
            }
        }
        seen_ptrs[i] = (const uint8_t*)key_data;
        seen_lens[i] = key_len;
        pos += n;

        if(feb_cbor_i_text_matches(key_data, key_len, "message")) {
            if(seen_request_id) {
                return FEB_CBOR_ERR_OUT_OF_ORDER;
            }
            n = feb_cbor_decode_text(
                in + pos,
                in_len - pos,
                &payload->message,
                &payload->message_len,
                FEB_CBOR_MAX_TEXT_LEN,
                &status);
            if(n == 0) return status;
            pos += n;
            payload->has_message = 1;
            seen_message = 1;
            (void)seen_message;
        } else if(feb_cbor_i_text_matches(key_data, key_len, "request_id")) {
            n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->request_id, &status);
            if(n == 0) return status;
            pos += n;
            payload->has_request_id = 1;
            seen_request_id = 1;
        } else {
            return FEB_CBOR_ERR_OUT_OF_ORDER;
        }
    }

    return FEB_CBOR_OK;
}

/* ---- `capability_query` / `capability_response` payloads ---- */

size_t feb_cbor_encode_capability_query_payload(
    uint8_t* out, size_t out_cap, const feb_capability_query_payload_t* payload) {
    if(out == NULL || payload == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, payload->has_requested ? 1u : 0u);
    if(n == 0) return 0;
    pos += n;
    if(payload->has_requested) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "requested", sizeof("requested") - 1);
        if(n == 0) return 0;
        pos += n;
        /* Content intentionally not captured/emitted by this firmware -- see the header
           comment. This firmware never sets has_requested, so this path is unused today. */
        n = feb_cbor_encode_array_header(out + pos, out_cap - pos, 0);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_capability_query_payload(
    const uint8_t* in, size_t in_len, feb_capability_query_payload_t* payload) {
    if(in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    memset(payload, 0, sizeof(*payload));
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    if(count == 1) {
        const char* key_data;
        size_t key_len;
        size_t n =
            feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len, FEB_CBOR_MAX_TEXT_LEN, &status);
        if(n == 0) return status;
        if(!feb_cbor_i_text_matches(key_data, key_len, "requested")) {
            return FEB_CBOR_ERR_OUT_OF_ORDER;
        }
        pos += n;

        size_t array_count = 0;
        n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
        if(n == 0) return status;
        if(array_count > FEB_CBOR_MAX_ARRAY_ENTRIES) {
            return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        }
        pos += n;
        for(size_t i = 0; i < array_count; i++) {
            const char* item_data;
            size_t item_len;
            n = feb_cbor_decode_text(
                in + pos, in_len - pos, &item_data, &item_len, FEB_CBOR_MAX_TEXT_LEN, &status);
            if(n == 0) return status;
            pos += n;
        }
        payload->has_requested = 1;
    }
    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_capability_response_payload(
    uint8_t* out, size_t out_cap, const feb_capability_response_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->board == NULL || payload->firmware == NULL) {
        return 0;
    }
    if(payload->feature_count > FEB_CAPABILITY_MAX_FEATURES) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 3);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "board", sizeof("board") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->board, payload->board_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "firmware", sizeof("firmware") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->firmware, payload->firmware_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "features", sizeof("features") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->feature_count);
    if(n == 0) return 0;
    pos += n;
    for(size_t i = 0; i < payload->feature_count; i++) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->features[i], payload->feature_lens[i]);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_capability_response_payload(
    const uint8_t* in, size_t in_len, feb_capability_response_payload_t* payload) {
    if(in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    memset(payload, 0, sizeof(*payload));
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

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "board", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->board, &payload->board_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "firmware", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->firmware, &payload->firmware_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "features", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    size_t feature_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &feature_count, &status);
    if(n == 0) return status;
    if(feature_count > FEB_CAPABILITY_MAX_FEATURES) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;
    for(size_t i = 0; i < feature_count; i++) {
        n = feb_cbor_decode_text(
            in + pos,
            in_len - pos,
            &payload->features[i],
            &payload->feature_lens[i],
            FEB_CBOR_MAX_TEXT_LEN,
            &status);
        if(n == 0) return status;
        pos += n;
    }
    payload->feature_count = feature_count;

    return FEB_CBOR_OK;
}

/* ---- `command` / `status` payloads (generic; see header comment) ---- */

size_t feb_cbor_encode_command_payload(uint8_t* out, size_t out_cap, const feb_command_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->capability == NULL || payload->arguments_span == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 3);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "capability", sizeof("capability") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->capability, payload->capability_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", sizeof("request_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "arguments", sizeof("arguments") - 1);
    if(n == 0) return 0;
    pos += n;
    if(out_cap - pos < payload->arguments_span_len) {
        return 0;
    }
    if(payload->arguments_span_len > 0) {
        memcpy(out + pos, payload->arguments_span, payload->arguments_span_len);
    }
    pos += payload->arguments_span_len;
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_command_payload(const uint8_t* in, size_t in_len, feb_command_payload_t* payload) {
    if(in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    memset(payload, 0, sizeof(*payload));
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

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "capability", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->capability, &payload->capability_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "request_id", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->request_id, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "arguments", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    /* Fresh depth-0 budget, not payload_span's depth-2 convention -- per docs/PROTOCOL.md's
       "Nesting depth" section (updated for this step): a field with its own dedicated,
       schema-aware decoder (this one) is its own self-contained span and gets a fresh depth
       budget starting at 0 when recursing into feb_cbor_skip_value() for a still-generically-
       validated sub-piece, rather than inheriting the depth already spent positioning the
       outer payload. This is a decoder-internal bookkeeping convention with no wire
       representation, but both firmwares must apply it identically or one will accept a
       record the other rejects -- see the matching `result` comment in
       feb_cbor_decode_status_payload() below for the full accounting. */
    n = feb_cbor_skip_value(
        in + pos, in_len - pos, 0, &payload->arguments_span, &payload->arguments_span_len, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

size_t feb_cbor_encode_status_payload(uint8_t* out, size_t out_cap, const feb_status_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->state == NULL) {
        return 0;
    }
    if(payload->has_result && payload->result_span == NULL) {
        return 0;
    }
    size_t count = payload->has_result ? 3u : 2u;
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "request_id", sizeof("request_id") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->request_id);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "state", sizeof("state") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, payload->state, payload->state_len);
    if(n == 0) return 0;
    pos += n;
    if(payload->has_result) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "result", sizeof("result") - 1);
        if(n == 0) return 0;
        pos += n;
        if(out_cap - pos < payload->result_span_len) {
            return 0;
        }
        if(payload->result_span_len > 0) {
            memcpy(out + pos, payload->result_span, payload->result_span_len);
        }
        pos += payload->result_span_len;
    }
    return pos;
}

feb_cbor_status_t
    feb_cbor_decode_status_payload(const uint8_t* in, size_t in_len, feb_status_payload_t* payload) {
    if(in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    memset(payload, 0, sizeof(*payload));
    feb_cbor_status_t status = FEB_CBOR_OK;
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if(pos == 0) {
        return status;
    }
    if(count < 2) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 3) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[3];
    size_t seen_lens[3];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "request_id", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->request_id, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "state", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &payload->state, &payload->state_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if(n == 0) return status;
    pos += n;

    if(count == 3) {
        n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "result", seen_ptrs, seen_lens, 2, &status);
        if(n == 0) return status;
        pos += n;
        /* Fresh depth-0 budget -- per docs/PROTOCOL.md's "Nesting depth" section (updated
           for this step, after this exact gap was found independently on both firmwares
           while implementing against the frozen FEB_VEC_WIFI_SCAN_STATUS_PARTIAL/COMPLETE_
           PAYLOAD vectors): wifi_scan's actual `result` shape (result-map -> aps-array ->
           6-field ap-result-map -> scalar field) is three containers deep in its own right,
           which exceeds FEB_CBOR_MAX_NESTING if it inherits the depth already spent
           positioning `status` itself as "the payload" (payload_span's depth-2 convention,
           or a naive "one level deeper", depth-3). Since this decoder is schema-aware down
           to the <ap-result> field level (unlike a truly opaque field, e.g. `payload` itself
           or capability_query's unused `requested`), `result` is validated as its own
           self-contained span starting fresh at depth 0, not as continued generic descent
           from `status`. This is a decoder-internal bookkeeping convention with no wire
           representation, but must match the ESP32 side's decoder exactly -- confirmed
           against PROTOCOL.md's now-updated wording rather than invented independently. */
        n = feb_cbor_skip_value(
            in + pos, in_len - pos, 0, &payload->result_span, &payload->result_span_len, &status);
        if(n == 0) return status;
        pos += n;
        payload->has_result = 1;
    }

    return FEB_CBOR_OK;
}

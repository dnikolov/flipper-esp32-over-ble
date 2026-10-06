#include "feb_agps_cmd.h"

#include <string.h>

static bool expect_key(const uint8_t *in, size_t in_len, size_t *pos, const char *name)
{
    const char *text;
    size_t text_len;
    feb_cbor_status_t status;
    size_t used = feb_cbor_decode_text(in + *pos, in_len - *pos, &text, &text_len, FEB_CBOR_MAX_TEXT_LEN, &status);

    if (used == 0 || text_len != strlen(name) || memcmp(text, name, text_len) != 0) {
        return false;
    }
    *pos += used;
    return true;
}

static bool read_uint(const uint8_t *in, size_t in_len, size_t *pos, const char *key, uint64_t *value)
{
    feb_cbor_status_t status;
    size_t used;

    if (!expect_key(in, in_len, pos, key)) {
        return false;
    }
    used = feb_cbor_decode_uint(in + *pos, in_len - *pos, value, &status);
    if (used == 0) {
        return false;
    }
    *pos += used;
    return true;
}

feb_cbor_status_t feb_agps_decode_load_args(const uint8_t *in, size_t in_len, feb_agps_load_args_t *out)
{
    size_t pos = 0;
    size_t count;
    size_t used;
    feb_cbor_status_t status = FEB_CBOR_OK;
    const char *text;
    size_t text_len;
    uint64_t value;
    uint64_t lat_off;
    uint64_t lon_off;
    uint64_t alt_off;

    memset(out, 0, sizeof(*out));
    used = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (used == 0) {
        return status;
    }
    pos += used;
    if (count != 5u && count != 8u) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }

    if (!expect_key(in, in_len, &pos, "action")) {
        return FEB_CBOR_ERR_OUT_OF_ORDER;
    }
    used = feb_cbor_decode_text(in + pos, in_len - pos, &text, &text_len, FEB_CBOR_MAX_TEXT_LEN, &status);
    if (used == 0) {
        return status;
    }
    pos += used;
    if (text_len != strlen("load") || memcmp(text, "load", text_len) != 0) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }

    if (!read_uint(in, in_len, &pos, "utc", &out->utc) || out->utc < FEB_AGPS_UTC_MIN ||
        out->utc >= FEB_AGPS_UTC_MAX) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    if (!read_uint(in, in_len, &pos, "total", &value) || value > 0xFFFFu) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    out->total = (uint32_t)value;
    if (!read_uint(in, in_len, &pos, "off", &value) || value > 0xFFFFu) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    out->off = (uint32_t)value;

    if (!expect_key(in, in_len, &pos, "data")) {
        return FEB_CBOR_ERR_OUT_OF_ORDER;
    }
    used = feb_cbor_decode_bytes(in + pos, in_len - pos, &out->data, &out->data_len, FEB_CBOR_MAX_BYTES_LEN, &status);
    if (used == 0) {
        return status;
    }
    pos += used;

    if (count == 8u) {
        if (!read_uint(in, in_len, &pos, "lat_e7_offset", &lat_off) || lat_off > FEB_AGPS_LAT_E7_OFFSET_MAX ||
            !read_uint(in, in_len, &pos, "lon_e7_offset", &lon_off) || lon_off > FEB_AGPS_LON_E7_OFFSET_MAX ||
            !read_uint(in, in_len, &pos, "altitude_dm_offset", &alt_off) || alt_off > FEB_AGPS_ALT_DM_OFFSET_MAX) {
            return FEB_CBOR_ERR_UNEXPECTED_TYPE;
        }
        out->position.valid = true;
        out->position.lat_e7 = (int32_t)((int64_t)lat_off - 900000000LL);
        out->position.lon_e7 = (int32_t)((int64_t)lon_off - 1800000000LL);
        out->position.alt_dm = (int32_t)((int64_t)alt_off - FEB_AGPS_ALT_DM_OFFSET);
    }
    if (pos != in_len) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    return FEB_CBOR_OK;
}

size_t feb_agps_encode_result(uint8_t *out, size_t out_cap, const feb_agps_result_t *result)
{
    static const char *const keys[4] = {"sent", "acked", "nacked", "timeout"};
    const uint64_t values[4] = {result->sent, result->acked, result->nacked, result->timeout};
    size_t pos = 0;
    size_t used;
    size_t i;

    used = feb_cbor_encode_map_header(out, out_cap, 4);
    if (used == 0) {
        return 0;
    }
    pos += used;
    for (i = 0; i < 4; i++) {
        used = feb_cbor_encode_text(out + pos, out_cap - pos, keys[i], strlen(keys[i]));
        if (used == 0) {
            return 0;
        }
        pos += used;
        used = feb_cbor_encode_uint(out + pos, out_cap - pos, values[i]);
        if (used == 0) {
            return 0;
        }
        pos += used;
    }
    return pos;
}

#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

size_t feb_cbor_encode_gps_result_payload(uint8_t *out, size_t out_cap, const feb_gps_result_payload_t *payload)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || payload == NULL) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 7);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lat_e7_offset", FEB_CBOR_I_KLEN("lat_e7_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->lat_e7_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lon_e7_offset", FEB_CBOR_I_KLEN("lon_e7_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->lon_e7_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "fix_quality", FEB_CBOR_I_KLEN("fix_quality"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->fix_quality);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "satellites", FEB_CBOR_I_KLEN("satellites"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->satellites);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "hdop_e1", FEB_CBOR_I_KLEN("hdop_e1"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->hdop_e1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "utc_timestamp_s", FEB_CBOR_I_KLEN("utc_timestamp_s"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->utc_timestamp_s);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "altitude_dm_offset", FEB_CBOR_I_KLEN("altitude_dm_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->altitude_dm_offset);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

feb_cbor_status_t feb_cbor_decode_gps_result_payload(const uint8_t *in, size_t in_len, feb_gps_result_payload_t *payload)
{
    static const char *const names[7] = {"lat_e7_offset", "lon_e7_offset", "fix_quality",
                                          "satellites", "hdop_e1", "utc_timestamp_s",
                                          "altitude_dm_offset"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[7] = {0, 0, 0, 0, 0, 0, 0};
    feb_cbor_status_t status;
    size_t consumed;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &status);
    if (consumed == 0) {
        return status;
    }
    if (count > 7) {
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
        uint64_t value;
        size_t n;

        key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                             FEB_CBOR_MAX_TEXT_LEN, &status);
        if (key_consumed == 0) {
            return status;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 7; j++) {
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

        n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &status);
        if (n == 0) {
            return status;
        }
        pos += n;

        switch (field_index) {
        case 0: payload->lat_e7_offset = value; break;
        case 1: payload->lon_e7_offset = value; break;
        case 2: payload->fix_quality = value; break;
        case 3: payload->satellites = value; break;
        case 4: payload->hdop_e1 = value; break;
        case 5: payload->utc_timestamp_s = value; break;
        case 6: payload->altitude_dm_offset = value; break;
        default: break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 7; i++) {
        if (!seen[i]) {
            return FEB_CBOR_ERR_MISSING_FIELD;
        }
    }
    return FEB_CBOR_OK;
}

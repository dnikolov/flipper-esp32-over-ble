#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

/* ---- `gps` capability status.result payload ---- */

size_t feb_cbor_encode_gps_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_gps_result_payload_t* payload) {
    if(out == NULL || payload == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 8);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lat_e7_offset", sizeof("lat_e7_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->lat_e7_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "lon_e7_offset", sizeof("lon_e7_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->lon_e7_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "fix_quality", sizeof("fix_quality") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->fix_quality);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "satellites", sizeof("satellites") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->satellites);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "hdop_e1", sizeof("hdop_e1") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->hdop_e1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "utc_timestamp_s", sizeof("utc_timestamp_s") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->utc_timestamp_s);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(
        out + pos, out_cap - pos, "altitude_dm_offset", sizeof("altitude_dm_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->altitude_dm_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "speed_e1_kmh", sizeof("speed_e1_kmh") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, payload->speed_e1_kmh);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

feb_cbor_status_t feb_cbor_decode_gps_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_gps_result_payload_t* payload) {
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
    if(count < 8) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 8) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[8];
    size_t seen_lens[8];
    size_t n;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "lat_e7_offset", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->lat_e7_offset, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "lon_e7_offset", seen_ptrs, seen_lens, 1, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->lon_e7_offset, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "fix_quality", seen_ptrs, seen_lens, 2, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->fix_quality, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "satellites", seen_ptrs, seen_lens, 3, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->satellites, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "hdop_e1", seen_ptrs, seen_lens, 4, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->hdop_e1, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "utc_timestamp_s", seen_ptrs, seen_lens, 5, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->utc_timestamp_s, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "altitude_dm_offset", seen_ptrs, seen_lens, 6, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->altitude_dm_offset, &status);
    if(n == 0) return status;
    pos += n;

    n = feb_cbor_i_decode_expected_key(
        in + pos, in_len - pos, "speed_e1_kmh", seen_ptrs, seen_lens, 7, &status);
    if(n == 0) return status;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &payload->speed_e1_kmh, &status);
    if(n == 0) return status;
    pos += n;

    return FEB_CBOR_OK;
}

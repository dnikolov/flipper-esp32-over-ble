#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

/* ---- `wifi_scan` capability payloads ---- */

size_t feb_cbor_encode_wifi_scan_ap(uint8_t* out, size_t out_cap, const feb_wifi_scan_ap_t* ap) {
    if(out == NULL || ap == NULL || ap->phy == NULL || ap->auth == NULL) {
        return 0;
    }
    if(ap->ssid == NULL && ap->ssid_len > 0) {
        return 0;
    }
    if(ap->ssid_len > FEB_WIFI_SCAN_SSID_MAX_LEN || ap->rssi_offset > 255u) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 6);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ssid", sizeof("ssid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ap->ssid, ap->ssid_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "bssid", sizeof("bssid") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ap->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ap->rssi_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "channel", sizeof("channel") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ap->channel);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "phy", sizeof("phy") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, ap->phy, ap->phy_len);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "auth", sizeof("auth") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, ap->auth, ap->auth_len);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

size_t feb_cbor_decode_wifi_scan_ap(
    const uint8_t* in,
    size_t in_len,
    feb_wifi_scan_ap_t* ap,
    feb_cbor_status_t* status) {
    if(in == NULL || ap == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(ap, 0, sizeof(*ap));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 6) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 6) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[6];
    size_t seen_lens[6];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "ssid", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_bytes(
        in + pos, in_len - pos, &ap->ssid, &ap->ssid_len, FEB_WIFI_SCAN_SSID_MAX_LEN, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "bssid", seen_ptrs, seen_lens, 1, status);
    if(n == 0) return 0;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_WIFI_SCAN_BSSID_LEN, status);
        if(n == 0) return 0;
        if(len != FEB_WIFI_SCAN_BSSID_LEN) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        memcpy(ap->bssid, data, FEB_WIFI_SCAN_BSSID_LEN);
        pos += n;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "rssi_offset", seen_ptrs, seen_lens, 2, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &ap->rssi_offset, status);
    if(n == 0) return 0;
    if(ap->rssi_offset > 255u) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "channel", seen_ptrs, seen_lens, 3, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &ap->channel, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "phy", seen_ptrs, seen_lens, 4, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(in + pos, in_len - pos, &ap->phy, &ap->phy_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "auth", seen_ptrs, seen_lens, 5, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(in + pos, in_len - pos, &ap->auth, &ap->auth_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wifi_scan_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_wifi_scan_result_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->ap_count > FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "aps", sizeof("aps") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->ap_count);
    if(n == 0) return 0;
    pos += n;
    for(size_t i = 0; i < payload->ap_count; i++) {
        n = feb_cbor_encode_wifi_scan_ap(out + pos, out_cap - pos, &payload->aps[i]);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_wifi_scan_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_wifi_scan_result_payload_t* payload) {
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
    if(count < 1) {
        return FEB_CBOR_ERR_MISSING_FIELD;
    }
    if(count > 1) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }

    const uint8_t* seen_ptrs[1];
    size_t seen_lens[1];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "aps", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;

    size_t array_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
    if(n == 0) return status;
    if(array_count > FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;

    for(size_t i = 0; i < array_count; i++) {
        size_t item_len = feb_cbor_decode_wifi_scan_ap(in + pos, in_len - pos, &payload->aps[i], &status);
        if(item_len == 0) {
            return status;
        }
        pos += item_len;
    }
    payload->ap_count = array_count;

    return FEB_CBOR_OK;
}

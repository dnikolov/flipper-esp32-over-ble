#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

size_t feb_cbor_encode_wifi_scan_ap(uint8_t *out, size_t out_cap, const feb_wifi_scan_ap_t *ap)
{
    size_t pos = 0;
    size_t n;

    if (out == NULL || ap == NULL) {
        return 0;
    }
    if (ap->ssid_len > FEB_WIFI_SCAN_SSID_MAX_LEN || ap->rssi_offset > 255u) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 6);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "ssid", FEB_CBOR_I_KLEN("ssid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ap->ssid, ap->ssid_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "bssid", FEB_CBOR_I_KLEN("bssid"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, ap->bssid, FEB_WIFI_SCAN_BSSID_LEN);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", FEB_CBOR_I_KLEN("rssi_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ap->rssi_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "channel", FEB_CBOR_I_KLEN("channel"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, ap->channel);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "phy", FEB_CBOR_I_KLEN("phy"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, ap->phy, ap->phy_len);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "auth", FEB_CBOR_I_KLEN("auth"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, ap->auth, ap->auth_len);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

size_t feb_cbor_decode_wifi_scan_ap(const uint8_t *in, size_t in_len, feb_wifi_scan_ap_t *ap, feb_cbor_status_t *status)
{
    static const char *const names[6] = {"ssid", "bssid", "rssi_offset", "channel", "phy", "auth"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[6] = {0, 0, 0, 0, 0, 0};
    feb_cbor_status_t local_status;
    size_t consumed;

    if (status == NULL) {
        return 0;
    }
    if (in == NULL || ap == NULL) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &local_status);
    if (consumed == 0) {
        *status = local_status;
        return 0;
    }
    if (count > 6) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
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
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);
        if (key_consumed == 0) {
            *status = local_status;
            return 0;
        }
        pos += key_consumed;

        found = -1;
        for (j = 0; j < 6; j++) {
            if (key_len == strlen(names[j]) && memcmp(key_data, names[j], key_len) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        field_index = (size_t)found;
        if (seen[field_index]) {
            *status = FEB_CBOR_ERR_DUPLICATE_KEY;
            return 0;
        }
        if (field_index < next_min) {
            *status = FEB_CBOR_ERR_OUT_OF_ORDER;
            return 0;
        }

        switch (field_index) {
        case 0: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_WIFI_SCAN_SSID_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ap->ssid = data;
            ap->ssid_len = len;
            pos += n;
            break;
        }
        case 1: {
            const uint8_t *data;
            size_t len;
            size_t n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len,
                                              FEB_WIFI_SCAN_BSSID_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (len != FEB_WIFI_SCAN_BSSID_LEN) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            memcpy(ap->bssid, data, FEB_WIFI_SCAN_BSSID_LEN);
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (value > 255u) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            ap->rssi_offset = value;
            pos += n;
            break;
        }
        case 3: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ap->channel = value;
            pos += n;
            break;
        }
        case 4: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ap->phy = data;
            ap->phy_len = len;
            pos += n;
            break;
        }
        case 5: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            ap->auth = data;
            ap->auth_len = len;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    for (i = 0; i < 6; i++) {
        if (!seen[i]) {
            *status = FEB_CBOR_ERR_MISSING_FIELD;
            return 0;
        }
    }
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_wifi_scan_result_payload(uint8_t *out, size_t out_cap, const feb_wifi_scan_result_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->ap_count > FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "aps", FEB_CBOR_I_KLEN("aps"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->ap_count);
    if (n == 0) return 0;
    pos += n;

    for (i = 0; i < payload->ap_count; i++) {
        n = feb_cbor_encode_wifi_scan_ap(out + pos, out_cap - pos, &payload->aps[i]);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
}

/* feb_cbor_decode_wifi_scan_result_payload() below is a thin wrapper over the streaming
   decoder just below it (feb_cbor_decode_wifi_scan_result_payload_stream()) -- one decode
   body per payload, not two. This fill callback is the only whole-array-specific piece: it
   writes each streamed AP into payload->aps[] in order, matching the pre-streaming
   implementation's own loop exactly (grep history: this file used to decode directly into
   payload->aps[i] in a single pass; array-cap overflow and per-field validation are still
   enforced identically, just inside the streaming pass now). */
typedef struct {
    feb_wifi_scan_result_payload_t *payload;
    size_t index;
} wifi_scan_array_fill_ctx_t;

static void wifi_scan_array_fill_cb(const feb_wifi_scan_ap_t *ap, void *ctx_ptr)
{
    wifi_scan_array_fill_ctx_t *ctx = (wifi_scan_array_fill_ctx_t *)ctx_ptr;

    ctx->payload->aps[ctx->index++] = *ap;
}

feb_cbor_status_t feb_cbor_decode_wifi_scan_result_payload(const uint8_t *in, size_t in_len, feb_wifi_scan_result_payload_t *payload)
{
    wifi_scan_array_fill_ctx_t ctx;
    size_t ap_count = 0;
    feb_cbor_status_t status;

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->ap_count = 0;
    ctx.payload = payload;
    ctx.index = 0;

    status = feb_cbor_decode_wifi_scan_result_payload_stream(in, in_len, wifi_scan_array_fill_cb, &ctx, &ap_count);
    if (status != FEB_CBOR_OK) {
        return status;
    }
    payload->ap_count = ap_count;

    return FEB_CBOR_OK;
}

/* mode: cb == NULL is the validate-only pass (no side effects); cb != NULL is the apply pass.
   See feb_cbor_decode_wifi_scan_result_payload_stream()'s header comment for why both passes
   exist. */
static feb_cbor_status_t wifi_scan_result_payload_stream_pass(const uint8_t *in, size_t in_len,
    feb_wifi_scan_ap_stream_cb_t cb, void *ctx, size_t *ap_count_out)
{
    size_t count;
    size_t pos;
    size_t i;
    feb_cbor_status_t status;
    size_t consumed;
    const char *key_data;
    size_t key_len;
    size_t key_consumed;
    size_t arr_count;
    size_t arr_consumed;
    feb_wifi_scan_ap_t ap;

    if (in == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    if (ap_count_out != NULL) {
        *ap_count_out = 0;
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

    key_consumed = feb_cbor_decode_text(in + pos, in_len - pos, &key_data, &key_len,
                                         FEB_CBOR_MAX_TEXT_LEN, &status);
    if (key_consumed == 0) {
        return status;
    }
    if (key_len != strlen("aps") || memcmp(key_data, "aps", key_len) != 0) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    pos += key_consumed;

    arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
    if (arr_consumed == 0) {
        return status;
    }
    if (arr_count > FEB_WIFI_SCAN_MAX_APS_PER_RECORD) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += arr_consumed;

    for (i = 0; i < arr_count; i++) {
        size_t ap_consumed = feb_cbor_decode_wifi_scan_ap(in + pos, in_len - pos, &ap, &status);

        if (ap_consumed == 0) {
            return status;
        }
        pos += ap_consumed;
        if (cb != NULL) {
            cb(&ap, ctx);
        }
    }
    if (ap_count_out != NULL) {
        *ap_count_out = arr_count;
    }

    return FEB_CBOR_OK;
}

feb_cbor_status_t feb_cbor_decode_wifi_scan_result_payload_stream(const uint8_t *in, size_t in_len,
    feb_wifi_scan_ap_stream_cb_t cb, void *ctx, size_t *ap_count_out)
{
    feb_cbor_status_t status = wifi_scan_result_payload_stream_pass(in, in_len, NULL, NULL, NULL);

    if (status != FEB_CBOR_OK) {
        if (ap_count_out != NULL) {
            *ap_count_out = 0;
        }
        return status;
    }
    return wifi_scan_result_payload_stream_pass(in, in_len, cb, ctx, ap_count_out);
}

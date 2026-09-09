#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

/* ---- `ble_scan` capability payloads ---- */

size_t feb_cbor_encode_ble_scan_device(uint8_t* out, size_t out_cap, const feb_ble_scan_device_t* device) {
    if(out == NULL || device == NULL || device->addr_type == NULL) {
        return 0;
    }
    if(device->has_name && device->name == NULL) {
        return 0;
    }
    if(device->has_name && device->name_len > FEB_BLE_SCAN_NAME_MAX_LEN) {
        return 0;
    }
    if(device->rssi_offset > 255u) {
        return 0;
    }
    size_t count = device->has_name ? 4u : 3u;
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, count);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "address", sizeof("address") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, device->address, FEB_BLE_SCAN_ADDRESS_LEN);
    if(n == 0) return 0;
    pos += n;
    if(device->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", sizeof("name") - 1);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, device->name, device->name_len);
        if(n == 0) return 0;
        pos += n;
    }
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", sizeof("rssi_offset") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, device->rssi_offset);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "addr_type", sizeof("addr_type") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, device->addr_type, device->addr_type_len);
    if(n == 0) return 0;
    pos += n;
    return pos;
}

size_t feb_cbor_decode_ble_scan_device(
    const uint8_t* in,
    size_t in_len,
    feb_ble_scan_device_t* device,
    feb_cbor_status_t* status) {
    if(in == NULL || device == NULL || status == NULL) {
        if(status != NULL) *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    memset(device, 0, sizeof(*device));
    size_t count = 0;
    size_t pos = feb_cbor_decode_map_header(in, in_len, &count, status);
    if(pos == 0) {
        return 0;
    }
    if(count < 3) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    if(count > 4) {
        *status = FEB_CBOR_ERR_TOO_MANY_ENTRIES;
        return 0;
    }

    const uint8_t* seen_ptrs[4];
    size_t seen_lens[4];
    size_t n;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "address", seen_ptrs, seen_lens, 0, status);
    if(n == 0) return 0;
    pos += n;
    {
        const uint8_t* data;
        size_t len;
        n = feb_cbor_decode_bytes(in + pos, in_len - pos, &data, &len, FEB_BLE_SCAN_ADDRESS_LEN, status);
        if(n == 0) return 0;
        if(len != FEB_BLE_SCAN_ADDRESS_LEN) {
            *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
            return 0;
        }
        memcpy(device->address, data, FEB_BLE_SCAN_ADDRESS_LEN);
        pos += n;
    }

    size_t next_index = 1;
    if(count == 4) {
        n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "name", seen_ptrs, seen_lens, next_index, status);
        if(n == 0) return 0;
        pos += n;
        n = feb_cbor_decode_text(
            in + pos, in_len - pos, &device->name, &device->name_len, FEB_BLE_SCAN_NAME_MAX_LEN, status);
        if(n == 0) return 0;
        pos += n;
        device->has_name = 1;
        next_index++;
    }

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "rssi_offset", seen_ptrs, seen_lens, next_index, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_uint(in + pos, in_len - pos, &device->rssi_offset, status);
    if(n == 0) return 0;
    if(device->rssi_offset > 255u) {
        *status = FEB_CBOR_ERR_UNEXPECTED_TYPE;
        return 0;
    }
    pos += n;
    next_index++;

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "addr_type", seen_ptrs, seen_lens, next_index, status);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_decode_text(
        in + pos, in_len - pos, &device->addr_type, &device->addr_type_len, FEB_CBOR_MAX_TEXT_LEN, status);
    if(n == 0) return 0;
    pos += n;

    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_ble_scan_result_payload(
    uint8_t* out,
    size_t out_cap,
    const feb_ble_scan_result_payload_t* payload) {
    if(out == NULL || payload == NULL || payload->device_count > FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        return 0;
    }
    size_t pos = 0;
    size_t n;
    n = feb_cbor_encode_map_header(out, out_cap, 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, "devices", sizeof("devices") - 1);
    if(n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->device_count);
    if(n == 0) return 0;
    pos += n;
    for(size_t i = 0; i < payload->device_count; i++) {
        n = feb_cbor_encode_ble_scan_device(out + pos, out_cap - pos, &payload->devices[i]);
        if(n == 0) return 0;
        pos += n;
    }
    return pos;
}

feb_cbor_status_t feb_cbor_decode_ble_scan_result_payload(
    const uint8_t* in,
    size_t in_len,
    feb_ble_scan_result_payload_t* payload) {
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

    n = feb_cbor_i_decode_expected_key(in + pos, in_len - pos, "devices", seen_ptrs, seen_lens, 0, &status);
    if(n == 0) return status;
    pos += n;

    size_t array_count = 0;
    n = feb_cbor_decode_array_header(in + pos, in_len - pos, &array_count, &status);
    if(n == 0) return status;
    if(array_count > FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += n;

    for(size_t i = 0; i < array_count; i++) {
        size_t item_len =
            feb_cbor_decode_ble_scan_device(in + pos, in_len - pos, &payload->devices[i], &status);
        if(item_len == 0) {
            return status;
        }
        pos += item_len;
    }
    payload->device_count = array_count;

    return FEB_CBOR_OK;
}

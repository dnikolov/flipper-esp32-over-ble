#include "cbor_codec.h"
#include "cbor_internal.h"

#include <string.h>

size_t feb_cbor_encode_ble_scan_device(uint8_t *out, size_t out_cap, const feb_ble_scan_device_t *device)
{
    size_t pos = 0;
    size_t n;
    size_t count = 3;

    if (out == NULL || device == NULL) {
        return 0;
    }
    if (device->rssi_offset > 255u) {
        return 0;
    }
    if (device->has_name) count++;

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, count);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "address", FEB_CBOR_I_KLEN("address"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_bytes(out + pos, out_cap - pos, device->address, FEB_BLE_SCAN_ADDRESS_LEN);
    if (n == 0) return 0;
    pos += n;

    if (device->has_name) {
        n = feb_cbor_encode_text(out + pos, out_cap - pos, "name", FEB_CBOR_I_KLEN("name"));
        if (n == 0) return 0;
        pos += n;
        n = feb_cbor_encode_text(out + pos, out_cap - pos, device->name, device->name_len);
        if (n == 0) return 0;
        pos += n;
    }

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "rssi_offset", FEB_CBOR_I_KLEN("rssi_offset"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_uint(out + pos, out_cap - pos, device->rssi_offset);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "addr_type", FEB_CBOR_I_KLEN("addr_type"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_text(out + pos, out_cap - pos, device->addr_type, device->addr_type_len);
    if (n == 0) return 0;
    pos += n;

    return pos;
}

size_t feb_cbor_decode_ble_scan_device(const uint8_t *in, size_t in_len, feb_ble_scan_device_t *device, feb_cbor_status_t *status)
{
    static const char *const names[4] = {"address", "name", "rssi_offset", "addr_type"};
    size_t count;
    size_t pos;
    size_t i;
    size_t next_min = 0;
    int seen[4] = {0, 0, 0, 0};
    feb_cbor_status_t local_status;
    size_t consumed;

    if (status == NULL) {
        return 0;
    }
    if (in == NULL || device == NULL) {
        *status = FEB_CBOR_ERR_TRUNCATED;
        return 0;
    }
    device->has_name = 0;

    consumed = feb_cbor_decode_map_header(in, in_len, &count, &local_status);
    if (consumed == 0) {
        *status = local_status;
        return 0;
    }
    if (count > 4) {
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
        for (j = 0; j < 4; j++) {
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
                                              FEB_BLE_SCAN_ADDRESS_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (len != FEB_BLE_SCAN_ADDRESS_LEN) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            memcpy(device->address, data, FEB_BLE_SCAN_ADDRESS_LEN);
            pos += n;
            break;
        }
        case 1: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_BLE_SCAN_NAME_MAX_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            device->name = data;
            device->name_len = len;
            device->has_name = 1;
            pos += n;
            break;
        }
        case 2: {
            uint64_t value;
            size_t n = feb_cbor_decode_uint(in + pos, in_len - pos, &value, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            if (value > 255u) { *status = FEB_CBOR_ERR_UNEXPECTED_TYPE; return 0; }
            device->rssi_offset = value;
            pos += n;
            break;
        }
        case 3: {
            const char *data;
            size_t len;
            size_t n = feb_cbor_decode_text(in + pos, in_len - pos, &data, &len,
                                             FEB_CBOR_MAX_TEXT_LEN, &local_status);

            if (n == 0) { *status = local_status; return 0; }
            device->addr_type = data;
            device->addr_type_len = len;
            pos += n;
            break;
        }
        default:
            break;
        }

        seen[field_index] = 1;
        next_min = field_index + 1;
    }

    if (!seen[0] || !seen[2] || !seen[3]) {
        *status = FEB_CBOR_ERR_MISSING_FIELD;
        return 0;
    }
    *status = FEB_CBOR_OK;
    return pos;
}

size_t feb_cbor_encode_ble_scan_result_payload(uint8_t *out, size_t out_cap, const feb_ble_scan_result_payload_t *payload)
{
    size_t pos = 0;
    size_t n;
    size_t i;

    if (out == NULL || payload == NULL) {
        return 0;
    }
    if (payload->device_count > FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        return 0;
    }

    n = feb_cbor_encode_map_header(out + pos, out_cap - pos, 1);
    if (n == 0) return 0;
    pos += n;

    n = feb_cbor_encode_text(out + pos, out_cap - pos, "devices", FEB_CBOR_I_KLEN("devices"));
    if (n == 0) return 0;
    pos += n;
    n = feb_cbor_encode_array_header(out + pos, out_cap - pos, payload->device_count);
    if (n == 0) return 0;
    pos += n;

    for (i = 0; i < payload->device_count; i++) {
        n = feb_cbor_encode_ble_scan_device(out + pos, out_cap - pos, &payload->devices[i]);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
}

feb_cbor_status_t feb_cbor_decode_ble_scan_result_payload(const uint8_t *in, size_t in_len, feb_ble_scan_result_payload_t *payload)
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

    if (in == NULL || payload == NULL) {
        return FEB_CBOR_ERR_TRUNCATED;
    }
    payload->device_count = 0;

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
    if (key_len != strlen("devices") || memcmp(key_data, "devices", key_len) != 0) {
        return FEB_CBOR_ERR_UNEXPECTED_TYPE;
    }
    pos += key_consumed;

    arr_consumed = feb_cbor_decode_array_header(in + pos, in_len - pos, &arr_count, &status);
    if (arr_consumed == 0) {
        return status;
    }
    if (arr_count > FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD) {
        return FEB_CBOR_ERR_TOO_MANY_ENTRIES;
    }
    pos += arr_consumed;

    for (i = 0; i < arr_count; i++) {
        size_t device_consumed = feb_cbor_decode_ble_scan_device(in + pos, in_len - pos, &payload->devices[i], &status);

        if (device_consumed == 0) {
            return status;
        }
        pos += device_consumed;
    }
    payload->device_count = arr_count;

    return FEB_CBOR_OK;
}

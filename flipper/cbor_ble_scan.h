/* Split out of cbor_codec.h 2026-09-08 (docs/OPTIMIZATION.md item 1) -- included by the
   umbrella cbor_codec.h, which must be included first (directly or transitively) so that
   feb_cbor_status_t and the FEB_CBOR_MAX_* macros this header uses are already visible; not
   meant to be included standalone. Not part of the cross-firmware shared-header contract
   check (tools/check_shared_headers.py diffs cbor_codec.h itself, which still transitively
   provides everything declared here).

   ---- `ble_scan`-specific `<device-result>` element and `result` map
   (docs/PROTOCOL.md "`ble_scan` command and status payloads") ----
   Field order per `<device-result>`: address, name (optional), rssi_offset, addr_type --
   matches PROTOCOL.md's table exactly. `name` is omitted from the map entirely (not an
   empty string) when the peer advertised no name -- same has_*-flag optional-field
   convention as feb_error_payload_t.has_message (cbor_records.h); a decoder distinguishes
   "absent" (map has 3 entries) from "present and empty" (map has 4 entries, name_len == 0)
   by the map's own entry count, same technique feb_cbor_decode_status_payload() already
   uses for its optional `result` field. `rssi_offset` is `rssi_dbm + 128`, same convention
   as `wifi_scan`'s field of the same name. `address`/`addr_type` follow this header's usual
   exact-length-copy / caller-owned-text conventions (see `bssid`/`phy`/`auth` in
   cbor_wifi_scan.h). */
#ifndef FEB_CBOR_BLE_SCAN_H
#define FEB_CBOR_BLE_SCAN_H

#define FEB_BLE_SCAN_ADDRESS_LEN 6u
#define FEB_BLE_SCAN_NAME_MAX_LEN 31u
/* Per-status-record `devices[]` bound; same generic array cap FEB_WIFI_SCAN_MAX_APS_PER_RECORD
   reuses. PROTOCOL.md's 32-total-devices-per-scan cap is a separate ESP32-side
   scan-result-selection concern, not a codec-layer bound. */
#define FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD FEB_CBOR_MAX_ARRAY_ENTRIES

typedef struct {
    uint8_t address[FEB_BLE_SCAN_ADDRESS_LEN];
    const char *name; /* NULL if absent; see has_name */
    size_t name_len;
    int has_name;
    uint64_t rssi_offset; /* rssi_dbm + 128; encoder/decoder reject a value > 255 */
    const char *addr_type; /* "public" or "random"; caller-owned, not validated here --
                               same treatment as feb_wifi_scan_ap_t's phy/auth */
    size_t addr_type_len;
} feb_ble_scan_device_t;

size_t feb_cbor_encode_ble_scan_device(uint8_t *out, size_t out_cap, const feb_ble_scan_device_t *device);
size_t feb_cbor_decode_ble_scan_device(const uint8_t *in, size_t in_len, feb_ble_scan_device_t *device, feb_cbor_status_t *status);

typedef struct {
    feb_ble_scan_device_t devices[FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD];
    size_t device_count;
} feb_ble_scan_result_payload_t;

size_t feb_cbor_encode_ble_scan_result_payload(uint8_t *out, size_t out_cap, const feb_ble_scan_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_ble_scan_result_payload(const uint8_t *in, size_t in_len, feb_ble_scan_result_payload_t *payload);

#endif /* FEB_CBOR_BLE_SCAN_H */

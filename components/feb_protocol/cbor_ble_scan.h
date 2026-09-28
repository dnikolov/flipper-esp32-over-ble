/* Split out of cbor_codec.h (docs/OPTIMIZATION.md item 1) -- shared contract, mirrored
   byte-for-byte in flipper/cbor_ble_scan.h. Changes here must be mirrored there and in
   docs/PROTOCOL.md, or the two firmwares diverge. Included transitively via cbor_codec.h;
   nothing outside the codec split should need to include this directly.

   ---- `ble_scan`-specific `<device-result>` element and `result` map
   (docs/PROTOCOL.md "`ble_scan` command and status payloads") ----
   Field order per `<device-result>`: address, name (optional), rssi_offset, addr_type --
   matches PROTOCOL.md's table exactly. `name`'s optional-field convention mirrors
   `error.message`/`capability_query.requested` (cbor_records.h): a `has_name` flag controls
   whether the field is encoded at all; a decoder treats "field absent" as legal and distinct
   from "field present and empty". `rssi_offset` is `rssi_dbm + 128`, same convention as
   `wifi_scan`. `addr_type` is caller-owned text ("public"/"random" per PROTOCOL.md); this
   module does not restrict its value, matching `phy`/`auth`'s treatment in cbor_wifi_scan.h
   -- value validation is a caller (main.c) concern. `feb_ble_scan_device_t` follows the same
   array-element decode convention as `feb_wifi_scan_ap_t` (returns bytes consumed, not a
   whole-payload-span status). */
#ifndef FEB_CBOR_BLE_SCAN_H
#define FEB_CBOR_BLE_SCAN_H

#include "cbor_codec.h"

#define FEB_BLE_SCAN_ADDRESS_LEN 6u
#define FEB_BLE_SCAN_NAME_MAX_LEN 31u
/* Per-status-record `devices[]` bound; PROTOCOL.md's 32-total-devices-per-scan cap is a
   separate ESP32-side scan-result-selection concern (main.c), same relationship
   FEB_WIFI_SCAN_MAX_APS_PER_RECORD has to wifi_scan's cap. */
#define FEB_BLE_SCAN_MAX_DEVICES_PER_RECORD FEB_CBOR_MAX_ARRAY_ENTRIES

typedef struct {
    uint8_t address[FEB_BLE_SCAN_ADDRESS_LEN];
    const char *name; /* NULL if has_name is 0; 0..FEB_BLE_SCAN_NAME_MAX_LEN bytes otherwise */
    size_t name_len;
    int has_name;
    uint64_t rssi_offset; /* rssi_dbm + 128; encoder/decoder reject a value > 255 */
    const char *addr_type;
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

/* Streaming counterpart to feb_cbor_decode_ble_scan_result_payload() above
   (docs/HARDENING_BACKLOG.md H04) -- same two-pass validate-then-apply contract as
   feb_cbor_decode_wifi_scan_result_payload_stream() (cbor_wifi_scan.h): a malformed device
   anywhere in the batch fails the whole decode with zero `cb` invocations, never a partial
   prefix. `device` passed to `cb` is only valid for the duration of that call.
   `device_count_out` (may be NULL) receives the decoded count on FEB_CBOR_OK, 0 otherwise. */
typedef void (*feb_ble_scan_device_stream_cb_t)(const feb_ble_scan_device_t *device, void *ctx);
feb_cbor_status_t feb_cbor_decode_ble_scan_result_payload_stream(const uint8_t *in, size_t in_len, feb_ble_scan_device_stream_cb_t cb, void *ctx, size_t *device_count_out);

#endif /* FEB_CBOR_BLE_SCAN_H */

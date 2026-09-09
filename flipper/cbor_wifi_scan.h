/* Split out of cbor_codec.h 2026-09-08 (docs/OPTIMIZATION.md item 1) -- included by the
   umbrella cbor_codec.h, which must be included first (directly or transitively) so that
   feb_cbor_status_t and the FEB_CBOR_MAX_* macros this header uses are already visible; not
   meant to be included standalone. Not part of the cross-firmware shared-header contract
   check (tools/check_shared_headers.py diffs cbor_codec.h itself, which still transitively
   provides everything declared here).

   ---- `wifi_scan`-specific `<ap-result>` element and `result` map
   (docs/PROTOCOL.md "`wifi_scan` command and status payloads") ----
   Field order per `<ap-result>`: ssid, bssid, rssi_offset, channel, phy, auth -- matches
   PROTOCOL.md's table exactly. `rssi_offset` is `rssi_dbm + 128` (an unsigned 0-255 value)
   per PROTOCOL.md's canonical-CBOR rule against negative integers in payload maps; callers
   convert to/from a real signed dBm value themselves. `phy`/`auth` are caller-owned text
   (main.c holds the board-specific wifi_auth_mode_t / PHY-generation string tables; this
   module only encodes/decodes whatever text it's given). `ssid` aliases the input on decode,
   same convention as every other variable-length byte/text field in this header; `bssid` is
   copied since it's exact-length, matching `session_id`'s treatment in cbor_records.h.

   `feb_wifi_scan_ap_t` is encoded/decoded as an array *element* (like `features` in
   cbor_records.h), not a standalone payload -- feb_cbor_decode_wifi_scan_ap() therefore
   follows the primitive decoder convention (returns bytes consumed via
   feb_cbor_decode_uint/_bytes/_text, not the feb_cbor_status_t-returning "whole exact
   payload span" convention used by `command`/`status`/`capability_*`), since the caller must
   know how far to advance within the `aps` array. */
#ifndef FEB_CBOR_WIFI_SCAN_H
#define FEB_CBOR_WIFI_SCAN_H

#define FEB_WIFI_SCAN_SSID_MAX_LEN 32u
#define FEB_WIFI_SCAN_BSSID_LEN 6u
/* Per-status-record `aps[]` bound; reuses the same generic array cap used elsewhere in this
   file. PROTOCOL.md's 32-total-APs-per-scan cap is a separate ESP32-side scan-result-selection
   concern (main.c), not a codec-layer bound -- it happens to be the same number today. */
#define FEB_WIFI_SCAN_MAX_APS_PER_RECORD FEB_CBOR_MAX_ARRAY_ENTRIES

typedef struct {
    const uint8_t *ssid; /* 0..FEB_WIFI_SCAN_SSID_MAX_LEN bytes; not guaranteed valid UTF-8 */
    size_t ssid_len;
    uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN];
    uint64_t rssi_offset; /* rssi_dbm + 128; encoder/decoder reject a value > 255 */
    uint64_t channel;
    const char *phy;
    size_t phy_len;
    const char *auth;
    size_t auth_len;
} feb_wifi_scan_ap_t;

size_t feb_cbor_encode_wifi_scan_ap(uint8_t *out, size_t out_cap, const feb_wifi_scan_ap_t *ap);
size_t feb_cbor_decode_wifi_scan_ap(const uint8_t *in, size_t in_len, feb_wifi_scan_ap_t *ap, feb_cbor_status_t *status);

typedef struct {
    feb_wifi_scan_ap_t aps[FEB_WIFI_SCAN_MAX_APS_PER_RECORD];
    size_t ap_count;
} feb_wifi_scan_result_payload_t;

size_t feb_cbor_encode_wifi_scan_result_payload(uint8_t *out, size_t out_cap, const feb_wifi_scan_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_wifi_scan_result_payload(const uint8_t *in, size_t in_len, feb_wifi_scan_result_payload_t *payload);

#endif /* FEB_CBOR_WIFI_SCAN_H */

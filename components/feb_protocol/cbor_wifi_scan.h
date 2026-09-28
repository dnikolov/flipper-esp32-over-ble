/* Split out of cbor_codec.h (docs/OPTIMIZATION.md item 1) -- shared contract, mirrored
   byte-for-byte in flipper/cbor_wifi_scan.h. Changes here must be mirrored there and in
   docs/PROTOCOL.md, or the two firmwares diverge. Included transitively via cbor_codec.h;
   nothing outside the codec split should need to include this directly.

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

#include "cbor_codec.h"

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

/* Streaming counterpart to feb_cbor_decode_wifi_scan_result_payload() above
   (docs/HARDENING_BACKLOG.md H04) -- for a caller that consumes each AP as it's decoded
   instead of needing the whole FEB_WIFI_SCAN_MAX_APS_PER_RECORD-entry array resident at once.
   Enforces identical validation (array cap, per-AP field order/type/range) as the whole-array
   decoder. Internally two passes over `in`: the first decodes every AP without invoking `cb`,
   purely to confirm the whole payload is well-formed; only once that succeeds does a second
   pass invoke `cb` once per AP, in wire order. This preserves the whole-array decoder's own
   all-or-nothing contract -- a malformed AP anywhere in the batch fails the whole decode with
   zero `cb` invocations, never a partial prefix of a batch that turns out invalid. `ap` passed
   to `cb` is only valid for the duration of that call. `ap_count_out` (may be NULL) receives
   the decoded count on FEB_CBOR_OK, 0 otherwise. */
typedef void (*feb_wifi_scan_ap_stream_cb_t)(const feb_wifi_scan_ap_t *ap, void *ctx);
feb_cbor_status_t feb_cbor_decode_wifi_scan_result_payload_stream(const uint8_t *in, size_t in_len, feb_wifi_scan_ap_stream_cb_t cb, void *ctx, size_t *ap_count_out);

#endif /* FEB_CBOR_WIFI_SCAN_H */

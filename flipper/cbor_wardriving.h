/* Split out of cbor_codec.h 2026-09-08 (docs/OPTIMIZATION.md item 1) -- included by the
   umbrella cbor_codec.h, which must be included first (directly or transitively) so that
   feb_cbor_status_t, the FEB_CBOR_MAX_* macros, and cbor_wifi_scan.h's/cbor_ble_scan.h's
   FEB_WIFI_SCAN_ / FEB_BLE_SCAN_ macros (aliased below) are already visible; not meant to be
   included standalone. Not part of the cross-firmware shared-header contract check
   (tools/check_shared_headers.py diffs cbor_codec.h itself, which still transitively
   provides everything declared here).

   ---- `wardriving` command/status payloads (docs/PROTOCOL.md "`wardriving` command and
   status payloads") ----

   `command.arguments` field order: action, sources, wifi_interval_ms, ble_window_ms,
   ble_interval_ms, wifi_swelling, country, wifi_band -- matches PROTOCOL.md exactly
   (`wifi_swelling`/`country` appended 2026-09-21, docs/WARDRIVING_REDESIGN.md; `wifi_band`
   appended 2026-09-26 for the OLIMEX MOD-ESP32-C5's dual-band radio, per this protocol's
   append-only convention -- always sent regardless of board 5GHz capability, see
   PROTOCOL.md's `wifi_band` row). `sources`/`wifi_interval_ms`/`ble_window_ms`+
   `ble_interval_ms`/`wifi_swelling`/`country`/`wifi_band` are present only for
   `action = "start"`, and their presence is derived purely from the map's own field count at
   this layer (0, 2, 4, or 6 trailing fields after action+sources -- the surviving
   combinations once `wifi_swelling`/`country`/`wifi_band` always co-occur with
   `wifi_interval_ms` as a 4-field wifi block, per PROTOCOL.md's "required when wifi is in
   sources" rule, and `ble_window_ms`/`ble_interval_ms` always co-occur as a 2-field ble
   block; 1, 3, and 5 trailing fields are therefore not decodable shapes) -- this codec
   does NOT itself validate `action`'s or `sources`' element values against
   "start"/"stop"/"wifi"/"ble", nor `wifi_swelling`'s/`country`'s/`wifi_band`'s own text
   against their enumerated wire values (that is a dispatch-layer concern, same split as
   feb_wifi_scan_ap_t's phy/auth not being validated here) -- the three fields report
   presence via has_wifi_swelling/has_country/has_wifi_band independently of
   has_wifi_interval_ms/has_ble_params, even though the caller-side "required iff wifi in
   sources" rule ties them together in practice. Internal representation choice: `sources` is captured as a small
   array of caller-owned text pointers (FEB_WARDRIVING_MAX_SOURCES == 2, the only two
   defined values today), not as pre-resolved has_wifi_source/has_ble_source booleans --
   dispatch-layer code inspects the captured strings itself. `ble_window_ms`/
   `ble_interval_ms` are always required together per PROTOCOL.md, so a single
   has_ble_params flag makes the invalid "one present, one absent" state unrepresentable,
   rather than relying on the decoder to separately guard against it (converged with the
   ESP32 side on this point 2026-09-08; ESP32 previously used two separate flags). */
#ifndef FEB_CBOR_WARDRIVING_H
#define FEB_CBOR_WARDRIVING_H

#define FEB_WARDRIVING_MAX_SOURCES 2u
#define FEB_WARDRIVING_ACTION_MAX_LEN FEB_CBOR_MAX_TEXT_LEN
#define FEB_WARDRIVING_SOURCE_MAX_LEN FEB_CBOR_MAX_TEXT_LEN
#define FEB_WARDRIVING_SWELLING_MAX_LEN FEB_CBOR_MAX_TEXT_LEN
#define FEB_WARDRIVING_COUNTRY_MAX_LEN FEB_CBOR_MAX_TEXT_LEN
#define FEB_WARDRIVING_BAND_MAX_LEN FEB_CBOR_MAX_TEXT_LEN
#define FEB_WARDRIVING_WIFI_SSID_MAX_LEN FEB_WIFI_SCAN_SSID_MAX_LEN
#define FEB_WARDRIVING_BLE_ADDRESS_LEN FEB_BLE_SCAN_ADDRESS_LEN
#define FEB_WARDRIVING_BLE_NAME_MAX_LEN FEB_BLE_SCAN_NAME_MAX_LEN

typedef struct {
    const char *action;
    size_t action_len;
    const char *sources[FEB_WARDRIVING_MAX_SOURCES];
    size_t source_lens[FEB_WARDRIVING_MAX_SOURCES];
    size_t source_count;
    int has_sources; /* absent for action="stop" */
    uint64_t wifi_interval_ms;
    int has_wifi_interval_ms;
    uint64_t ble_window_ms;
    uint64_t ble_interval_ms;
    int has_ble_params; /* ble_window_ms/ble_interval_ms are always present or absent
                            together per PROTOCOL.md */
    const char *wifi_swelling;
    size_t wifi_swelling_len;
    int has_wifi_swelling;
    const char *country;
    size_t country_len;
    int has_country;
    const char *wifi_band;
    size_t wifi_band_len;
    int has_wifi_band;
} feb_wardriving_command_payload_t;

size_t feb_cbor_encode_wardriving_command_payload(uint8_t *out, size_t out_cap, const feb_wardriving_command_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_wardriving_command_payload(const uint8_t *in, size_t in_len, feb_wardriving_command_payload_t *payload);

/* `<wardriving-record>.payload` cut-down sub-shapes (docs/PROTOCOL.md: "the same
   fields/encodings as <ap-result>/<device-result> above, minus phy/addr_type"). Deliberately
   NOT feb_wifi_scan_ap_t/feb_ble_scan_device_t -- their wire field sets differ (this is
   PROTOCOL.md's own explicit "cut-down" framing), so reusing those structs would either
   silently encode a field the wire shape doesn't have or require a wasted/ignored member.
   Encode/decode for these two sub-shapes are file-static internal helpers in
   cbor_wardriving.c (not declared here), matching the ESP32 side -- nothing outside this
   codec calls them on either firmware. */
typedef struct {
    const uint8_t *ssid; /* 0..FEB_WARDRIVING_WIFI_SSID_MAX_LEN bytes; not guaranteed UTF-8 */
    size_t ssid_len;
    uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN];
    uint64_t rssi_offset; /* rssi_dbm + 128 */
    uint64_t channel;
    const char *auth; /* caller-owned, not validated here */
    size_t auth_len;
} feb_wardriving_wifi_payload_t;

typedef struct {
    uint8_t address[FEB_WARDRIVING_BLE_ADDRESS_LEN];
    const char *name; /* NULL if absent; see has_name */
    size_t name_len;
    int has_name;
    uint64_t rssi_offset; /* rssi_dbm + 128 */
} feb_wardriving_ble_payload_t;

/* `<wardriving-record>` fixed field order: timestamp_ms, utc_timestamp_s, lat_e7_offset,
   lon_e7_offset, source, payload -- matches PROTOCOL.md exactly (utc_timestamp_s added
   2026-09-12, docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and real
   wardriving-record timestamps" -- Unix epoch seconds derived from the most recent valid
   `RMC` sentence; always present and valid, per that design's decision 7, since a record is
   only ever logged/streamed while the location driver reports a real fix, which requires a
   valid RMC too -- no optional-field/fallback case to design around). `payload`'s shape is picked by `source`
   ("wifi" -> wifi payload, "ble" -> ble payload); any other `source` value is rejected
   FEB_CBOR_ERR_UNEXPECTED_TYPE by the decoder (this field IS validated here, unlike
   action/sources above, because it is structurally required to know which payload shape
   to decode next -- there is no way to defer it to a dispatch layer). `payload_kind` is the
   discriminator: the encoder derives which union member to encode from `payload_kind` (not
   from inspecting `source` text), and the decoder sets `payload_kind` from the decoded
   `source` string and populates only the matching union member -- matches the ESP32 side
   (converged 2026-09-08; this side previously held both sub-payloads as always-present named
   members instead of a real union, discriminated only by `source`). This struct is large
   relative to this codec's other per-element types (its own two-member union of full
   sub-payloads); any code holding an array of these on the BLE event path must keep it
   file-scope `static`, never a stack local -- see flipper_esp32_over_ble.c's existing
   static-buffer convention for wifi_scan_result. */
typedef enum {
    FEB_WARDRIVING_PAYLOAD_WIFI = 0,
    FEB_WARDRIVING_PAYLOAD_BLE = 1,
} feb_wardriving_payload_kind_t;

typedef struct {
    uint64_t timestamp_ms;
    uint64_t utc_timestamp_s;
    uint64_t lat_e7_offset;
    uint64_t lon_e7_offset;
    const char *source;
    size_t source_len;
    feb_wardriving_payload_kind_t payload_kind;
    union {
        feb_wardriving_wifi_payload_t wifi;
        feb_wardriving_ble_payload_t ble;
    } payload;
} feb_wardriving_record_t;

size_t feb_cbor_encode_wardriving_record(uint8_t *out, size_t out_cap, const feb_wardriving_record_t *record);
size_t feb_cbor_decode_wardriving_record(const uint8_t *in, size_t in_len, feb_wardriving_record_t *record, feb_cbor_status_t *status);

/* `status.result` for `wardriving`: `{ "records": [<wardriving-record>, ...],
   "backlog_remaining": uint }`, field order records then backlog_remaining, matching
   PROTOCOL.md. Reached via feb_cbor_decode_status_payload()'s existing fresh-depth-0
   feb_cbor_skip_value() capture of `result` (unchanged, capability-agnostic) -- this
   payload's own real structure (result map -> records array -> <wardriving-record> map ->
   payload map -> payload's own scalar fields) is 4 container levels below `result` itself,
   landing exactly at FEB_CBOR_MAX_NESTING; verified against a host-native round-trip
   vector (tests/flipper/test_flipper_codec.c) rather than assumed. This module's own
   encode/decode functions for `<wardriving-record>`/its sub-payloads are fully
   schema-aware (direct feb_cbor_decode_map_header/_text/_uint calls, same as
   feb_cbor_decode_wifi_scan_ap) and never call feb_cbor_skip_value() themselves -- the
   depth accounting above belongs entirely to the one generic skip_value() call already
   made by feb_cbor_decode_status_payload() to capture `result`'s span in the first place,
   not to any recursion introduced by this section. */
#define FEB_WARDRIVING_MAX_RECORDS_PER_BATCH FEB_CBOR_MAX_ARRAY_ENTRIES

typedef struct {
    feb_wardriving_record_t records[FEB_WARDRIVING_MAX_RECORDS_PER_BATCH];
    size_t record_count;
    uint64_t backlog_remaining;
} feb_wardriving_status_result_payload_t;

size_t feb_cbor_encode_wardriving_status_result_payload(uint8_t *out, size_t out_cap, const feb_wardriving_status_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_wardriving_status_result_payload(const uint8_t *in, size_t in_len, feb_wardriving_status_result_payload_t *payload);

#endif /* FEB_CBOR_WARDRIVING_H */

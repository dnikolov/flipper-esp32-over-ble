/* Split out of cbor_codec.h (docs/OPTIMIZATION.md item 1) -- shared contract, mirrored
   byte-for-byte in flipper/cbor_wardriving.h. Changes here must be mirrored there and in
   docs/PROTOCOL.md, or the two firmwares diverge. Included transitively via cbor_codec.h;
   nothing outside the codec split should need to include this directly.

   Depends on cbor_wifi_scan.h (FEB_WIFI_SCAN_BSSID_LEN, FEB_WIFI_SCAN_SSID_MAX_LEN) and
   cbor_ble_scan.h (FEB_BLE_SCAN_ADDRESS_LEN, FEB_BLE_SCAN_NAME_MAX_LEN) -- wardriving's
   wifi/ble record payloads reuse those exact-length/max-length constants directly rather
   than redefining them, so this is a real (not incidental) cross-section header dependency.

   ---- `wardriving`-specific `command.arguments`, `status.result`, and `<wardriving-record>`
   (docs/PROTOCOL.md "`wardriving` command and status payloads") ----

   `command.arguments` for wardriving: `feb_wardriving_command_payload_t`. Field order:
   action, sources, wifi_interval_ms, ble_window_ms, ble_interval_ms, wifi_swelling, country
   (wifi_swelling/country added 2026-09-21, docs/WARDRIVING_REDESIGN.md). Only `action` is
   unconditionally required by this decoder; PROTOCOL.md's action-dependent presence rules
   ("sources required for start, absent for stop"; "wifi_interval_ms required when \"wifi\"
   in sources"; "wifi_swelling/country required when \"wifi\" in sources", etc.) are NOT
   enforced here -- same split established by wifi_scan's
   command payload (see feb_cbor_decode_command_payload's non-empty-arguments test in
   tests/esp32/test_framing_cbor.c): this decoder validates each *present* field's own
   shape/type and the fixed field order, and reports which fields were present via
   has_sources/has_wifi_interval_ms/has_ble_params; the caller (main.c's future
   handle_wardriving_command(), not part of this change) is responsible for the
   action-dependent cross-field validation and for mapping a violation to
   `invalid_command`. This split is a deliberate implementation choice for this codec --
   record it here so the Flipper-side implementer's decoder matches (same layer choice),
   not a PROTOCOL.md requirement either way. The one exception is ble_window_ms/
   ble_interval_ms pairing itself (see has_ble_params below), which IS enforced here
   since PROTOCOL.md states the two are always present or absent together -- unlike the
   action-dependent rules, that isn't a caller-conditional rule needing scan-state
   context, just a structural invariant of the pair.

   `sources` is capped at FEB_WARDRIVING_MAX_SOURCES entries (2 today: "wifi"/"ble" are the
   only two sources this protocol defines) -- a third array entry is rejected as
   FEB_CBOR_ERR_TOO_MANY_ENTRIES (structural: no combination of currently-defined sources
   can legally exceed 2), but the *values* of present entries are not restricted to
   "wifi"/"ble" by this decoder, matching addr_type/phy/auth's caller-owned-text treatment
   in cbor_wifi_scan.h/cbor_ble_scan.h -- board-capability validation ("a source the board
   does not advertise") is a caller concern.

   `ble_window_ms`/`ble_interval_ms` are collapsed into a single `has_ble_params` flag
   (not two independent has_ble_window_ms/has_ble_interval_ms flags) because
   docs/PROTOCOL.md states the two fields are always required together -- one present
   without the other is not a representable state, so unlike the action-dependent
   presence rules above (deferred to the caller), this pairing IS enforced by this
   decoder: seeing exactly one of the two keys is FEB_CBOR_ERR_MISSING_FIELD. */
#ifndef FEB_CBOR_WARDRIVING_H
#define FEB_CBOR_WARDRIVING_H

#include "cbor_codec.h"
#include "cbor_wifi_scan.h"
#include "cbor_ble_scan.h"

#define FEB_WARDRIVING_ACTION_MAX_LEN FEB_CBOR_MAX_TEXT_LEN
#define FEB_WARDRIVING_SOURCE_MAX_LEN FEB_CBOR_MAX_TEXT_LEN
#define FEB_WARDRIVING_MAX_SOURCES 2u
#define FEB_WARDRIVING_SWELLING_MAX_LEN FEB_CBOR_MAX_TEXT_LEN
#define FEB_WARDRIVING_COUNTRY_MAX_LEN FEB_CBOR_MAX_TEXT_LEN

typedef struct {
    const char *action;
    size_t action_len;

    const char *sources[FEB_WARDRIVING_MAX_SOURCES];
    size_t source_lens[FEB_WARDRIVING_MAX_SOURCES];
    size_t source_count;
    int has_sources;

    uint64_t wifi_interval_ms;
    int has_wifi_interval_ms;

    uint64_t ble_window_ms;
    uint64_t ble_interval_ms;
    int has_ble_params; /* ble_window_ms/ble_interval_ms are always present or absent
                            together per docs/PROTOCOL.md */

    const char *wifi_swelling; /* "normal" | "aggressive" | "speed_based" */
    size_t wifi_swelling_len;
    int has_wifi_swelling;

    const char *country; /* "BG" | "RoW" */
    size_t country_len;
    int has_country;
} feb_wardriving_command_payload_t;

size_t feb_cbor_encode_wardriving_command_payload(uint8_t *out, size_t out_cap, const feb_wardriving_command_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_wardriving_command_payload(const uint8_t *in, size_t in_len, feb_wardriving_command_payload_t *payload);

/* `<wardriving-record>` fixed field order: timestamp_ms, utc_timestamp_s, lat_e7_offset,
   lon_e7_offset, source, payload -- matches PROTOCOL.md's table exactly (utc_timestamp_s
   added 2026-09-12, "Real GPS driver, wardriving fix-dependency, and real wardriving-record
   timestamps": Unix epoch seconds derived from the most recent valid RMC sentence; always
   present and valid on every logged record since a record is only ever logged while the
   location driver reports state = "fix", which requires a valid RMC alongside the valid GGA
   -- see location.h). `payload`'s shape depends on
   `source` ("wifi" -> ssid/bssid/rssi_offset/channel/auth; "ble" -> address/name
   (optional)/rssi_offset) -- modeled here as a tagged union: `payload_kind` discriminates
   which of `payload.wifi`/`payload.ble` is valid. `source`/`source_len` are populated by
   the decoder as raw aliases into the input buffer (same convention as `state`/`phy`/
   `auth` elsewhere in the split codec) for round-trip/inspection purposes, but the ENCODER
   derives the wire `source` string solely from `payload_kind`, not from `source`/
   `source_len` -- this makes an encode-time mismatch between the two structurally
   impossible rather than a caller obligation to keep them in sync.

   Nesting depth: this shape is reached via `status.result`'s own fresh depth-0
   feb_cbor_skip_value() budget (result -> records array -> record map -> payload map ->
   payload's own scalar fields = 4 container levels, exactly FEB_CBOR_MAX_NESTING) when
   status_payload's generic decoder captures+validates the whole `result` span generically
   -- see docs/PROTOCOL.md's "Nesting depth" section. This module's own
   feb_cbor_decode_wardriving_record()/feb_cbor_decode_wardriving_status_result_payload()
   below, however, never call feb_cbor_skip_value() themselves -- like
   feb_cbor_decode_wifi_scan_ap()/_result_payload() (cbor_wifi_scan.h) above, they are fully
   schema-aware and decode every level (record, payload) via direct
   feb_cbor_decode_map_header()/_text()/_bytes()/_uint() calls, so the depth-budget question
   only matters for the generic validation pass that already happens once at
   feb_cbor_decode_status_payload() (cbor_records.h), not anywhere in this wardriving-specific
   code. Confirmed against a host-native round-trip test (tests/esp32/test_framing_cbor.c)
   rather than assumed -- the 4-level shape held without needing the payload_kind-flattening
   fallback PROTOCOL.md anticipates. */
#define FEB_WARDRIVING_WIFI_SSID_MAX_LEN FEB_WIFI_SCAN_SSID_MAX_LEN
#define FEB_WARDRIVING_BLE_ADDRESS_LEN FEB_BLE_SCAN_ADDRESS_LEN
#define FEB_WARDRIVING_BLE_NAME_MAX_LEN FEB_BLE_SCAN_NAME_MAX_LEN
#define FEB_WARDRIVING_MAX_RECORDS_PER_BATCH FEB_CBOR_MAX_ARRAY_ENTRIES

typedef enum {
    FEB_WARDRIVING_PAYLOAD_WIFI = 0,
    FEB_WARDRIVING_PAYLOAD_BLE = 1,
} feb_wardriving_payload_kind_t;

typedef struct {
    const uint8_t *ssid; /* 0..FEB_WARDRIVING_WIFI_SSID_MAX_LEN bytes; not guaranteed UTF-8 */
    size_t ssid_len;
    uint8_t bssid[FEB_WIFI_SCAN_BSSID_LEN];
    uint64_t rssi_offset; /* rssi_dbm + 128 */
    uint64_t channel;
    const char *auth;
    size_t auth_len;
} feb_wardriving_wifi_payload_t;

typedef struct {
    uint8_t address[FEB_WARDRIVING_BLE_ADDRESS_LEN];
    const char *name; /* NULL if has_name is 0 */
    size_t name_len;
    int has_name;
    uint64_t rssi_offset; /* rssi_dbm + 128 */
} feb_wardriving_ble_payload_t;

typedef struct {
    uint64_t timestamp_ms;
    uint64_t utc_timestamp_s;
    uint64_t lat_e7_offset;
    uint64_t lon_e7_offset;
    const char *source; /* raw wire text ("wifi"/"ble"), decoder-only -- see comment above */
    size_t source_len;
    feb_wardriving_payload_kind_t payload_kind;
    union {
        feb_wardriving_wifi_payload_t wifi;
        feb_wardriving_ble_payload_t ble;
    } payload;
} feb_wardriving_record_t;

/* Array-element decode convention (bytes consumed via *status), matching
   feb_cbor_decode_wifi_scan_ap()/feb_cbor_decode_ble_scan_device() (cbor_wifi_scan.h/
   cbor_ble_scan.h) above. */
size_t feb_cbor_encode_wardriving_record(uint8_t *out, size_t out_cap, const feb_wardriving_record_t *record);
size_t feb_cbor_decode_wardriving_record(const uint8_t *in, size_t in_len, feb_wardriving_record_t *record, feb_cbor_status_t *status);

typedef struct {
    feb_wardriving_record_t records[FEB_WARDRIVING_MAX_RECORDS_PER_BATCH];
    size_t record_count;
    uint64_t backlog_remaining;
} feb_wardriving_status_result_payload_t;

size_t feb_cbor_encode_wardriving_status_result_payload(uint8_t *out, size_t out_cap, const feb_wardriving_status_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_wardriving_status_result_payload(const uint8_t *in, size_t in_len, feb_wardriving_status_result_payload_t *payload);

#endif /* FEB_CBOR_WARDRIVING_H */

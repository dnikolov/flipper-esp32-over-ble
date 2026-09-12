/* Shared contract, mirrored byte-for-byte in esp32/main/cbor_gps.h (added there first,
   2026-09-12 frozen design session, docs/PLAN.md "Real GPS driver, wardriving
   fix-dependency, and real wardriving-record timestamps"). Changes here must be mirrored
   there and in docs/PROTOCOL.md, or the two firmwares diverge. Included by the umbrella
   cbor_codec.h, which must be included first (directly or transitively) so that
   feb_cbor_status_t is already visible; not meant to be included standalone. Now part of
   the cross-firmware shared-header contract check (tools/check_shared_headers.py's
   HEADER_PAIRS) -- that script only diffs macros/prototypes, not struct bodies, so the
   field list below was checked by hand against esp32/main/cbor_gps.h, not just by a clean
   script run.

   ---- `gps`-specific `status.result` map (docs/PROTOCOL.md "`gps` command and status
   payloads") ----
   Present only when `status.state == "fix"`. Field order: lat_e7_offset, lon_e7_offset,
   fix_quality, satellites, hdop_e1, utc_timestamp_s, altitude_dm_offset -- matches
   PROTOCOL.md exactly. `altitude_dm_offset` was appended after the original six fields
   (added 2026-09-12, same design session) rather than inserted next to lat/lon_e7_offset,
   since this codec's field order is meaningful (the decoder below rejects out-of-order
   keys) and appending is the minimal, additive change. Unlike wifi_scan/ble_scan/
   wardriving's own `result` maps, `gps` has no per-item array wrapper -- there is exactly
   one flat set of scalar fields, so this struct/codec pair encodes/decodes `status.result`
   directly rather than an inner named array field. The caller only invokes the decoder
   below when `status.has_result` is set (state == "fix"), matching every other
   capability's own has_result convention -- there is nothing to decode for "no_signal"/
   "acquiring". */
#ifndef FEB_CBOR_GPS_H
#define FEB_CBOR_GPS_H

/* Added to a signed altitude_dm value (decimeters) before encoding as an unsigned CBOR
   integer, and subtracted back out after decoding. 1,000,000 dm (+-100,000.0 m) is a
   generous symmetric bound comfortably beyond the u-blox NEO-6 module's documented
   operational altitude limit (50,000 m) in either direction, mirroring lat_e7_offset/
   lon_e7_offset's offset-by-max-magnitude convention above. */
#define FEB_GPS_ALTITUDE_DM_OFFSET 1000000LL

typedef struct {
    uint64_t lat_e7_offset;   /* same encoding as <wardriving-record>'s own field */
    uint64_t lon_e7_offset;   /* same encoding as <wardriving-record>'s own field */
    uint64_t fix_quality;     /* raw NMEA GGA fix-quality value, passed through as-is */
    uint64_t satellites;      /* GGA satellite-in-use count */
    uint64_t hdop_e1;         /* GGA HDOP, scaled by 10 and truncated to an integer */
    uint64_t utc_timestamp_s; /* Unix epoch seconds derived from the most recent valid RMC */
    uint64_t altitude_dm_offset; /* GGA MSL altitude, decimeters, offset per FEB_GPS_ALTITUDE_DM_OFFSET above */
} feb_gps_result_payload_t;

size_t feb_cbor_encode_gps_result_payload(uint8_t *out, size_t out_cap, const feb_gps_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_gps_result_payload(const uint8_t *in, size_t in_len, feb_gps_result_payload_t *payload);

#endif /* FEB_CBOR_GPS_H */

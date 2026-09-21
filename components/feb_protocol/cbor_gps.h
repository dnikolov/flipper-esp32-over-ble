/* Split out of cbor_codec.h (docs/OPTIMIZATION.md item 1 pattern) -- shared contract, to be
   mirrored byte-for-byte in flipper/cbor_gps.h once the Flipper side implements the `gps`
   capability (docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and real
   wardriving-record timestamps", design frozen 2026-09-12). Changes here must be mirrored
   there and in docs/PROTOCOL.md, or the two firmwares diverge. Included transitively via
   cbor_codec.h; nothing outside the codec split should need to include this directly.

   ---- `gps`-specific `status.result` map (docs/PROTOCOL.md "`gps` command and status
   payloads") ----
   Field order: lat_e7_offset, lon_e7_offset, fix_quality, satellites, hdop_e1,
   utc_timestamp_s, altitude_dm_offset -- matches PROTOCOL.md's table exactly. `altitude_dm_offset`
   was appended after the original six fields (added 2026-09-12, same design session) rather
   than inserted next to lat/lon_e7_offset, since this codec's field order is meaningful (the
   decoder below rejects out-of-order keys) and appending is the minimal, additive change.
   `lat_e7_offset`/`lon_e7_offset` use the exact same offset-encoding as <wardriving-record>'s
   fields (cbor_wardriving.h); callers convert to/from a real signed value themselves.
   `altitude_dm_offset` uses its own offset (see FEB_GPS_ALTITUDE_DM_OFFSET below) since
   altitude has no natural bounded range like latitude/longitude. This map is present only
   when the `gps` capability's `status.state` is `"fix"` -- main.c omits the whole `result`
   field (status.has_result = 0) for `"no_signal"`/`"acquiring"`, matching wifi_scan/ble_scan's
   optional-result convention (cbor_records.h's feb_status_payload_t). `gps`'s `command`
   takes no capability-specific payload beyond the generic empty `arguments` map, so this
   header defines no command-payload type. */
#ifndef FEB_CBOR_GPS_H
#define FEB_CBOR_GPS_H

#include "cbor_codec.h"

/* Added to a signed altitude_dm value (decimeters) before encoding as an unsigned CBOR
   integer, and subtracted back out after decoding. 1,000,000 dm (+-100,000.0 m) is a
   generous symmetric bound comfortably beyond the u-blox NEO-6 module's documented
   operational altitude limit (50,000 m) in either direction, mirroring lat_e7_offset/
   lon_e7_offset's offset-by-max-magnitude convention above. */
#define FEB_GPS_ALTITUDE_DM_OFFSET 1000000LL

typedef struct {
    uint64_t lat_e7_offset;
    uint64_t lon_e7_offset;
    uint64_t fix_quality;
    uint64_t satellites;
    uint64_t hdop_e1;
    uint64_t utc_timestamp_s;
    uint64_t altitude_dm_offset;
} feb_gps_result_payload_t;

size_t feb_cbor_encode_gps_result_payload(uint8_t *out, size_t out_cap, const feb_gps_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_gps_result_payload(const uint8_t *in, size_t in_len, feb_gps_result_payload_t *payload);

#endif /* FEB_CBOR_GPS_H */

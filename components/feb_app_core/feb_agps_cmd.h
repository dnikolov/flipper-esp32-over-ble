#ifndef FEB_AGPS_CMD_H
#define FEB_AGPS_CMD_H

/* ESP-side codec for the `agps` command arguments and the `complete` result map
   (docs/PROTOCOL.md "`agps` command and status payloads"). Pure, host-tested. Lives here, not
   in components/feb_protocol, because the Flipper only encodes the command and decodes the
   result with its own code; there is no mirrored header pair to keep in sync. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "feb_agps_casic.h"

/* Plausible host RTC window: 2023-11-14 .. 2096-10-02. A Flipper with an unset RTC fails. */
#define FEB_AGPS_UTC_MIN 1700000000ull
#define FEB_AGPS_UTC_MAX 4000000000ull
#define FEB_AGPS_LAT_E7_OFFSET_MAX 1800000000ull
#define FEB_AGPS_LON_E7_OFFSET_MAX 3600000000ull
#define FEB_AGPS_ALT_DM_OFFSET_MAX 2000000ull
#define FEB_AGPS_ALT_DM_OFFSET 1000000

typedef struct {
    uint64_t utc;
    uint32_t total;
    uint32_t off;
    const uint8_t *data; /* aliases the input span */
    size_t data_len;
    feb_agps_position_t position;
} feb_agps_load_args_t;

/* arguments = {action:"load", utc, total, off, data [, lat_e7_offset, lon_e7_offset,
   altitude_dm_offset]} in exactly that key order; the position triple is all-or-none. Rejects
   anything else, including trailing bytes. Chunk-size/offset rules are the blob module's job. */
feb_cbor_status_t feb_agps_decode_load_args(const uint8_t *in, size_t in_len, feb_agps_load_args_t *out);

/* result = {sent, acked, nacked, timeout}; returns bytes written or 0 if `out_cap` is too small. */
size_t feb_agps_encode_result(uint8_t *out, size_t out_cap, const feb_agps_result_t *result);

#endif

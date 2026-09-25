/* Pure, zero-ESP-IDF-dependency NMEA-0183 GGA/RMC sentence parsing, underneath location.c's
   UART-driven GPS driver (docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and real
   wardriving-record timestamps"). Kept in its own translation unit, with no dependency on
   driver/uart.h or FreeRTOS, so tests/esp32/test_location.c can validate parsing/checksum
   logic on the host without a UART peripheral -- same split as
   wardriving_record_format.h/wardriving_log.c. Not part of the shared ESP32/Flipper protocol
   contract (NMEA sentences never cross the BLE wire; only the derived fix values in
   location.h/cbor_gps.h do).

   Only GGA (fix quality/satellite count/HDOP/lat-lon/altitude) and RMC (date+time, status,
   and speed-over-ground, used for utc_timestamp_s and speed_e1_kmh) are parsed -- see
   docs/PLAN.md's design decision 2 for why ZDA was rejected and VTG is not parsed. RMC's
   course field exists on the wire but is intentionally never extracted here (backlogged, not
   in this design's scope; see docs/WARDRIVING_REDESIGN.md for the speed field's addition). */
#ifndef FEB_NMEA_PARSER_H
#define FEB_NMEA_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if `line` (no trailing CR/LF) is a `$`-prefixed sentence whose checksum (the two hex
   digits after `*`) matches the XOR of every byte between `$` and `*`. Used by location.c to
   detect "valid NMEA traffic ever seen" (docs/PROTOCOL.md's `no_signal` vs `acquiring`
   distinction) independent of sentence type, since a talker can emit sentences this module
   doesn't otherwise parse (e.g. `VTG`). */
bool nmea_checksum_valid(const char *line, size_t line_len);

typedef struct {
    uint32_t fix_quality; /* raw GGA field, 0 = no fix, passed through uncollapsed */
    uint32_t satellites;
    uint32_t hdop_e1; /* HDOP * 10, truncated -- e.g. 2.3 -> 23 */
    int32_t lat_e7;   /* 0 when fix_quality == 0 (GGA omits position fields) */
    int32_t lon_e7;
    int32_t altitude_dm; /* GGA MSL altitude (field 9) * 10, truncated, sign preserved -- e.g.
                            52.9 -> 529, -3.2 -> -32; 0 when the field is empty (no fix) */
} nmea_gga_t;

/* Parses one GGA sentence (any 2-letter talker ID, e.g. $GNGGA/$GPGGA). Returns false (and
   leaves *out untouched) for a checksum failure, a non-GGA sentence, or a structurally
   malformed field -- true otherwise, including the fix_quality == 0 "no fix yet" case (that
   is a valid, parseable sentence, just not a fix). */
bool nmea_parse_gga(const char *line, size_t line_len, nmea_gga_t *out);

typedef struct {
    bool status_active; /* true iff the status field is 'A' (valid); false for 'V' (void) */
    uint8_t hour, minute, second;
    uint8_t day, month;
    uint8_t year_2digit; /* add 2000 -- GPS did not exist before 2000, and this project's
                            operating dates are decades past that */
    uint32_t speed_knots_e1; /* speed over ground, knots * 10, truncated -- e.g. 2.3 -> 23;
                                0 when the field is empty (matches parse_x10_field's existing
                                empty-field convention, same as GGA's hdop_e1) */
} nmea_rmc_t;

/* Parses one RMC sentence. Returns false for a checksum failure, a non-RMC sentence, or a
   malformed status/time/date field -- true otherwise (including status_active == false,
   "void" fix). RMC's course field is structurally present on the wire but never extracted
   here (docs/PLAN.md's scope boundary); speed-over-ground (field index 6) is parsed into
   speed_knots_e1. */
bool nmea_parse_rmc(const char *line, size_t line_len, nmea_rmc_t *out);

/* Unix epoch seconds for an RMC sentence's date+time fields, treated as UTC (NMEA's own
   convention). Only meaningful when `rmc->status_active` is true -- callers must check that
   themselves; this function does no validity gating of its own. */
uint64_t nmea_rmc_to_unix_time(const nmea_rmc_t *rmc);

#endif /* FEB_NMEA_PARSER_H */

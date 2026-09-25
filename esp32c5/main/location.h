/* Location-source interface (docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and
   real wardriving-record timestamps", design frozen 2026-09-12; ported onto this board
   2026-09-25 per docs/hardware/olimex-mod-esp32-c5/README.md's GPIO4/GPIO5-wired ATGM336H GPS
   module). Backed by a real UART/NMEA-0183 GPS module (location.c opens a UART port and runs a
   dedicated background parse task built on nmea_parser.c/.h's pure sentence parsing).
   Consumed only by the `gps` capability's handle_gps_command() here -- wardriving has not been
   ported to this board, so there is no record-capture call site yet. Identical shape to
   esp32/main/location.h and heltec/main/location.h; only location.c's compile-time pin/UART-
   port constants differ per board. */
#ifndef FEB_LOCATION_H
#define FEB_LOCATION_H

#include <stdbool.h>
#include <stdint.h>

/* Matches docs/PROTOCOL.md's `gps` status `state` field exactly. no_signal: no NMEA byte
   ever received since boot. acquiring: valid NMEA traffic seen, but the most recent GGA
   fix quality is 0 and/or the most recent RMC status is not 'A'. fix: both conditions hold
   -- location_get_fix()'s *out is only meaningful in this state. */
typedef enum {
    FEB_LOCATION_NO_SIGNAL = 0,
    FEB_LOCATION_ACQUIRING = 1,
    FEB_LOCATION_FIX = 2,
} feb_location_state_t;

typedef struct {
    int32_t lat_e7;         /* latitude * 1e7 */
    int32_t lon_e7;         /* longitude * 1e7 */
    uint32_t fix_quality;   /* raw GGA fix-quality value, uncollapsed */
    uint32_t satellites;    /* GGA satellites-in-use count */
    uint32_t hdop_e1;       /* GGA HDOP * 10, truncated */
    int32_t altitude_dm;    /* GGA MSL altitude * 10, truncated, sign preserved */
    uint64_t utc_timestamp_s; /* Unix epoch seconds from the most recent valid RMC */
    uint32_t speed_e1_kmh;  /* ground speed, km/h * 10, truncated, from the most recent valid
                               RMC's speed-over-ground (knots) * 1.852 -- integer-only:
                               (speed_knots_e1 * 1852) / 1000. Same gating as utc_timestamp_s. */
} feb_location_t;

/* Opens the GPS UART (this board: UART1, RX=GPIO4, TX=GPIO5, 9600 8N1, no flow control --
   compile-time constant, see location.c) and starts a dedicated background parse task, RX-only
   (never transmits to the module). A UART/task-start failure is logged and leaves the driver
   permanently in FEB_LOCATION_NO_SIGNAL -- GPS is optional peripheral hardware, not required
   for the board to boot. */
void location_init(void);

/* Always call this fresh per `gps` command reply, never cache the result at a longer-lived
   scope -- the underlying fix changes continuously and readers must see the current state,
   not a snapshot from an earlier point in time. Returns the current 3-state status; *out is
   populated with the latest known values whenever state is not FEB_LOCATION_NO_SIGNAL
   (harmless zeros otherwise), but only state == FEB_LOCATION_FIX means *out represents an
   actual satellite fix -- callers gating on "do we have a fix" must check the returned
   state, not merely whether *out was written. Thread-safe: takes a snapshot of the
   background parse task's state under a short critical section. */
feb_location_state_t location_get_fix(feb_location_t *out);

#endif /* FEB_LOCATION_H */

#include "location.h"

/* docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub reorder": this is the deferred-GPS
   swap point. A real implementation will replace this file's body with a UART-based
   NMEA-0183 parser (GY-NEO6MV2/NEO-6M) -- location_init() will open the UART and start a
   background parse task there; location_get_fix() will read the most recently parsed fix
   instead of returning a constant. Callers (wardriving_log.c / handle_ble_scan_command()
   etc.) must not change: this header's shape already carries has_fix for exactly the
   "discard until first fix" feature that's backlogged for the real driver. */

void location_init(void)
{
    /* No-op: nothing to open/start until a real GPS UART driver replaces this file. */
}

bool location_get_fix(feb_location_t *out)
{
    /* Placeholder coordinate, NOT a real fix: Null Island (0 deg N, 0 deg E), the
       conventional GIS "obviously fake" location (intersection of the equator and prime
       meridian, open ocean, no actual survey point). Every call returns the same constant
       until this file is replaced by a real NMEA-parsing driver -- callers must re-fetch
       per record rather than caching, exactly so that swap requires no call-site changes. */
    out->lat_e7 = 0;
    out->lon_e7 = 0;
    out->has_fix = true;
    return true;
}

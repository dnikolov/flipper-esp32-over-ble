/* Location-source interface (docs/PLAN.md "`ble_scan`, `wardriving`, and the GPS-stub
   reorder"). Today's implementation is a fixed-coordinate stub; a real GPS/NMEA driver is
   the deferred swap this interface exists to make a body-only change (see location.c). */
#ifndef FEB_LOCATION_H
#define FEB_LOCATION_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t lat_e7; /* latitude * 1e7 */
    int32_t lon_e7; /* longitude * 1e7 */
    bool has_fix;
} feb_location_t;

void location_init(void);

/* Always call this fresh per captured record, never cache the result at a longer-lived
   scope -- see location.c's comment on why. Returns true when *out was populated (today,
   always true from the stub; a real driver may legitimately return false before its first
   fix). */
bool location_get_fix(feb_location_t *out);

#endif /* FEB_LOCATION_H */

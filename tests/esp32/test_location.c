/* Host-native test driver for esp32/main/location.c, compiled directly (not a copy). This
   module has no BLE/flash dependency, so unlike test_framing_cbor.c it needs no shared
   vectors -- just confirms the stub returns a stable, in-range coordinate. */
#include <stdio.h>

#include "location.h"

static int g_failures = 0;

static void check(int condition, const char *name)
{
    if (condition) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        g_failures++;
    }
}

int main(void)
{
    feb_location_t first;
    feb_location_t second;

    location_init();

    check(location_get_fix(&first), "location_get_fix returns true");
    check(first.has_fix, "location_get_fix reports has_fix");
    check(first.lat_e7 >= -900000000 && first.lat_e7 <= 900000000,
          "lat_e7 within +-90 degrees (e7 scale)");
    check(first.lon_e7 >= -1800000000 && first.lon_e7 <= 1800000000,
          "lon_e7 within +-180 degrees (e7 scale)");

    location_get_fix(&second);
    check(first.lat_e7 == second.lat_e7 && first.lon_e7 == second.lon_e7 &&
          first.has_fix == second.has_fix,
          "consecutive calls return a stable coordinate");

    printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}

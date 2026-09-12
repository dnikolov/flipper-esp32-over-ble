/* Host-native test driver for esp32/main/nmea_parser.c -- the pure, zero-ESP-IDF-dependency
   NMEA-0183 GGA/RMC sentence parser underneath location.c's real UART-driven GPS driver
   (docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and real wardriving-record
   timestamps"). Retained under this file's original name/location (it used to test
   location.c's fixed-coordinate stub directly) -- location.c itself now depends on
   driver/uart.h and FreeRTOS and is not host-buildable, same treatment as
   wardriving_log.c/wardriving_record_format.c's split. Sentences below are taken verbatim
   from tools/gps_antenna_last_run.log, a real hardware-captured NMEA sample. */
#include <stdio.h>
#include <string.h>

#include "nmea_parser.h"

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
    /* Real captured sentences (tools/gps_antenna_last_run.log), a valid 3D fix. */
    static const char gga_fix[] = "$GNGGA,091010.000,4242.30373,N,02742.79804,E,1,09,2.0,52.9,M,0.0,M,,*4B";
    static const char rmc_fix[] = "$GNRMC,091010.000,A,4242.30373,N,02742.79804,E,0.00,0.00,120926,,,A*72";
    /* Same GGA with the checksum's last hex digit flipped. */
    static const char gga_bad_checksum[] = "$GNGGA,091010.000,4242.30373,N,02742.79804,E,1,09,2.0,52.9,M,0.0,M,,*4A";
    /* Synthetic "no fix yet" GGA (quality 0, empty lat/lon) and "void" RMC (status V) --
       shape a real module emits before its first fix, not captured verbatim but matching
       the NMEA 0183 spec's field layout for this case. */
    static const char gga_no_fix[] = "$GNGGA,091010.000,,,,,0,00,,,M,,M,,*6F";
    static const char rmc_void[] = "$GNRMC,091010.000,V,,,,,0.00,0.00,120926,,,N*54";
    /* Synthetic negative altitude (below mean sea level) -- not in the captured log, but
       structurally valid per the NMEA 0183 spec; checksum hand-computed. */
    static const char gga_negative_altitude[] =
        "$GNGGA,091010.000,4242.30373,N,02742.79804,E,1,09,2.0,-3.2,M,0.0,M,,*59";
    /* A VTG sentence (not parsed for data, but a valid NMEA sentence -- used to prove
       nmea_checksum_valid() is type-agnostic). */
    static const char vtg[] = "$GNVTG,0.00,T,,M,0.00,N,0.00,K,A*23";

    nmea_gga_t gga;
    nmea_rmc_t rmc;
    int ok;

    check(nmea_checksum_valid(gga_fix, strlen(gga_fix)), "checksum valid: real captured GGA");
    check(nmea_checksum_valid(vtg, strlen(vtg)), "checksum valid: real captured VTG (type-agnostic)");
    check(!nmea_checksum_valid(gga_bad_checksum, strlen(gga_bad_checksum)),
          "checksum invalid: flipped final hex digit rejected");

    ok = nmea_parse_gga(gga_fix, strlen(gga_fix), &gga);
    ok = ok && gga.fix_quality == 1 && gga.satellites == 9 && gga.hdop_e1 == 20;
    check(ok, "GGA fix: parses fix_quality/satellites/hdop_e1");
    check(gga.altitude_dm == 529, "GGA fix: altitude 52.9m matches parsed altitude_dm");
    /* 42 deg 42.30373' N -> 42 + 42.30373/60 = 42.705062166...deg -> e7 42705062 (rounded). */
    check(gga.lat_e7 == 427050622, "GGA fix: latitude matches hand-computed e7 value");
    /* 027 deg 42.79804' E -> 27 + 42.79804/60 = 27.7133006666...deg -> e7 277133007 (rounded). */
    check(gga.lon_e7 == 277133007, "GGA fix: longitude matches hand-computed e7 value");

    ok = nmea_parse_rmc(rmc_fix, strlen(rmc_fix), &rmc);
    ok = ok && rmc.status_active && rmc.hour == 9 && rmc.minute == 10 && rmc.second == 10;
    ok = ok && rmc.day == 12 && rmc.month == 9 && rmc.year_2digit == 26;
    check(ok, "RMC fix: parses status/time/date");
    /* 2026-09-12 09:10:10 UTC. */
    check(nmea_rmc_to_unix_time(&rmc) == 1789204210ULL, "RMC fix: unix time matches hand-computed value");

    ok = nmea_parse_gga(gga_no_fix, strlen(gga_no_fix), &gga);
    ok = ok && gga.fix_quality == 0 && gga.lat_e7 == 0 && gga.lon_e7 == 0 && gga.altitude_dm == 0;
    check(ok, "GGA no-fix: parses structurally OK with fix_quality 0, zeroed coordinates/altitude");

    ok = nmea_parse_gga(gga_negative_altitude, strlen(gga_negative_altitude), &gga);
    ok = ok && gga.altitude_dm == -32;
    check(ok, "GGA negative altitude: -3.2m matches parsed altitude_dm");

    ok = nmea_parse_rmc(rmc_void, strlen(rmc_void), &rmc);
    ok = ok && !rmc.status_active;
    check(ok, "RMC void: parses structurally OK with status_active false");

    check(!nmea_parse_gga(vtg, strlen(vtg), &gga), "GGA parser rejects a VTG sentence");
    check(!nmea_parse_rmc(gga_fix, strlen(gga_fix), &rmc), "RMC parser rejects a GGA sentence");
    check(!nmea_parse_gga(gga_bad_checksum, strlen(gga_bad_checksum), &gga),
          "GGA parser rejects a checksum failure");

    printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}

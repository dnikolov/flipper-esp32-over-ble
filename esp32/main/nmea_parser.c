#include "nmea_parser.h"

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool nmea_checksum_valid(const char *line, size_t line_len)
{
    uint8_t computed = 0;
    size_t i;
    size_t star = 0;
    bool found_star = false;

    if (line == NULL || line_len < 4 || line[0] != '$') {
        return false;
    }
    for (i = 1; i < line_len; i++) {
        if (line[i] == '*') {
            star = i;
            found_star = true;
            break;
        }
        computed ^= (uint8_t)line[i];
    }
    if (!found_star || star + 2 >= line_len) {
        return false;
    }
    {
        int hi = hex_val(line[star + 1]);
        int lo = hex_val(line[star + 2]);

        if (hi < 0 || lo < 0) {
            return false;
        }
        return ((uint8_t)((hi << 4) | lo)) == computed;
    }
}

static bool sentence_is(const char *line, size_t line_len, const char *type3)
{
    if (line_len < 6 || line[0] != '$') {
        return false;
    }
    return line[3] == type3[0] && line[4] == type3[1] && line[5] == type3[2];
}

/* Returns the `field_index`-th comma-separated data field (0-based, counting from just after
   the `$aaccc` sentence-id token) via *out and *out_len, stopping at `*` (checksum
   delimiter) or end of line. Returns false if the line has fewer than field_index+1 data
   fields. An empty field (two adjacent commas) is a valid, zero-length result, not a failure
   -- GGA/RMC both legitimately omit fields (e.g. lat/lon while fix_quality is 0). */
static bool nmea_field(const char *line, size_t line_len, size_t field_index,
                       const char **out, size_t *out_len)
{
    size_t i;
    size_t field = 0;
    size_t start = 0;

    for (i = 0; i <= line_len; i++) {
        bool sep = (i == line_len) || line[i] == ',' || line[i] == '*';

        if (!sep) {
            continue;
        }
        if (field == field_index + 1) {
            *out = line + start;
            *out_len = i - start;
            return true;
        }
        field++;
        start = i + 1;
        if (i < line_len && line[i] == '*') {
            break;
        }
    }
    return false;
}

static uint32_t parse_uint_field(const char *f, size_t flen)
{
    uint32_t value = 0;
    size_t i;

    for (i = 0; i < flen; i++) {
        if (!is_digit(f[i])) {
            return 0;
        }
        value = value * 10u + (uint32_t)(f[i] - '0');
    }
    return value;
}

/* "D.d" (at least one integer digit, optional fractional part) -> value*10, truncated -- e.g.
   "2.3" -> 23, "12" -> 120. Returns 0 for an empty or malformed field rather than failing the
   whole sentence, since HDOP is supplementary (GGA's structural validity doesn't hinge on it). */
static uint32_t parse_x10_field(const char *f, size_t flen)
{
    size_t dot = flen;
    size_t i;
    uint32_t int_part = 0;
    uint32_t frac_digit = 0;

    if (flen == 0) {
        return 0;
    }
    for (i = 0; i < flen; i++) {
        if (f[i] == '.') {
            dot = i;
            break;
        }
    }
    for (i = 0; i < dot; i++) {
        if (!is_digit(f[i])) {
            return 0;
        }
        int_part = int_part * 10u + (uint32_t)(f[i] - '0');
    }
    if (dot < flen - 1 && is_digit(f[dot + 1])) {
        frac_digit = (uint32_t)(f[dot + 1] - '0');
    }
    return int_part * 10u + frac_digit;
}

/* Same shape as parse_x10_field(), but accepts an optional leading '-' -- GGA's altitude field
   (unlike HDOP) can be negative (below mean sea level, e.g. -3.2). Returns 0 for an empty or
   malformed field, same non-failing convention as parse_x10_field(). */
static int32_t parse_signed_x10_field(const char *f, size_t flen)
{
    bool negative = false;

    if (flen > 0 && f[0] == '-') {
        negative = true;
        f++;
        flen--;
    }
    {
        uint32_t magnitude = parse_x10_field(f, flen);

        return negative ? -(int32_t)magnitude : (int32_t)magnitude;
    }
}

/* "ddmm.mmmm" (degree_digits=2, latitude) or "dddmm.mmmm" (degree_digits=3, longitude) ->
   *out_e7 = round(decimal_degrees * 1e7), always non-negative (sign applied by the caller
   from the N/S or E/W hemisphere field). Returns false for anything not matching this exact
   fixed-width shape. */
static bool parse_ddmm(const char *f, size_t flen, size_t degree_digits, int32_t *out_e7)
{
    size_t i;
    size_t dot = flen;
    size_t frac_digits;
    int64_t deg = 0;
    int64_t min_int;
    int64_t min_frac_e7 = 0;

    if (flen < degree_digits + 2u) {
        return false;
    }
    for (i = 0; i < flen; i++) {
        if (f[i] == '.') {
            dot = i;
            break;
        }
    }
    if (dot != degree_digits + 2u) {
        return false;
    }
    for (i = 0; i < dot; i++) {
        if (!is_digit(f[i])) {
            return false;
        }
    }
    for (i = 0; i < degree_digits; i++) {
        deg = deg * 10 + (f[i] - '0');
    }
    min_int = (f[degree_digits] - '0') * 10 + (f[degree_digits + 1] - '0');

    frac_digits = flen - dot - 1u;
    if (frac_digits > 7u) {
        frac_digits = 7u;
    }
    for (i = 0; i < frac_digits; i++) {
        char c = f[dot + 1 + i];

        if (!is_digit(c)) {
            return false;
        }
        min_frac_e7 = min_frac_e7 * 10 + (c - '0');
    }
    for (i = frac_digits; i < 7u; i++) {
        min_frac_e7 *= 10;
    }

    {
        int64_t minutes_e7 = min_int * 10000000LL + min_frac_e7;
        int64_t degrees_e7 = deg * 10000000LL;

        *out_e7 = (int32_t)(degrees_e7 + (minutes_e7 + 30) / 60);
    }
    return true;
}

bool nmea_parse_gga(const char *line, size_t line_len, nmea_gga_t *out)
{
    const char *f;
    size_t flen;
    const char *hemi;
    size_t hemi_len;

    if (out == NULL || !sentence_is(line, line_len, "GGA") || !nmea_checksum_valid(line, line_len)) {
        return false;
    }
    out->fix_quality = 0;
    out->satellites = 0;
    out->hdop_e1 = 0;
    out->lat_e7 = 0;
    out->lon_e7 = 0;
    out->altitude_dm = 0;

    if (!nmea_field(line, line_len, 5, &f, &flen)) return false; /* fix quality */
    out->fix_quality = parse_uint_field(f, flen);

    if (!nmea_field(line, line_len, 6, &f, &flen)) return false; /* satellites in use */
    out->satellites = parse_uint_field(f, flen);

    if (!nmea_field(line, line_len, 7, &f, &flen)) return false; /* HDOP */
    out->hdop_e1 = parse_x10_field(f, flen);

    if (!nmea_field(line, line_len, 8, &f, &flen)) return false; /* altitude MSL, empty when no fix */
    out->altitude_dm = parse_signed_x10_field(f, flen);

    if (!nmea_field(line, line_len, 1, &f, &flen)) return false; /* latitude */
    if (!nmea_field(line, line_len, 2, &hemi, &hemi_len)) return false; /* N/S */
    if (flen > 0) {
        if (!parse_ddmm(f, flen, 2, &out->lat_e7)) return false;
        if (hemi_len == 1 && hemi[0] == 'S') out->lat_e7 = -out->lat_e7;
    }

    if (!nmea_field(line, line_len, 3, &f, &flen)) return false; /* longitude */
    if (!nmea_field(line, line_len, 4, &hemi, &hemi_len)) return false; /* E/W */
    if (flen > 0) {
        if (!parse_ddmm(f, flen, 3, &out->lon_e7)) return false;
        if (hemi_len == 1 && hemi[0] == 'W') out->lon_e7 = -out->lon_e7;
    }

    return true;
}

bool nmea_parse_rmc(const char *line, size_t line_len, nmea_rmc_t *out)
{
    const char *f;
    size_t flen;

    if (out == NULL || !sentence_is(line, line_len, "RMC") || !nmea_checksum_valid(line, line_len)) {
        return false;
    }

    if (!nmea_field(line, line_len, 0, &f, &flen) || flen < 6) return false; /* hhmmss[.sss] */
    if (!is_digit(f[0]) || !is_digit(f[1]) || !is_digit(f[2]) ||
        !is_digit(f[3]) || !is_digit(f[4]) || !is_digit(f[5])) {
        return false;
    }
    out->hour = (uint8_t)((f[0] - '0') * 10 + (f[1] - '0'));
    out->minute = (uint8_t)((f[2] - '0') * 10 + (f[3] - '0'));
    out->second = (uint8_t)((f[4] - '0') * 10 + (f[5] - '0'));

    if (!nmea_field(line, line_len, 1, &f, &flen) || flen != 1 ||
        (f[0] != 'A' && f[0] != 'V')) {
        return false;
    }
    out->status_active = (f[0] == 'A');

    if (!nmea_field(line, line_len, 8, &f, &flen) || flen < 6) return false; /* ddmmyy */
    if (!is_digit(f[0]) || !is_digit(f[1]) || !is_digit(f[2]) ||
        !is_digit(f[3]) || !is_digit(f[4]) || !is_digit(f[5])) {
        return false;
    }
    out->day = (uint8_t)((f[0] - '0') * 10 + (f[1] - '0'));
    out->month = (uint8_t)((f[2] - '0') * 10 + (f[3] - '0'));
    out->year_2digit = (uint8_t)((f[4] - '0') * 10 + (f[5] - '0'));

    out->speed_knots_e1 = 0;
    if (nmea_field(line, line_len, 6, &f, &flen)) { /* speed over ground, knots */
        out->speed_knots_e1 = parse_x10_field(f, flen);
    }

    return true;
}

/* Howard Hinnant's days-from-civil algorithm (well-known, integer-only, proleptic Gregorian;
   see http://howardhinnant.github.io/date_algorithms.html#days_from_civil) -- avoids floats
   per this codebase's convention (see cbor_wardriving.h and friends: no field in this
   protocol is ever floating-point). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    int64_t era;
    int64_t yoe;
    int64_t doy;
    int64_t doe;

    y -= (m <= 2) ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint64_t nmea_rmc_to_unix_time(const nmea_rmc_t *rmc)
{
    int64_t year = 2000 + rmc->year_2digit;
    int64_t days = days_from_civil(year, rmc->month, rmc->day);
    int64_t seconds = days * 86400 + rmc->hour * 3600 + rmc->minute * 60 + rmc->second;

    return (uint64_t)seconds;
}

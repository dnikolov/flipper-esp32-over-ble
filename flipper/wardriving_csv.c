/* See wardriving_csv.h for the format/scope note. */
#include "wardriving_csv.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

uint32_t feb_wardriving_backdate_first_seen(
    uint64_t record_timestamp_ms, uint64_t anchor_timestamp_ms, uint32_t anchor_unix_time) {
    if(record_timestamp_ms >= anchor_timestamp_ms) {
        return anchor_unix_time;
    }
    uint64_t delta_ms = anchor_timestamp_ms - record_timestamp_ms;
    uint64_t delta_s = delta_ms / 1000u;
    if(delta_s > (uint64_t)anchor_unix_time) {
        return 0;
    }
    return anchor_unix_time - (uint32_t)delta_s;
}

size_t feb_wardriving_csv_format_header(char *out, size_t out_cap) {
    int written = snprintf(
        out,
        out_cap,
        "WigleWifi-1.4,appRelease=1.0.0,model=ESP32-C6,release=1.0.0,"
        "device=flipper-esp32-over-ble,display=none,board=f7,brand=flipper\n"
        "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,"
        "AltitudeMeters,AccuracyMeters,Type\n");
    if(written < 0 || (size_t)written >= out_cap) {
        return 0;
    }
    return (size_t)written;
}

/* Sanitizes `src` to printable ASCII (non-printable bytes -> '.', matching this app's
   existing on-screen SSID/name sanitization) into `dst`, then CSV-quotes the result (wraps in
   double quotes, doubling any embedded quote) if it contains a comma, quote, or newline --
   the sanitization pass above can never itself introduce a comma/quote (both are already
   printable ASCII and pass through unchanged), so quoting is still needed for legitimately
   comma-or-quote-bearing SSIDs/BLE names. Returns bytes written (excluding NUL), or 0 if
   dst_cap is too small for even an empty field.

   `sanitized` is file-scope static, not a local: this function (via
   feb_wardriving_csv_format_row() below) is reachable from the Flipper's 1280-byte
   BleEventWorker thread (handle_wardriving_status() -> wardriving_csv_write_record() ->
   feb_wardriving_csv_format_row() -> csv_write_field(), see docs/LESSONS.md's recurring
   stack-overflow bug class), and this module is only ever called from that one thread in
   this app (never concurrently), so static storage is safe here -- same established
   convention as flipper_esp32_over_ble.c's own BLE-callback-path buffers. No initializer
   here relies on run-once-at-load semantics: every call fully overwrites and NUL-terminates
   the bytes it uses before returning. */
static char sanitized[FEB_CBOR_MAX_TEXT_LEN + 1];

static size_t
    csv_write_field(char *dst, size_t dst_cap, const uint8_t *src, size_t src_len) {
    if(dst_cap == 0) {
        return 0;
    }
    /* Bounded scratch: FEB_CBOR_MAX_TEXT_LEN (64) is the largest possible src_len for any
       field this module formats (ssid/name), sanitized in place before the quoting pass. */
    size_t n = src_len > FEB_CBOR_MAX_TEXT_LEN ? FEB_CBOR_MAX_TEXT_LEN : src_len;
    for(size_t i = 0; i < n; i++) {
        uint8_t b = src[i];
        sanitized[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
    }
    sanitized[n] = '\0';

    int needs_quote = 0;
    for(size_t i = 0; i < n; i++) {
        if(sanitized[i] == ',' || sanitized[i] == '"' || sanitized[i] == '\n' ||
           sanitized[i] == '\r') {
            needs_quote = 1;
            break;
        }
    }

    if(!needs_quote) {
        if(n >= dst_cap) {
            return 0;
        }
        memcpy(dst, sanitized, n);
        return n;
    }

    size_t pos = 0;
    if(pos >= dst_cap) return 0;
    dst[pos++] = '"';
    for(size_t i = 0; i < n; i++) {
        if(sanitized[i] == '"') {
            if(pos + 2 > dst_cap) return 0;
            dst[pos++] = '"';
            dst[pos++] = '"';
        } else {
            if(pos + 1 > dst_cap) return 0;
            dst[pos++] = sanitized[i];
        }
    }
    if(pos + 1 > dst_cap) return 0;
    dst[pos++] = '"';
    return pos;
}

/* mac/ssid_field/auth_field/channel_field are file-scope static, not locals, for the same
   BleEventWorker-stack-budget reason as csv_write_field()'s `sanitized` above (this function
   is the direct caller in that same reachable chain) -- ssid_field alone (131 bytes) already
   exceeds this project's own "≥100 bytes must be static" rule, and the whole chain's frames
   are cumulative (docs/LESSONS.md: "nesting is what kills, not any single frame"). Every
   field is fully written and NUL-terminated on every call before being read (both the wifi
   and ble branches below cover every field), so there is no stale-data-from-a-previous-call
   risk despite the static storage. */
static char mac[18];
static char ssid_field[FEB_CBOR_MAX_TEXT_LEN * 2 + 2 + 1]; /* worst case: every byte doubled + 2 quotes */
static char auth_field[32];
static char channel_field[16];

/* Worst-case row length (see FEB_WARDRIVING_CSV_ROW_MAX_LEN's own comment in
   wardriving_csv.h): mac(17) + ssid_field(130, a 64-byte all-quote-character SSID escaped)
   + auth_field(31) + first_seen(19) + channel(3) + rssi(4, "-128") + lat(11) + lon(12) +
   the literal "0,0"(3) + type(4) + 10 field-separating commas + 1 newline == 245 bytes. */
size_t feb_wardriving_csv_format_row(
    char *out,
    size_t out_cap,
    const feb_wardriving_record_t *record,
    const char *first_seen,
    size_t first_seen_len) {
    if(out_cap == 0) {
        return 0;
    }

    /* Literals cast explicitly to (double) rather than relying on an unsuffixed floating
       constant's default type: some embedded ARM GCC configurations build with
       -fsingle-precision-constant, which makes an unsuffixed decimal/exponent literal like
       900000000.0 or 1e7 a `float`, tripping -Werror=double-promotion the moment it's used
       in an expression already promoted to double (found by the real FBT build; MSVC's host
       test build has no such flag and never caught this). */
    double lat = ((double)(int64_t)record->lat_e7_offset - (double)900000000) / (double)10000000;
    double lon = ((double)(int64_t)record->lon_e7_offset - (double)1800000000) / (double)10000000;
    int32_t rssi_dbm;
    const char *type_str;
    long channel = -1; /* -1 => blank Channel field */

    if(record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
        const feb_wardriving_wifi_payload_t *wifi = &record->payload.wifi;
        snprintf(
            mac,
            sizeof(mac),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            wifi->bssid[0],
            wifi->bssid[1],
            wifi->bssid[2],
            wifi->bssid[3],
            wifi->bssid[4],
            wifi->bssid[5]);
        size_t ssid_written =
            csv_write_field(ssid_field, sizeof(ssid_field), wifi->ssid, wifi->ssid_len);
        ssid_field[ssid_written] = '\0';
        size_t auth_n = wifi->auth_len > sizeof(auth_field) - 1 ? sizeof(auth_field) - 1 : wifi->auth_len;
        for(size_t i = 0; i < auth_n; i++) {
            char c = wifi->auth[i];
            auth_field[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        auth_field[auth_n] = '\0';
        rssi_dbm = (int32_t)wifi->rssi_offset - 128;
        channel = (long)wifi->channel;
        type_str = "WIFI";
    } else if(record->payload_kind == FEB_WARDRIVING_PAYLOAD_BLE) {
        const feb_wardriving_ble_payload_t *ble = &record->payload.ble;
        snprintf(
            mac,
            sizeof(mac),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            ble->address[0],
            ble->address[1],
            ble->address[2],
            ble->address[3],
            ble->address[4],
            ble->address[5]);
        if(ble->has_name) {
            size_t ssid_written = csv_write_field(
                ssid_field, sizeof(ssid_field), (const uint8_t *)ble->name, ble->name_len);
            ssid_field[ssid_written] = '\0';
        } else {
            ssid_field[0] = '\0';
        }
        auth_field[0] = '\0';
        rssi_dbm = (int32_t)ble->rssi_offset - 128;
        type_str = "BLE";
    } else {
        return 0;
    }

    /* channel_field is sized against `long`'s worst-case textual width (up to 11 digits +
       sign + NUL) rather than this project's real 2.4GHz-only channel range (1-14), to
       satisfy -Werror=format-truncation's static (type-range-based, not value-based)
       analysis; declared file-scope static above alongside mac/ssid_field/auth_field. */
    if(channel >= 0) {
        snprintf(channel_field, sizeof(channel_field), "%ld", channel);
    } else {
        channel_field[0] = '\0';
    }

    int written = snprintf(
        out,
        out_cap,
        "%s,%s,%s,%.*s,%s,%ld,%.7f,%.7f,0,0,%s\n",
        mac,
        ssid_field,
        auth_field,
        (int)first_seen_len,
        first_seen,
        channel_field,
        (long)rssi_dbm,
        lat,
        lon,
        type_str);
    if(written < 0 || (size_t)written >= out_cap) {
        return 0;
    }
    return (size_t)written;
}

static void wardriving_dedup_record_address(const feb_wardriving_record_t *record, uint8_t out[6]) {
    if (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
        memcpy(out, record->payload.wifi.bssid, 6);
    } else {
        memcpy(out, record->payload.ble.address, 6);
    }
}

static int32_t wardriving_dedup_record_rssi_dbm(const feb_wardriving_record_t *record) {
    uint64_t rssi_offset = (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI) ?
        record->payload.wifi.rssi_offset : record->payload.ble.rssi_offset;
    return (int32_t)rssi_offset - 128;
}

/* Flat-earth approximation -- adequate at the ~30m scale FEB_WARDRIVING_DEDUP_MOVE_METERS
   operates at, not worth a full haversine for a distinction this coarse. lat/lon decoding
   mirrors feb_wardriving_csv_format_row()'s own arithmetic above (same e7-offset encoding,
   same explicit-double-literal style to avoid this project's known
   -Werror=double-promotion/-fsingle-precision-constant trap on the real FBT build). */
static double wardriving_dedup_distance_meters(
    uint64_t lat_e7_a, uint64_t lon_e7_a, uint64_t lat_e7_b, uint64_t lon_e7_b) {
    double lat_a = ((double)(int64_t)lat_e7_a - (double)900000000) / (double)10000000;
    double lon_a = ((double)(int64_t)lon_e7_a - (double)1800000000) / (double)10000000;
    double lat_b = ((double)(int64_t)lat_e7_b - (double)900000000) / (double)10000000;
    double lon_b = ((double)(int64_t)lon_e7_b - (double)1800000000) / (double)10000000;
    double meters_per_degree = (double)111320;
    double dlat_m = (lat_b - lat_a) * meters_per_degree;
    double dlon_m = (lon_b - lon_a) * meters_per_degree *
                    cos(lat_a * (double)3.14159265358979323846 / (double)180);
    return sqrt(dlat_m * dlat_m + dlon_m * dlon_m);
}

void feb_wardriving_dedup_reset(feb_wardriving_dedup_table_t *table) {
    memset(table, 0, sizeof(*table));
}

bool feb_wardriving_dedup_should_write(
    feb_wardriving_dedup_table_t *table, const feb_wardriving_record_t *record) {
    uint8_t address[6];
    wardriving_dedup_record_address(record, address);
    int32_t rssi_dbm = wardriving_dedup_record_rssi_dbm(record);

    for (size_t i = 0; i < FEB_WARDRIVING_DEDUP_CAPACITY; i++) {
        feb_wardriving_dedup_entry_t *entry = &table->entries[i];

        if (entry->occupied && entry->payload_kind == record->payload_kind &&
            memcmp(entry->address, address, sizeof(address)) == 0) {
            bool stronger = (rssi_dbm - entry->last_rssi_dbm) >= FEB_WARDRIVING_DEDUP_RSSI_IMPROVE_DB;
            bool moved = wardriving_dedup_distance_meters(
                             entry->last_lat_e7_offset,
                             entry->last_lon_e7_offset,
                             record->lat_e7_offset,
                             record->lon_e7_offset) >= FEB_WARDRIVING_DEDUP_MOVE_METERS;

            if (!stronger && !moved) {
                return false;
            }
            entry->last_rssi_dbm = rssi_dbm;
            entry->last_lat_e7_offset = record->lat_e7_offset;
            entry->last_lon_e7_offset = record->lon_e7_offset;
            return true;
        }
    }

    {
        feb_wardriving_dedup_entry_t *slot = &table->entries[table->next_evict_index];

        memcpy(slot->address, address, sizeof(address));
        slot->payload_kind = record->payload_kind;
        slot->occupied = true;
        slot->last_rssi_dbm = rssi_dbm;
        slot->last_lat_e7_offset = record->lat_e7_offset;
        slot->last_lon_e7_offset = record->lon_e7_offset;
        table->next_evict_index = (table->next_evict_index + 1) % FEB_WARDRIVING_DEDUP_CAPACITY;
    }
    return true;
}

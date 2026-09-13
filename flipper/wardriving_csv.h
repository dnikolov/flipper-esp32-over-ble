/* WiGLE CSV export formatting for the `wardriving` capability (docs/CAPABILITIES.md's
   wardriving bullet: "the Flipper converts to WiGLE CSV only when writing to its SD card").

   Flipper-only, not a shared cross-firmware wire-format header (not in
   tools/check_shared_headers.py's HEADER_PAIRS -- there is no ESP32-side equivalent; the
   ESP32 only ever sends the CBOR <wardriving-record> shape, never a CSV row). Deliberately
   has no Furi dependency (stdint/stddef/cbor_codec.h only) so it builds and is host-testable
   in tests/flipper's MSVC harness exactly like the cbor_* codec modules (see
   tests/flipper/build.ps1) -- the file-I/O half (open/append/sync/close, needs Storage*) is
   Furi-dependent and lives in flipper_esp32_over_ble.c instead, calling into this module only
   for the pure byte-formatting.

   Format is WigleWifi-1.4 (MAC, SSID, AuthMode, FirstSeen, Channel, Frequency, RSSI,
   CurrentLatitude, CurrentLongitude, AltitudeMeters, AccuracyMeters, Type) -- WigleWifi-1.6
   additionally adds RCOIs/MfgrId, which this project genuinely has no data source for, so
   there is no benefit to the newer version tag for those two. Frequency is different: it's a
   deterministic function of a Wi-Fi Channel (2.4GHz only, matching this board's single radio),
   not a field needing its own data source, so it's included here despite the older version
   tag -- a WiGLE parser reads the column-header row itself, not just the version tag, to know
   what's present. AltitudeMeters/AccuracyMeters are always written as 0 (no altitude/accuracy
   data exists behind the current GPS stub, and WiGLE's own consumers tolerate a flat 0 in
   these columns). AuthMode for a `source="ble"` record is left blank (WiGLE's AuthMode column
   has no BLE meaning); Channel and Frequency are likewise blank for a BLE record -- BLE hops
   across channels rather than occupying one fixed frequency, so there is no single correct
   value to report, unlike Wi-Fi's fixed per-AP channel. */
#ifndef FEB_WARDRIVING_CSV_H
#define FEB_WARDRIVING_CSV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "cbor_codec.h"

/* Sized against the real worst case computed in wardriving_csv.c's own comment above
   feb_wardriving_csv_format_row() -- both header lines together are ~237 bytes; a
   maximally-escaped row (a 64-byte all-quote-character SSID, the longest defensively-clamped
   auth string, and the widest lat/lon/rssi text) is ~244 bytes. Rounded up with margin. */
#define FEB_WARDRIVING_CSV_HEADER_MAX_LEN 256u
#define FEB_WARDRIVING_CSV_ROW_MAX_LEN 256u
/* Real content is always "YYYY-MM-DD HH:MM:SS" == 19 bytes + NUL, but the caller-owned
   buffer this sizes is built with snprintf("%04u-%02u-%02u %02u:%02u:%02u", ...) from a
   Furi DateTime's fields (year is uint16_t, month/day/hour/minute/second are uint8_t) --
   GCC's -Werror=format-truncation reasons from each field's *type* range, not its real
   calendar-valid range, so it sees a worst case of a 5-digit year and 3-digit
   month/day/hour/minute/second (5+1+3+1+3+1+3+1+3+1+3 == 25 bytes) and demands a buffer
   that large or it errors at build time. Sized for that worst case, not the real one, so
   the FBT build (which treats this warning as an error; MSVC's host build does not) passes
   cleanly -- caller-owned buffer, shared constant so both flipper_esp32_over_ble.c and this
   module's host test agree on one size. */
#define FEB_WARDRIVING_CSV_FIRST_SEEN_LEN 32u

/* The two WigleWifi-1.4 header lines (metadata line, then column-name line), each
   newline-terminated and concatenated into one buffer -- written once, when a new export
   file is created. Returns bytes written (excluding any NUL), or 0 if out_cap is too small. */
size_t feb_wardriving_csv_format_header(char *out, size_t out_cap);

/* One CSV data row (newline-terminated) for a single decoded <wardriving-record>.
   `first_seen` is a caller-formatted "YYYY-MM-DD HH:MM:SS" string, built by the caller
   directly from the record's own `utc_timestamp_s` field (docs/PROTOCOL.md; always present
   and valid -- see cbor_wardriving.h) via datetime_timestamp_to_datetime(), a Furi/lib API,
   which is why that conversion stays in flipper_esp32_over_ble.c rather than here, keeping
   this module Furi-free. Sanitizes SSID/BLE-name to printable ASCII and CSV-quotes any field
   containing a comma, quote, or newline, same convention as this app's on-screen display
   sanitization. Returns
   bytes written (excluding NUL), or 0 on failure (out_cap too small, or an unrecognized
   `record->payload_kind`). */
size_t feb_wardriving_csv_format_row(
    char *out,
    size_t out_cap,
    const feb_wardriving_record_t *record,
    const char *first_seen,
    size_t first_seen_len);

/* Per-BSSID/address CSV-row deduplication (docs/PROJECT_HISTORY.md's 2026-09-10 "wardriving
   duplicate records" discussion) -- the flash log and wire protocol stay exactly as-is (an
   honest, complete observation-by-observation record); this only collapses redundant rows at
   the one point WiGLE CSV is actually the consumer, matching this header's own top comment
   ("the Flipper converts to WiGLE CSV only when writing to its SD card").

   Policy (independently designed after surveying three permissively-relevant-or-referenced
   prior projects, no code reused from any of them): write a row if the address is new this
   export session, OR its RSSI improved by >= FEB_WARDRIVING_DEDUP_RSSI_IMPROVE_DB since its
   last written row, OR it has moved >= FEB_WARDRIVING_DEDUP_MOVE_METERS since then (bettercap's
   MIT-licensed wifi_recon.go: keyed-map update-in-place instead of appending; the GPL-3.0
   wardriver_rev3's bounded MAC-history ring buffer: fixed-capacity table, no heap, evict-oldest
   when full; an unlicensed Hak5-payload project's "moved/stronger/refresh" OR-gate: the
   multi-criteria policy shape, adapted here). Deliberately NOT included: a pure time-elapsed
   trigger (e.g. "write again every 300s regardless") -- with the current fixed-coordinate GPS
   stub, that would write rows identical in every field except FirstSeen, which is exactly the
   kind of duplicate this exists to remove. The move clause is real code, not a stub, but is
   inert until real GPS lands: distance from a fixed coordinate to itself is always 0.

   Table lifetime matches the CSV export file's own lifetime, not any narrower per-restart
   session segment (docs/CAPABILITIES.md; former docs/BACKLOG.md G29) -- reset only from
   wardriving_csv_close() in flipper_esp32_over_ble.c (disconnect/profile-teardown/app-exit),
   deliberately never from a same-file "started" ack, so a manual stop/restart mid-capture
   does not make every address still in range look brand-new again. Same scope as the
   FirstSeen-anchor state above. 256 entries chosen deliberately more conservative
   than wardriver_rev3's 512 -- a Flipper app shares far less free RAM with the rest of the
   firmware than that project's dedicated board. A table full of distinct addresses evicts its
   oldest entry (simple ring cursor, not LRU, matching wardriver_rev3's own simplification) --
   not a correctness problem, just a soft floor on how well an unusually address-dense session
   gets deduplicated.

   Split into two independent sub-tables (Wi-Fi / BLE), each with its own eviction cursor,
   rather than one shared 256-slot ring keyed by payload_kind (docs/HARDENING_BACKLOG.md H04
   follow-up) -- BLE churns far faster than Wi-Fi (default scan interval 500ms vs Wi-Fi's
   5000ms, plus BLE's common use of rotating private addresses), so a shared ring let BLE
   insertions evict still-relevant Wi-Fi entries purely for sharing a ring, causing avoidable
   duplicate Wi-Fi CSV rows. The ESP32's own independent dedup layer
   (esp32/main/wardriving_dedup.c) already treats these as two separate tables (256 Wi-Fi /
   512 BLE) for exactly this reason. Capacities kept at the same 1:2 ratio but scaled down
   (144 total vs the ESP32's 768) since this table only needs to catch duplicates within one
   calendar-day CSV file's dedup window, not gate the whole capture pipeline the way the
   ESP32's own table does. */
#define FEB_WARDRIVING_DEDUP_WIFI_CAPACITY 48u
#define FEB_WARDRIVING_DEDUP_BLE_CAPACITY 96u
#define FEB_WARDRIVING_DEDUP_RSSI_IMPROVE_DB 6
#define FEB_WARDRIVING_DEDUP_MOVE_METERS ((double)30.0)

/* No payload_kind field: each sub-table below only ever holds one kind, so the field
   dropped here is redundant, not a size optimization -- it previously sat in what would
   otherwise be alignment padding before the uint64_t fields, so sizeof(entry) is unchanged. */
typedef struct {
    uint8_t address[6];
    bool occupied;
    int32_t last_rssi_dbm;
    uint64_t last_lat_e7_offset;
    uint64_t last_lon_e7_offset;
} feb_wardriving_dedup_entry_t;

typedef struct {
    feb_wardriving_dedup_entry_t entries[FEB_WARDRIVING_DEDUP_WIFI_CAPACITY];
    uint32_t next_evict_index;
} feb_wardriving_dedup_wifi_table_t;

typedef struct {
    feb_wardriving_dedup_entry_t entries[FEB_WARDRIVING_DEDUP_BLE_CAPACITY];
    uint32_t next_evict_index;
} feb_wardriving_dedup_ble_table_t;

typedef struct {
    feb_wardriving_dedup_wifi_table_t wifi;
    feb_wardriving_dedup_ble_table_t ble;
} feb_wardriving_dedup_table_t;

/* Zeroes the table (all entries unoccupied, cursor at 0). Call once per new CSV export
   file (its lifetime, not any narrower restart segment within it), alongside
   wardriving_csv_reset_state() -- see that function's own comment. */
void feb_wardriving_dedup_reset(feb_wardriving_dedup_table_t *table);

/* Looks up `record`'s address in `table`, applies the policy above, and updates the matching
   (or newly inserted) entry when the row should be written. Returns true if the caller should
   write this row, false if it's a redundant repeat to skip -- a false return is a normal,
   expected outcome, not an error. */
bool feb_wardriving_dedup_should_write(
    feb_wardriving_dedup_table_t *table, const feb_wardriving_record_t *record);

#endif /* FEB_WARDRIVING_CSV_H */

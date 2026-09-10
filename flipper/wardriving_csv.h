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

   Format is WigleWifi-1.4 (MAC, SSID, AuthMode, FirstSeen, Channel, RSSI, CurrentLatitude,
   CurrentLongitude, AltitudeMeters, AccuracyMeters, Type) -- the later WigleWifi-1.6 adds a
   Frequency column plus RCOIs/MfgrId that this project has no data source for, so there is
   no benefit to the newer version here. AltitudeMeters/AccuracyMeters are always written as
   0 (no altitude/accuracy data exists behind the current GPS stub, and WiGLE's own consumers
   tolerate a flat 0 in these columns). AuthMode for a `source="ble"` record is left blank
   (WiGLE's AuthMode column has no BLE meaning); Channel is likewise blank for a BLE record. */
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
   `first_seen` is a caller-formatted "YYYY-MM-DD HH:MM:SS" string (see
   feb_wardriving_backdate_first_seen below for computing the underlying UNIX time; the
   UNIX-time -> calendar-string conversion itself needs datetime_timestamp_to_datetime(),
   a Furi/lib API, so it stays in flipper_esp32_over_ble.c, keeping this module Furi-free).
   Sanitizes SSID/BLE-name to printable ASCII and CSV-quotes any field containing a comma,
   quote, or newline, same convention as this app's on-screen display sanitization. Returns
   bytes written (excluding NUL), or 0 on failure (out_cap too small, or an unrecognized
   `record->payload_kind`). */
size_t feb_wardriving_csv_format_row(
    char *out,
    size_t out_cap,
    const feb_wardriving_record_t *record,
    const char *first_seen,
    size_t first_seen_len);

/* Reconstructs an approximate wall-clock UNIX FirstSeen for a record whose only real
   timestamp is boot-relative `timestamp_ms` (this board has no RTC) -- docs/CAPABILITIES.md:
   "anchoring the newest drained record to the Flipper's current clock and backdating the
   rest". `anchor_timestamp_ms` is the largest `timestamp_ms` seen so far in this export
   session and `anchor_unix_time` is the Flipper's wall-clock UNIX time at the moment that
   anchor record was processed; the caller updates the anchor to a new maximum (using
   record_timestamp_ms as anchor_timestamp_ms and the current wall clock as anchor_unix_time)
   before calling this for the anchor record itself, so record_timestamp_ms <=
   anchor_timestamp_ms always holds here. Saturates at 0 (UNIX epoch) instead of underflowing
   if the computed backdated time would be negative -- an approximation edge case (documented
   as such in docs/CAPABILITIES.md), not an error to propagate. */
uint32_t feb_wardriving_backdate_first_seen(
    uint64_t record_timestamp_ms, uint64_t anchor_timestamp_ms, uint32_t anchor_unix_time);

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

   Table lifetime matches the CSV export session (reset alongside wardriving_csv_reset_state()'s
   existing call sites in flipper_esp32_over_ble.c: session close, and a "started" ack), same
   scope as the FirstSeen-anchor state above. 256 entries chosen deliberately more conservative
   than wardriver_rev3's 512 -- a Flipper app shares far less free RAM with the rest of the
   firmware than that project's dedicated board. A table full of distinct addresses evicts its
   oldest entry (simple ring cursor, not LRU, matching wardriver_rev3's own simplification) --
   not a correctness problem, just a soft floor on how well an unusually address-dense session
   gets deduplicated. */
#define FEB_WARDRIVING_DEDUP_CAPACITY 256u
#define FEB_WARDRIVING_DEDUP_RSSI_IMPROVE_DB 6
#define FEB_WARDRIVING_DEDUP_MOVE_METERS 30.0

typedef struct {
    uint8_t address[6];
    feb_wardriving_payload_kind_t payload_kind;
    bool occupied;
    int32_t last_rssi_dbm;
    uint64_t last_lat_e7_offset;
    uint64_t last_lon_e7_offset;
} feb_wardriving_dedup_entry_t;

typedef struct {
    feb_wardriving_dedup_entry_t entries[FEB_WARDRIVING_DEDUP_CAPACITY];
    uint32_t next_evict_index;
} feb_wardriving_dedup_table_t;

/* Zeroes the table (all entries unoccupied, cursor at 0). Call once per new CSV export
   session, alongside wardriving_csv_reset_state(). */
void feb_wardriving_dedup_reset(feb_wardriving_dedup_table_t *table);

/* Looks up `record`'s address in `table`, applies the policy above, and updates the matching
   (or newly inserted) entry when the row should be written. Returns true if the caller should
   write this row, false if it's a redundant repeat to skip -- a false return is a normal,
   expected outcome, not an error. */
bool feb_wardriving_dedup_should_write(
    feb_wardriving_dedup_table_t *table, const feb_wardriving_record_t *record);

#endif /* FEB_WARDRIVING_CSV_H */

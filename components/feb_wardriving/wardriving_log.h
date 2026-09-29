/* Raw-flash-backed circular log for the `wardriving` capability (docs/PLAN.md step 8's
   wardriving-log persistence, pulled forward per the 2026-09-07 reorder; see
   docs/PROTOCOL.md's "Flash log eviction" note for the wire-visible behavioral contract
   this implements). Lives on its own dedicated partition (esp32/partitions.csv, "wardrive")
   using esp_partition directly -- not NVS, not a wear-levelling filesystem (see that note
   and docs/PLAN.md step 8 for why). The on-flash byte layout and its crash-safety
   properties are documented in wardriving_record_format.h, which this module's boot scan
   and append/peek/drain logic are built on.

   Call wardriving_log_init() once at boot (after nvs_flash_init(), matching this project's
   existing "storage subsystems come up early in app_main()" convention -- no actual
   dependency on NVS itself). Every other function here is only safe to call after that.

   Not part of the shared ESP32/Flipper protocol contract -- this is ESP32-local storage.
   What crosses the wire is exactly one feb_wardriving_record_t per captured observation
   (cbor_wardriving.h), which this module encodes/decodes internally to store/retrieve. */
#ifndef FEB_WARDRIVING_LOG_H
#define FEB_WARDRIVING_LOG_H

#include <stdbool.h>
#include <stddef.h>

#include "cbor_wardriving.h"

void wardriving_log_init(void);

/* Encodes `record` and appends it to the log. Returns false only for a real failure (no
   "wardrive" partition found at init, or feb_cbor_encode_wardriving_record() itself failing
   -- should not happen for a record built from this firmware's own wifi/ble scan results,
   which stay well within WD_RECORD_MAX_PAYLOAD). Never fails merely because the log is
   full -- a full log evicts the oldest sector's worth of records to make room
   (docs/PROTOCOL.md's "Flash log eviction" note), it does not reject the append. */
bool wardriving_log_append(const feb_wardriving_record_t *record);

/* Number of buffered records not yet included in any sent batch. 0 means "caught up to
   live" per docs/PROTOCOL.md's backlog_remaining convention. */
size_t wardriving_log_pending_count(void);

/* Reads up to max_records of the oldest still-undrained records into out[], oldest first.
   out[i]'s pointer members (ssid/auth/name) alias into scratch_buf, which must stay valid
   and untouched by the caller for as long as out[] is used. Does not mark anything drained
   -- follow with wardriving_log_mark_drained() using the number of records actually placed
   into a sent batch (which may be less than this call's return value, if the caller's own
   trial-encode-and-back-off packing couldn't fit all of them into one status record).
   Returns the number of records actually filled into out[] (0 if nothing is pending, or if
   scratch_buf ran out of room for even the first pending record's encoded bytes). Capped at
   FEB_WARDRIVING_MAX_RECORDS_PER_BATCH regardless of max_records. */
size_t wardriving_log_peek_pending(feb_wardriving_record_t *out, size_t max_records,
                                   uint8_t *scratch_buf, size_t scratch_buf_len);

/* Marks the oldest `count` records -- the same ones the most recent
   wardriving_log_peek_pending() call returned, in the same order -- as drained (one
   single-byte partial-program flash write per record; see wardriving_record_format.h's
   header comment for why this is crash-safe without touching anything else in the record).
   `count` must not exceed that call's return value; a stale/second call without an
   intervening peek is a no-op. */
void wardriving_log_mark_drained(size_t count);

#endif /* FEB_WARDRIVING_LOG_H */

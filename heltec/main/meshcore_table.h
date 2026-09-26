/* In-RAM, spinlock-guarded MeshCore node table -- keyed by node_id_hex (upsert semantics: a
   repeated sighting of the same node_id updates its existing entry rather than adding a
   duplicate). Written by meshcore_radio.cpp's dedicated RX task on every successfully parsed
   ADVERT (meshcore_proto.h); read by handle_meshcore_command() (main.c) via
   meshcore_table_snapshot(), the same guarded-shared-state pattern location.h's
   location_spinlock/location_get_fix() use for the GPS driver.

   RAM-only, cleared on reboot -- no flash persistence (docs/PLAN.md's design plan's explicit
   Phase 1 scope cut; see docs/CAPABILITIES.md's `meshcore_scan` entry).

   Sizing: MESHCORE_TABLE_MAX_ENTRIES was originally planned at 64 (deliberately small
   relative to wardriving's 256/512-slot dedup tables, wardriving_dedup.h, since MeshCore
   nodes are sparse/persistent infrastructure rather than wardriving's large transient AP/BLE
   population) but was cut to 12 during this same implementation pass, 2026-09-26, after
   `idf.py build` on the real heltec/ target actually overflowed this board's classic-ESP32
   DRAM budget (`.dram0.bss` over by 4536 bytes) with the original 64-entry, 88-byte-per-entry
   table -- not a soft guess, a measured hard constraint. meshcore_table_entry_t was also
   tightened (fields reordered to eliminate padding, last_seen_ms narrowed to a 32-bit
   boot-relative counter -- see its own comment) to shrink each entry from 88 to exactly 64
   bytes before picking 12 as the final count (768 bytes total, ~328 bytes of margin left in
   this build). This still has NOT been validated against a real MeshCore capture (no
   hardware owned yet, per docs/PLAN.md's design plan) -- unlike the DRAM ceiling, the "is 12
   actually enough for a real mesh" question is still exactly the open sizing question
   wardriving_dedup.h's own precedent describes, just with a much lower ceiling than
   originally planned to work with. Independently, components/feb_protocol/cbor_meshcore.h's
   FEB_MESHCORE_MAX_NODES_PER_RESULT (a single-status-reply wire-payload size bound, smaller
   still) means a single `meshcore_scan` status reply may not be able to list every table
   entry at once even well before this table itself is full -- see
   meshcore_table_snapshot()'s own comment on how that's surfaced. */
#ifndef FEB_MESHCORE_TABLE_H
#define FEB_MESHCORE_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meshcore_proto.h"

#define MESHCORE_TABLE_MAX_ENTRIES 12u

/* Field order deliberately groups the byte/char-array members first (17 + 25 + 1 + 1 = 44,
   already a multiple of 4) and the 4-byte-aligned scalar members last, so this struct packs
   to exactly 64 bytes with zero compiler-inserted padding on a 32-bit-aligned ABI (confirmed
   via this session's actual `idf.py build` map-file inspection, not assumed) -- reordering
   these fields is a real regression risk for meshcore_table.c's sizing math above, not just a
   style choice. */
typedef struct {
    char node_id_hex[MESHCORE_NODE_ID_HEX_LEN + 1u];
    char name[MESHCORE_NAME_MAX_LEN + 1u];
    bool has_name;
    bool has_location;
    meshcore_role_t role;
    int32_t rssi_dbm; /* from the LoRa radio's own per-frame RSSI reading, not anything
                          carried in the MeshCore packet itself */
    uint32_t last_seen_ms; /* milliseconds since ESP32 boot (esp_timer_get_time() / 1000),
                               same boot-relative convention as wardriving's timestamp_ms --
                               see cbor_meshcore.h's comment on why the ADVERT's own
                               (unverified, sender-clock) timestamp is not used instead.
                               Deliberately 32-bit (not uint64_t like the wire codec's
                               last_seen_ms field) to keep this table's per-entry size down
                               -- wraps after ~49.7 days of continuous uptime, at which point
                               a stale entry could misleadingly sort as "just seen"; accepted
                               as a low-probability, low-consequence edge case (this table is
                               RAM-only and already cleared on every reboot) rather than
                               spending another 4 bytes/entry on a scenario this board is
                               unlikely to run continuously through unattended. */
    int32_t lat_e7;
    int32_t lon_e7;
} meshcore_table_entry_t;

/* Upserts one observed ADVERT: refreshes the existing entry if advert->node_id_hex is
   already tracked, otherwise inserts a new one (evicting the single least-recently-seen
   entry if the table is full -- see meshcore_table.c). Called only from
   meshcore_radio.cpp's RX task. Thread-safe (spinlock). last_seen_ms is accepted as uint64_t
   (matching esp_timer_get_time()/1000's natural return width and the wire codec's own field
   width) and truncated to the table's internal uint32_t storage -- see that field's comment. */
void meshcore_table_upsert(const meshcore_advert_t *advert, int32_t rssi_dbm, uint64_t last_seen_ms);

/* Copies up to max_entries of the table's current contents into out[], ordered
   most-recently-seen first (so a caller forced to truncate -- see cbor_meshcore.h's sizing
   note -- keeps the most operationally relevant nodes, not an arbitrary subset). Returns the
   number of entries actually copied (<= max_entries and <= the table's current occupancy).
   If out_total_known is non-NULL, *out_total_known receives the table's current total
   occupied-entry count, which may exceed the return value when max_entries is smaller --
   the caller (handle_meshcore_command(), main.c) surfaces that gap via the wire's
   `total_known_nodes` field rather than silently under-reporting. Thread-safe. */
size_t meshcore_table_snapshot(meshcore_table_entry_t *out, size_t max_entries, uint64_t *out_total_known);

#endif /* FEB_MESHCORE_TABLE_H */

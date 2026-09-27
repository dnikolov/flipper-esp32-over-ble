/* In-RAM, spinlock-guarded Meshtastic node table -- keyed by node_id_hex (upsert semantics:
   a repeated sighting of the same node_id updates its existing entry rather than adding a
   duplicate). Written by lora_shared_radio.cpp's dedicated RX task on every successfully
   parsed Meshtastic packet (meshtastic_proto.h); read by handle_meshtastic_command() (main.c)
   via meshtastic_table_snapshot(). Mirrors meshcore_table.h's shape/pattern exactly (same
   guarded-shared-state convention location.h's location_spinlock/location_get_fix() use).

   RAM-only, cleared on reboot -- no flash persistence (same Phase 1 scope cut as
   meshcore_scan; see docs/CAPABILITIES.md's `meshtastic_scan` entry).

   Sizing: each entry has no role/location fields (unlike meshcore_table_entry_t -- see
   meshtastic_proto.h's scope note on why: Meshtastic's cleartext header carries neither, and
   decoding a POSITION_APP payload for location is out of Phase 1 scope), so it's
   substantially smaller per-entry than meshcore's 64-byte entry (this struct is 28 bytes at
   MESHTASTIC_NAME_MAX_LEN=8). MESHTASTIC_TABLE_MAX_ENTRIES was chosen the same way
   meshcore_table.h's was -- measured against a real `idf.py build` DRAM report on this
   classic-ESP32 target, not guessed -- but the *available* headroom this time was much
   smaller than meshcore_scan alone had, because meshcore_scan's own table/scratch buffers
   were already occupying most of this board's DRAM before meshtastic_scan added a second
   node table, a second per-command entries[]/result scratch, and an AES-CTR decrypt scratch
   buffer (meshtastic_proto.c) alongside it. Reaching a clean `idf.py build` required cutting
   MESHTASTIC_NAME_MAX_LEN to 8 (meshtastic_proto.h), switching the decoded name field from
   Meshtastic's long_name to its shorter short_name to match, and capping both this table (3
   entries) and cbor_meshtastic.h's own FEB_MESHTASTIC_MAX_NODES_PER_RESULT (2) well below
   what meshcore_scan uses for the equivalent constants (12 and 3 respectively) -- the final
   build leaves only ~120 bytes of DRAM headroom on this board (docs/SESSION_MEMORY.md's
   meshtastic_scan entry has the exact measured number), a real, tight, hard constraint: any
   future capability work on this board should expect very little DRAM margin left to spend.
   Like meshcore_table.h, this has NOT been validated against a real Meshtastic mesh's node
   density (no hardware owned) -- the "is 3 entries actually enough" question is open the same
   way, and is now more tightly constrained than it would otherwise be by DRAM alone, not by a
   deliberate product judgment that 3 is the right number for real-world use. */
#ifndef FEB_MESHTASTIC_TABLE_H
#define FEB_MESHTASTIC_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meshtastic_proto.h"

#define MESHTASTIC_TABLE_MAX_ENTRIES 3u

typedef struct {
    char node_id_hex[MESHTASTIC_NODE_ID_HEX_LEN + 1u];
    char name[MESHTASTIC_NAME_MAX_LEN + 1u];
    bool has_name;
    int32_t rssi_dbm; /* from the LoRa radio's own per-frame RSSI reading */
    uint32_t last_seen_ms; /* milliseconds since ESP32 boot -- same boot-relative, 32-bit,
                               ~49.7-day-wraps-but-accepted convention as
                               meshcore_table_entry_t.last_seen_ms; see that struct's comment
                               for the full rationale, unchanged here */
} meshtastic_table_entry_t;

/* Upserts one observed packet. Called only from lora_shared_radio.cpp's RX task. Thread-safe
   (spinlock). last_seen_ms accepted as uint64_t (matching esp_timer_get_time()/1000's natural
   return width), truncated to this table's internal uint32_t storage. */
void meshtastic_table_upsert(const meshtastic_advert_t *advert, int32_t rssi_dbm, uint64_t last_seen_ms);

/* Copies up to max_entries of the table's current contents into out[], ordered
   most-recently-seen first. Returns the number of entries actually copied (<= max_entries and
   <= the table's current occupancy). If out_total_known is non-NULL, *out_total_known
   receives the table's current total occupied-entry count. Thread-safe. */
size_t meshtastic_table_snapshot(meshtastic_table_entry_t *out, size_t max_entries, uint64_t *out_total_known);

#endif /* FEB_MESHTASTIC_TABLE_H */

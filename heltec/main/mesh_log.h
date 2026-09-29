/* Raw-flash-backed circular log for the `mesh_log` capability (docs/WARDRIVING_PUBLISH.md
   "Mesh node publishing", design frozen 2026-09-27) -- the missing capture/accumulate/drain
   layer feeding wdgwars.pl's mesh-node (MeshCore/Meshtastic) upload. Mirrors
   wardriving_log.h/.c's architecture (own dedicated partition, esp_partition directly, not
   NVS/a wear-levelling filesystem; append/peek/mark-drained API shape, caller-supplied peek
   scratch buffer) -- see that header's top comment for the shared crash-safety rationale, not
   repeated here.

   Real differences from wardriving_log.c, all driven either by this board's severe classic-
   ESP32 DRAM ceiling (docs/BACKLOG.md BL23 -- only ~120 bytes of `.dram0.bss` headroom left
   before this capability existed -- every static byte here was fought for) or by this module
   having producer/consumer calls on two *different* FreeRTOS tasks (wardriving_log.c only
   ever runs on the NimBLE host task, so it needs no lock of its own):
   - `mesh_log_record_sighting()` is called from `lora_shared_radio.cpp`'s dedicated LoRa RX
     task on every successfully decoded, positioned sighting (mirrors where
     `meshcore_table_upsert()`/`meshtastic_table_upsert()` are already called from). Every
     other function here (`mesh_log_pending_count()`/`mesh_log_peek_pending()`/
     `mesh_log_mark_drained()`) is only ever called from the NimBLE host task (`mesh_caps.c`'s
     `mesh_log_send_next_batch()`, itself only reachable from BLE session-authenticated
     handling). Since both tasks touch the same on-flash bookkeeping state, every public entry
     point here takes a real FreeRTOS mutex (not a spinlock: `esp_partition_write`/
     `esp_partition_erase_range` can take milliseconds, which is unsafe to hold a
     `portENTER_CRITICAL()` spinlock through with interrupts disabled -- a spinlock is fine for
     meshcore_table.c's plain-memory-only critical sections, not for this module's flash I/O).
     `mesh_log_peek_pending()`'s scratch buffer is caller-supplied (exactly like
     wardriving_log_peek_pending()'s own convention) specifically so it can live on the NimBLE
     host task's *stack* (mesh_caps.c's `feb_mesh_log_send_next_batch()`, a plain local, not `static`)
     rather than adding a second permanent `.bss` buffer here -- classic ESP32 is dual-core, so
     that task and the LoRa RX task can genuinely execute simultaneously; a single scratch
     buffer shared between the two paths would be a real, silent data race, but two *disjoint*
     buffers (mesh_log_record_sighting()'s own stack-local one -- see that function's own
     comment on why it too is a plain local, not `static`, despite this codebase's usual
     BLE/radio-callback-path convention -- and mesh_caps.c's own stack-local one, used only inside
     `mesh_log_send_next_batch()`'s own mutex-held call into `mesh_log_peek_pending()`) never
     touch each other, so there is nothing to race over.
   - `FEB_MESH_LOG_MAX_RECORDS_PER_BATCH` (cbor_mesh_log.h) is 1, not a dynamically-packed
     batch like `wardriving`'s -- see that constant's own comment. This keeps the caller-side
     scratch buffer above to a single record's worth (ML_RECORD_MAX_PAYLOAD bytes), not
     `wardriving_log.c`'s own `N * WD_RECORD_MAX_PAYLOAD`.
   - Sector/offset bookkeeping fields use `uint8_t`/`uint16_t` (this partition's own sector
     count and `ML_SECTOR_SIZE` both fit comfortably), not `size_t` -- a real, deliberate
     width-narrowing for the same DRAM reason `meshcore_table_entry_t`/
     `meshtastic_table_entry_t` narrowed their own fields (see those structs' comments).
   - Dedup: a given `node_id` is recorded at most once, ever, gating what
     `mesh_log_record_sighting()` actually appends (docs/WARDRIVING_PUBLISH.md's "revised
     2026-09-27" note), via a heap-allocated table of 128 4-byte FNV-1a hashes (~512 bytes,
     `malloc()`'d once in `mesh_log_init()`) -- collisions are an accepted ~2e-6-at-n=128
     tradeoff since wdgwars.pl only cares seen-vs-not-seen. **Survives a reboot**:
     `mesh_log_init()` seeds this table from the existing flash log once at boot (every
     still-present record, drained or not) -- see mesh_log.c's top comment for details.

   Call mesh_log_init() once at boot (after nvs_flash_init(), matching wardriving_log_init()'s
   own convention -- no actual dependency on NVS). Every other function here is only safe to
   call after that. Not part of the shared ESP32/Flipper protocol contract -- this is
   ESP32-local storage; what crosses the wire is exactly one feb_mesh_log_record_t per drained
   sighting (components/feb_protocol/cbor_mesh_log.h), which this module encodes/decodes
   internally to store/retrieve. */
#ifndef FEB_MESH_LOG_H
#define FEB_MESH_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_mesh_log.h"
#include "mesh_log_record_format.h"

typedef enum {
    MESH_LOG_NETWORK_MESHCORE = 0,
    MESH_LOG_NETWORK_MESHTASTIC = 1,
} mesh_log_network_t;

void mesh_log_init(void);

/* Records one positioned sighting if (and only if) node_id_hex is not already known to this
   log -- an already-known node_id is a silent no-op, not an error (this is the expected
   steady-state case once a mesh has been observed for a while). node_id_hex need not be
   NUL-terminated; exactly node_id_hex_len bytes are used (must be
   1..FEB_MESH_LOG_NODE_ID_MAX_LEN, cbor_mesh_log.h). lat_e7/lon_e7 are this project's usual
   signed, already-*1e7-scaled coordinate convention (meshcore_proto.h's lat_e7/lon_e7 fields
   can be passed straight through); the wire's unsigned +offset encoding is applied internally.
   Returns false only for a real failure (no "meshlog" partition at init, or a genuinely new
   record's encode/append itself failing) -- returns true for both "recorded" and "already
   known, no-op". Safe to call from a different task than every other function below (see this
   header's top comment) -- the only function in this module that is. */
bool mesh_log_record_sighting(const char *node_id_hex, size_t node_id_hex_len,
                              mesh_log_network_t network, int32_t lat_e7, int32_t lon_e7);

/* Number of buffered records not yet included in any sent batch. 0 means "caught up to live",
   same convention as wardriving_log_pending_count(). */
size_t mesh_log_pending_count(void);

/* Same peek/mark-drained batching contract as wardriving_log_peek_pending()/
   wardriving_log_mark_drained() (wardriving_log.h), including the caller-supplied
   scratch_buf/scratch_buf_len convention (out[0]'s node_id/network pointers alias into
   scratch_buf, which must stay valid and untouched until the matching
   mesh_log_mark_drained() call) -- see that header's fuller comment on the
   count-must-not-exceed-last-peek rule, not repeated here. Capped at
   FEB_MESH_LOG_MAX_RECORDS_PER_BATCH (1) regardless of max_records. Only ever called from the
   NimBLE host task -- see this header's top comment on why its scratch buffer should be a
   plain stack local there, not `static`. */
size_t mesh_log_peek_pending(feb_mesh_log_record_t *out, size_t max_records,
                             uint8_t *scratch_buf, size_t scratch_buf_len);
void mesh_log_mark_drained(size_t count);

#endif /* FEB_MESH_LOG_H */

/* See mesh_log.h's top comment for this module's overall shape/rationale.

   Dedup strategy actually used in this build: a **heap-allocated** table
   (ml_dedup_entries below), the frozen design's originally-preferred option, switched to from
   an earlier flash-scan fallback once a real hardware session (2026-09-27) finally measured
   `esp_get_free_heap_size()` at boot: **121808 bytes free** (before Wi-Fi/BLE stack init, so
   an upper bound on steady-state, not the true worst case -- see docs/BACKLOG.md BL24) --
   comfortably enough to size a generous dedup table without touching this board's exhausted
   `.dram0.bss` budget at all (docs/BACKLOG.md BL23). `ml_dedup_entries` is allocated once via
   `malloc()` in `mesh_log_init()`, never `static` -- only the pointer to it and a small
   count/flag (a handful of bytes total, see their own declarations below) live in `.bss`, so
   this change does not materially move BL24's own headroom number. Capacity is
   `ML_DEDUP_CAPACITY` (128) entries of `ml_dedup_entry_t` (17 bytes each: a 1-byte length plus
   up to `FEB_MESH_LOG_NODE_ID_MAX_LEN` (16) raw id bytes, no NUL terminator) -- about 2.2 KB
   total, trivial against the measured 121 KB free, and generous above the "dozens, not
   hundreds/thousands" mesh-node population docs/WARDRIVING_PUBLISH.md expects. If `malloc()`
   ever fails (essentially unreachable given the measured margin, but checked anyway per this
   codebase's "check every esp_err_t/allocation" discipline), dedup is simply disabled --
   `mesh_log_record_sighting()` still stores every sighting, just without ever suppressing a
   repeat, rather than treating an allocation failure as fatal to the whole capability.

   **Eviction policy once the table is full**: none -- new, not-yet-seen node_ids are simply
   no longer deduped once `ML_DEDUP_CAPACITY` entries are populated (a one-time warning is
   logged the first time this happens). No LRU/oldest-eviction scheme was built: mesh nodes are
   expected sparse enough that 128 distinct nodes in one board's lifetime-before-reboot is
   already an unlikely ceiling to reach, and wdgwars.pl's own server-side dedup ("what counts is
   that the node exists, not how many times you've seen it" -- docs/WARDRIVING_PUBLISH.md)
   already tolerates an occasional duplicate upload without penalty, so silently allowing
   through the rare overflow case is an acceptable simplification, not a hidden bug.

   **Reboot caveat, closed (not merely accepted)**: an earlier version of this switch left the
   heap table starting empty on every boot, reopening the persistence property the flash-scan
   fallback it replaced had incidentally provided. Fixed the same day: `mesh_log_init()` now
   seeds `ml_dedup_entries` from the existing flash log once, at boot, before creating
   `ml_mutex` -- see the record-walking block inside `mesh_log_init()`'s resumed-partition path,
   which decodes every still-present record (drained or not, not just undrained ones -- a node
   logged once, drained, and later re-heard must still not re-append) and calls
   `ml_dedup_insert()` for each distinct `node_id` found, up to `ML_DEDUP_CAPACITY`. This reuses
   the same walk that already existed there for `ml_undrained_in_sector`/oldest-cursor
   bookkeeping rather than adding a second pass over the same sectors, so it costs no extra
   flash reads beyond what init already did -- purely a one-time boot-time cost bounded by this
   partition's small size (4 sectors), not a recurring one, and adds no new `.bss`. A boot log
   line ("mesh log dedup table seeded with N entries from existing log") reports how many
   entries this pass populated, for future hardware sessions to sanity-check. If the log ever
   holds more distinct node_ids than `ML_DEDUP_CAPACITY` at boot, the same "warn once, stop
   deduping new ones past capacity" policy above applies -- no separate eviction scheme needed
   for the seed pass. Net effect: this heap table now gets both the fast O(1)-ish runtime lookup
   the switch to heap storage was for, and the flash log's own persistence across reboots the
   original flash-scan fallback had -- not a trade-off between the two.

   Every static byte in this file was fought for against this board's ~120-byte (pre-existing)
   `.dram0.bss` headroom (docs/BACKLOG.md BL23), which turned out to have essentially zero
   room left once this capability's other tables/scratch buffers were already accounted for --
   getting a clean `idf.py build` needed real structural cuts, not just narrower field widths:
   sector/offset bookkeeping fields are `uint8_t`/`uint16_t`, not `size_t`; `ML_MAX_SECTORS` is
   a fixed compile-time constant used directly in arithmetic rather than a discovered-at-init
   field (this partition, unlike wardriving's, is entirely this capability's own and sized to
   match exactly -- see that constant's own comment); there is no persisted pending-count or
   per-sector "generation" field beyond what's genuinely needed across calls
   (ml_compute_pending_count() sums on demand instead); the init-only sector-occupied/
   generation scan arrays are plain function locals inside mesh_log_init() (stack, not `.bss`);
   mesh_log_peek_pending() takes a caller-supplied scratch buffer (mesh_log.h's top comment);
   and mesh_log_record_sighting()'s own CBOR-encode scratch buffer is a stack local too (that
   function's own comment justifies this specific, narrow exception to this codebase's usual
   static-buffer convention for radio/BLE-callback-path code). The new heap-allocated dedup
   table above adds only a pointer plus a small count/flag to this file's own `.bss` footprint
   (see this file's dedup-strategy comment above) -- its actual entry storage lives on the
   heap, outside this budget entirely. */
#include "mesh_log.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mesh_log";

#define ML_PARTITION_NAME "meshlog"
/* Exact match to heltec/partitions.csv's "meshlog" partition size (0x4000 / ML_SECTOR_SIZE
   4096 = 4 sectors) -- sized this small (not wardriving_log.c's own headroom-above-actual
   convention) because every sector costs bookkeeping bytes this board can't spare (see this
   file's top comment), and 4 sectors already holds well over a hundred tightly-packed
   records -- generous above the "dozens, not hundreds/thousands" population
   docs/WARDRIVING_PUBLISH.md expects. A partition larger than this is silently truncated to
   this many usable sectors (logged once at init), same defensive treatment as
   wardriving_log.c's own bound. */
#define ML_MAX_SECTORS 4u

/* Field declaration order below is deliberate, not incidental: 4-byte-aligned fields first,
   then 2-byte, then 1-byte last -- an attempt to group every static here by its own natural
   alignment so the linker's per-symbol `.bss` placement needs minimal padding between them
   (the same discipline meshcore_table_entry_t's own field-order comment documents), though
   this session's own `idf.py build`/map-file inspection found the linker's actual per-symbol
   placement doesn't strictly follow source declaration order regardless -- some padding here
   is not fully controllable from this file alone. Reordering these is still not a purely
   cosmetic choice given how tight this board's `.dram0.bss` headroom is (docs/BACKLOG.md
   BL23), just not a guaranteed-precise lever the way it would be in a plain struct. */
static const esp_partition_t *ml_partition;
static SemaphoreHandle_t ml_mutex;

/* uint16_t, not wardriving_log.c's own uint32_t generation counters -- this log rarely
   evicts at all in practice (dedup means a given node_id is never re-appended once recorded,
   and mesh nodes are sparse, so the log plausibly never even fills its 4 sectors over this
   board's realistic uptime); 65,536 sector reallocations is generous headroom for a counter
   that a wardriving-scale, constantly-cycling log would need 32 bits for, but this one does
   not. */
static uint16_t ml_next_generation;
static uint16_t ml_write_offset;
/* Oldest *undrained* record -- drives peek/mark-drained batching, exactly like
   wardriving_log.c's wd_oldest_sector/wd_oldest_offset. No separate "absolute oldest
   surviving" cursor exists here (an earlier version of this file had one, needed only by the
   flash-scan dedup approach this module no longer uses -- see this file's top comment on the
   switch to a heap-allocated dedup table, which never touches the flash log at all and so has
   no use for such a cursor either). */
static uint16_t ml_oldest_offset;
/* Persistent at runtime (unlike the init-only occupied/generation scan, kept local to
   mesh_log_init() -- see this file's top comment): how many undrained records currently sit
   in each sector, needed by eviction (ml_roll_to_next_sector()). Also this module's only
   pending-count storage -- no separate running total is kept; ml_compute_pending_count()
   below sums this 4-entry array on demand instead, trading a handful of cheap add
   instructions per call for one less persistent field (every byte here was fought for, see
   this file's top comment). */
static uint16_t ml_undrained_in_sector[ML_MAX_SECTORS];
static uint16_t ml_peek_offset;

static uint8_t ml_active_sector;
static uint8_t ml_oldest_sector;
/* Populated by the most recent mesh_log_peek_pending() call; mesh_log_mark_drained()
   consumes this instead of re-walking the log, matching wardriving_log.c's own
   peek/mark-drained consistency-by-construction convention, just for a single slot
   (FEB_MESH_LOG_MAX_RECORDS_PER_BATCH == 1). Deliberately does NOT also cache "the position
   right after this record" the way wardriving_log.c's own wd_peek_sector[N]/wd_peek_offset[N]
   do -- mesh_log_mark_drained() re-reads this one record's header instead (one extra cheap
   flash read on an infrequent call) to save the field that would otherwise cost. */
static uint8_t ml_peek_sector;
static bool ml_active_sector_closed;
static bool ml_peek_valid;

/* Heap-allocated dedup table (see this file's top comment for the full rationale/sizing/
   eviction-policy discussion). Only these three symbols live in `.bss`; ML_DEDUP_CAPACITY *
   sizeof(ml_dedup_entry_t) bytes of actual entry storage is malloc()'d once in
   mesh_log_init() and never touches the static budget BL23/BL24 track. */
#define ML_DEDUP_CAPACITY 128u

typedef struct {
    uint8_t len; /* 1..FEB_MESH_LOG_NODE_ID_MAX_LEN; 0 means this slot is unused */
    char id[FEB_MESH_LOG_NODE_ID_MAX_LEN]; /* raw id bytes, not NUL-terminated */
} ml_dedup_entry_t;

static ml_dedup_entry_t *ml_dedup_entries; /* NULL if malloc() failed at init -- dedup then a no-op */
static uint16_t ml_dedup_count;
static bool ml_dedup_table_full_warned;

static bool ml_dedup_contains(const char *node_id, size_t node_id_len)
{
    size_t i;

    if (ml_dedup_entries == NULL) {
        return false;
    }
    for (i = 0; i < ml_dedup_count; i++) {
        if (ml_dedup_entries[i].len == node_id_len &&
            memcmp(ml_dedup_entries[i].id, node_id, node_id_len) == 0) {
            return true;
        }
    }
    return false;
}

/* No-op (not an error) once ML_DEDUP_CAPACITY is reached -- see this file's top comment on why
   this is an accepted simplification rather than an LRU/oldest-eviction scheme. Only ever
   called after a fresh sighting has already been confirmed absent (ml_dedup_contains()
   returned false) and successfully appended to flash, so there is no risk of inserting a
   duplicate entry here. */
static void ml_dedup_insert(const char *node_id, size_t node_id_len)
{
    if (ml_dedup_entries == NULL) {
        return;
    }
    if (ml_dedup_count >= ML_DEDUP_CAPACITY) {
        if (!ml_dedup_table_full_warned) {
            ESP_LOGW(TAG, "mesh log dedup table full (%u entries); further new node_ids will not be deduped",
                     (unsigned)ML_DEDUP_CAPACITY);
            ml_dedup_table_full_warned = true;
        }
        return;
    }
    ml_dedup_entries[ml_dedup_count].len = (uint8_t)node_id_len;
    memcpy(ml_dedup_entries[ml_dedup_count].id, node_id, node_id_len);
    ml_dedup_count++;
}

static size_t ml_compute_pending_count(void)
{
    size_t total = 0;
    size_t i;

    for (i = 0; i < ML_MAX_SECTORS; i++) {
        total += ml_undrained_in_sector[i];
    }
    return total;
}

static const char *mesh_log_network_to_string(mesh_log_network_t network)
{
    switch (network) {
    case MESH_LOG_NETWORK_MESHTASTIC:
        return "meshtastic";
    case MESH_LOG_NETWORK_MESHCORE:
    default:
        return "meshcore";
    }
}

static bool ml_read(size_t sector, size_t offset, void *out, size_t len)
{
    esp_err_t err = esp_partition_read(ml_partition, sector * ML_SECTOR_SIZE + offset, out, len);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_read(sector=%u,offset=%u,len=%u) failed: %s",
                 (unsigned)sector, (unsigned)offset, (unsigned)len, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool ml_write(size_t sector, size_t offset, const void *data, size_t len)
{
    esp_err_t err = esp_partition_write(ml_partition, sector * ML_SECTOR_SIZE + offset, data, len);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_write(sector=%u,offset=%u,len=%u) failed: %s",
                 (unsigned)sector, (unsigned)offset, (unsigned)len, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool ml_erase_sector(size_t sector)
{
    esp_err_t err = esp_partition_erase_range(ml_partition, sector * ML_SECTOR_SIZE, ML_SECTOR_SIZE);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_erase_range(sector=%u) failed: %s", (unsigned)sector, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Mirrors wardriving_log.c's wd_try_read_valid_record() exactly (header magic, in-sector
   bounds, CRC) -- see that file's comment. payload_out must be >= ML_RECORD_MAX_PAYLOAD
   bytes when non-NULL. */
static bool ml_try_read_valid_record(size_t sector, size_t offset, ml_record_header_t *parsed,
                                     uint8_t *payload_out)
{
    uint8_t rec_header[ML_RECORD_HEADER_SIZE];
    uint32_t crc;

    if (offset + ML_RECORD_HEADER_SIZE > ML_SECTOR_SIZE) {
        return false;
    }
    if (!ml_read(sector, offset, rec_header, sizeof(rec_header))) {
        return false;
    }
    if (!ml_record_header_parse(rec_header, parsed)) {
        return false;
    }
    if (parsed->payload_len > ML_RECORD_MAX_PAYLOAD ||
        offset + ml_record_on_flash_size(parsed->payload_len) > ML_SECTOR_SIZE) {
        return false;
    }
    if (payload_out == NULL) {
        return true;
    }
    if (!ml_read(sector, offset + ML_RECORD_HEADER_SIZE, payload_out, parsed->payload_len)) {
        return false;
    }
    crc = ml_crc32(payload_out, parsed->payload_len);
    if (crc != parsed->crc32) {
        return false;
    }
    return true;
}

static bool ml_roll_to_next_sector(void)
{
    size_t next_sector = ((size_t)ml_active_sector + 1u) % ML_MAX_SECTORS;
    uint8_t header[ML_SECTOR_HEADER_SIZE];

    if (ml_read(next_sector, 0, header, sizeof(header)) &&
        ml_sector_header_parse(header, NULL)) {
        /* next_sector already holds a valid (older) sector header -- by this module's strict
           round-robin allocation invariant, the log has wrapped all the way around and this
           is the physically-oldest surviving sector; reclaim it, discarding whatever it holds
           regardless of drained state (dedup no longer reads the flash log at all -- see this
           file's top comment on the heap-table switch -- so there is nothing here to keep in
           sync when a sector is reclaimed). */
        ESP_LOGW(TAG, "mesh log full; evicting sector %u (%u undrained record(s) lost)",
                 (unsigned)next_sector, (unsigned)ml_undrained_in_sector[next_sector]);
        ml_undrained_in_sector[next_sector] = 0;
        if (ml_oldest_sector == next_sector) {
            /* The reclaimed sector held the current oldest-undrained cursor -- advance it
               past the sector being erased. mesh_log_peek_pending() re-validates whatever it
               finds there before trusting it either way (same defensive posture
               wardriving_log_peek_pending() takes), so a momentarily-stale cursor here is not
               a correctness risk even in a pathological ordering. */
            ml_oldest_sector = (uint8_t)((next_sector + 1u) % ML_MAX_SECTORS);
            ml_oldest_offset = ML_SECTOR_HEADER_SIZE;
        }
    }

    if (!ml_erase_sector(next_sector)) {
        return false;
    }
    ml_sector_header_pack(header, ml_next_generation++);
    if (!ml_write(next_sector, 0, header, sizeof(header))) {
        return false;
    }
    ml_active_sector = (uint8_t)next_sector;
    ml_write_offset = ML_SECTOR_HEADER_SIZE;
    ml_active_sector_closed = false;
    ml_undrained_in_sector[next_sector] = 0;
    return true;
}

void mesh_log_init(void)
{
    uint8_t header[ML_SECTOR_HEADER_SIZE];
    size_t i;
    size_t newest;
    size_t oldest;
    uint32_t max_gen = 0;
    /* Init-scan-only bookkeeping -- plain locals (stack), never read again after this
       function returns (mirrors wardriving_log.c's own equivalent arrays' lifetime; see this
       file's top comment on why these aren't `static` here). */
    bool sector_occupied[ML_MAX_SECTORS];
    uint32_t sector_generation[ML_MAX_SECTORS];

    ml_peek_valid = false;
    memset(ml_undrained_in_sector, 0, sizeof(ml_undrained_in_sector));
    memset(sector_occupied, 0, sizeof(sector_occupied));
    memset(sector_generation, 0, sizeof(sector_generation));

    /* Heap-allocated once here, not `static` (see this file's top comment) -- draws from the
       121808-byte free-heap reading a real hardware session measured 2026-09-27 (before
       Wi-Fi/BLE init; see docs/BACKLOG.md BL24), not this file's own exhausted `.dram0.bss`
       budget. A malloc() failure here does not disable mesh_log itself (the partition/mutex
       setup below is unaffected either way) -- it only disables dedup, which then degrades to
       "every sighting is appended, never suppressed" rather than being treated as fatal. */
    ml_dedup_entries = (ml_dedup_entry_t *)malloc(ML_DEDUP_CAPACITY * sizeof(ml_dedup_entry_t));
    ml_dedup_count = 0;
    ml_dedup_table_full_warned = false;
    if (ml_dedup_entries == NULL) {
        ESP_LOGW(TAG, "mesh log dedup table allocation failed (%u bytes); dedup disabled, "
                      "sightings will not be deduped",
                 (unsigned)(ML_DEDUP_CAPACITY * sizeof(ml_dedup_entry_t)));
    }

    /* No separate `ml_ready` flag (an earlier version of this file had one) -- every public
       function below already gates on `ml_mutex == NULL` as its very first check, so this
       function deliberately creates the mutex only as the LAST step of each successful path
       below (both the fresh-partition and resumed-partition returns), never on an early
       failure return -- `ml_mutex != NULL` alone is therefore an exact proxy for "fully
       initialized", saving one field on a board where every static byte was fought for (see
       this file's top comment). Init itself runs single-threaded at boot, before any other
       task could call into this module, so nothing here needs the mutex's protection yet. */
    ml_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                            ML_PARTITION_NAME);
    if (ml_partition == NULL) {
        ESP_LOGE(TAG, "\"%s\" partition not found; mesh_log disabled", ML_PARTITION_NAME);
        return;
    }

    {
        /* ML_MAX_SECTORS is a fixed compile-time constant used directly in every sector-index
           calculation below (not a discovered-at-runtime field like wardriving_log.c's own
           wd_sector_count) -- unlike that shared-history partition, this one is entirely this
           capability's own, sized to match exactly (see heltec/partitions.csv), so there is no
           stored field's worth of DRAM to spend remembering a number this build already knows
           at compile time. Still validated here defensively: a partition genuinely smaller
           than expected is a real misconfiguration this refuses to silently paper over. */
        size_t sector_count = ml_partition->size / ML_SECTOR_SIZE;

        if (sector_count > ML_MAX_SECTORS) {
            ESP_LOGW(TAG, "partition has %u sectors, exceeding this build's %u-sector bookkeeping "
                          "bound; only the first %u sectors are usable",
                     (unsigned)sector_count, (unsigned)ML_MAX_SECTORS, (unsigned)ML_MAX_SECTORS);
        } else if (sector_count < ML_MAX_SECTORS) {
            ESP_LOGE(TAG, "\"%s\" partition has only %u sectors, fewer than this build's "
                          "%u-sector bookkeeping bound; mesh_log disabled",
                     ML_PARTITION_NAME, (unsigned)sector_count, (unsigned)ML_MAX_SECTORS);
            return;
        }
    }

    for (i = 0; i < ML_MAX_SECTORS; i++) {
        uint32_t gen;

        if (!ml_read(i, 0, header, sizeof(header))) {
            sector_occupied[i] = false;
            continue;
        }
        if (ml_sector_header_parse(header, &gen)) {
            sector_occupied[i] = true;
            sector_generation[i] = gen;
            if (gen > max_gen) {
                max_gen = gen;
            }
        } else if (ml_sector_header_is_erased(header)) {
            sector_occupied[i] = false;
        } else {
            ESP_LOGW(TAG, "sector %u has a torn header; re-erasing", (unsigned)i);
            ml_erase_sector(i);
            sector_occupied[i] = false;
        }
    }

    ml_next_generation = (uint16_t)(max_gen + 1u);
    newest = ml_find_newest_generation_index(sector_occupied, sector_generation, ML_MAX_SECTORS);

    if (newest == ML_MAX_SECTORS) {
        /* Fresh partition. */
        ml_active_sector = 0;
        if (!ml_erase_sector(ml_active_sector)) {
            return;
        }
        ml_sector_header_pack(header, ml_next_generation++);
        if (!ml_write(ml_active_sector, 0, header, sizeof(header))) {
            return;
        }
        ml_write_offset = ML_SECTOR_HEADER_SIZE;
        ml_active_sector_closed = false;
        ml_oldest_sector = ml_active_sector;
        ml_oldest_offset = ml_write_offset;
        ml_mutex = xSemaphoreCreateMutex();
        if (ml_mutex == NULL) {
            ESP_LOGE(TAG, "failed to create mutex; mesh_log disabled");
            return;
        }
        ESP_LOGI(TAG, "mesh log initialized fresh (%u sectors x %u bytes)",
                 (unsigned)ML_MAX_SECTORS, (unsigned)ML_SECTOR_SIZE);
        /* Nothing to seed -- a genuinely fresh partition holds no records at all yet, so
           ml_dedup_count is trivially 0 here. Logged anyway for symmetry with the resumed
           path below, so a future boot log never leaves the reader guessing whether seeding
           ran at all. */
        ESP_LOGI(TAG, "mesh log dedup table seeded with 0 entries from existing log");
        return;
    }

    ml_active_sector = (uint8_t)newest;

    {
        size_t offset = ML_SECTOR_HEADER_SIZE;
        ml_record_header_t parsed;

        while (ml_try_read_valid_record(ml_active_sector, offset, &parsed, NULL)) {
            offset += ml_record_on_flash_size(parsed.payload_len);
        }
        if (offset + ML_RECORD_HEADER_SIZE > ML_SECTOR_SIZE) {
            ml_active_sector_closed = true;
        } else {
            uint8_t rec_header[ML_RECORD_HEADER_SIZE];

            if (ml_read(ml_active_sector, offset, rec_header, sizeof(rec_header)) &&
                ml_record_header_is_erased(rec_header)) {
                ml_active_sector_closed = false;
            } else {
                ESP_LOGW(TAG, "active sector %u has a torn/corrupt record at offset %u; "
                              "closing it for new writes",
                         (unsigned)ml_active_sector, (unsigned)offset);
                ml_active_sector_closed = true;
            }
        }
        ml_write_offset = (uint16_t)offset;
    }

    oldest = ml_find_oldest_generation_index(sector_occupied, sector_generation, ML_MAX_SECTORS);

    {
        size_t occupied_total = 0;
        size_t sector = oldest;
        bool found_oldest_undrained = false;

        for (i = 0; i < ML_MAX_SECTORS; i++) {
            if (sector_occupied[i]) {
                occupied_total++;
            }
        }

        /* This walk decodes *every* still-present record, drained or not (payload_out is
           always non-NULL, so ml_try_read_valid_record() CRC-validates every one regardless)
           -- not just undrained ones. That is deliberate, not incidental: the undrained-only
           bookkeeping below (ml_undrained_in_sector[]/ml_oldest_sector/offset) only needs
           undrained records, but seeding the dedup table below needs every node_id this log
           still holds *at all*, including already-drained ones -- a node logged once, drained
           after a successful send, and later re-heard should still not re-append (this is
           exactly the persistence property a plain heap table starting empty every boot would
           otherwise lose -- see this file's top comment on the dedup-table/reboot-caveat
           history). Reusing this single existing walk for both purposes, rather than adding a
           second independent pass over the same sectors, keeps this a one-time boot-time cost
           without doubling the flash reads it takes. */
        for (i = 0; i < occupied_total; i++) {
            size_t offset = ML_SECTOR_HEADER_SIZE;
            ml_record_header_t parsed;
            uint8_t payload_buf[ML_RECORD_MAX_PAYLOAD];

            while (ml_try_read_valid_record(sector, offset, &parsed, payload_buf)) {
                feb_mesh_log_record_t record = {0};
                feb_cbor_status_t decode_status = FEB_CBOR_OK;
                bool decoded = feb_cbor_decode_mesh_log_record(payload_buf, parsed.payload_len,
                                                               &record, &decode_status) != 0;

                if (!decoded) {
                    if (parsed.undrained) {
                        ESP_LOGW(TAG,
                                 "mesh log: stored record at sector %u offset %u failed to decode during resume (status %d); discarding stale record",
                                 (unsigned)sector, (unsigned)offset, (int)decode_status);
                    }
                    /* A drained record that fails to decode has nothing else depending on it
                       (no pending-count/oldest-cursor bookkeeping to keep correct, unlike the
                       undrained case above) -- skip it silently for dedup-seeding too. */
                } else {
                    if (!ml_dedup_contains(record.node_id, record.node_id_len)) {
                        ml_dedup_insert(record.node_id, record.node_id_len);
                    }
                    if (parsed.undrained) {
                        ml_undrained_in_sector[sector]++;
                        if (!found_oldest_undrained) {
                            ml_oldest_sector = (uint8_t)sector;
                            ml_oldest_offset = (uint16_t)offset;
                            found_oldest_undrained = true;
                        }
                    }
                }
                offset += ml_record_on_flash_size(parsed.payload_len);
            }
            sector = (sector + 1u) % ML_MAX_SECTORS;
        }

        if (!found_oldest_undrained) {
            ml_oldest_sector = ml_active_sector;
            ml_oldest_offset = ml_write_offset;
        }
    }

    ml_mutex = xSemaphoreCreateMutex();
    if (ml_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create mutex; mesh_log disabled");
        return;
    }
    ESP_LOGI(TAG, "mesh log dedup table seeded with %u entr%s from existing log",
             (unsigned)ml_dedup_count, ml_dedup_count == 1 ? "y" : "ies");
    ESP_LOGI(TAG, "mesh log resumed: active sector %u (gen %u), %u pending record(s)",
             (unsigned)ml_active_sector, (unsigned)sector_generation[newest],
             (unsigned)ml_compute_pending_count());
}

bool mesh_log_record_sighting(const char *node_id_hex, size_t node_id_hex_len,
                              mesh_log_network_t network, int32_t lat_e7, int32_t lon_e7)
{
    /* Plain local, not `static` -- a deliberate, narrow exception to this codebase's usual
       "non-trivial buffers on a BLE/radio-callback path default to file-scope static storage"
       convention (heltec-developer.md; see docs/LESSONS.md's task-stack-overflow writeups on
       why that convention exists). Justification, since this is exactly the class of code
       that convention is about: this function runs on `lora_shared_radio.cpp`'s LoRa RX task,
       whose stack (MESHCORE_RADIO_TASK_STACK_SIZE, 4096 bytes) already carries comparable
       locals in the same shallow, non-recursive call chain (lora_handle_meshcore_frame()'s own
       `hex_buf`); this function adds only ML_RECORD_MAX_PAYLOAD (83) bytes plus a handful of
       scalars, a small fraction of that budget. This buffer was `static` in an earlier version
       of this file -- moved to the stack specifically because this board's `.dram0.bss` had no
       remaining room at all (docs/BACKLOG.md BL23) once this capability's other fields were
       already cut as far as they safely could be; a real `-fstack-usage`/hardware
       high-water-mark check (this session had neither compiler flag output nor physical
       hardware available) should confirm this before being fully trusted, same as any other
       tight-budget buffer per that same convention -- flagged here, not silently assumed. */
    uint8_t scratch[ML_RECORD_MAX_PAYLOAD];
    feb_mesh_log_record_t record = {0};
    const char *network_str;
    size_t encoded_len;
    size_t needed;
    uint8_t rec_header[ML_RECORD_HEADER_SIZE];
    uint32_t crc;
    bool result = true;

    if (node_id_hex == NULL || node_id_hex_len == 0 ||
        node_id_hex_len > FEB_MESH_LOG_NODE_ID_MAX_LEN || ml_mutex == NULL) {
        return false;
    }
    network_str = mesh_log_network_to_string(network);

    xSemaphoreTake(ml_mutex, portMAX_DELAY);

    if (ml_dedup_contains(node_id_hex, node_id_hex_len)) {
        goto out; /* already known -- silent no-op, not a failure */
    }

    record.node_id = node_id_hex;
    record.node_id_len = node_id_hex_len;
    record.network = network_str;
    record.network_len = strlen(network_str);
    record.lat_e7_offset = (uint64_t)((int64_t)lat_e7 + 900000000LL);
    record.lon_e7_offset = (uint64_t)((int64_t)lon_e7 + 1800000000LL);

    encoded_len = feb_cbor_encode_mesh_log_record(scratch, sizeof(scratch), &record);
    if (encoded_len == 0) {
        ESP_LOGE(TAG, "mesh_log record encode failed (node_id_len=%u, network=%s)",
                 (unsigned)node_id_hex_len, network_str);
        result = false;
        goto out;
    }

    needed = ml_record_on_flash_size(encoded_len);
    if (ml_active_sector_closed || (size_t)ml_write_offset + needed > ML_SECTOR_SIZE) {
        if (!ml_roll_to_next_sector()) {
            result = false;
            goto out;
        }
    }

    crc = ml_crc32(scratch, encoded_len);
    ml_record_header_pack(rec_header, (uint16_t)encoded_len, true, crc);

    if (!ml_write(ml_active_sector, ml_write_offset, rec_header, sizeof(rec_header)) ||
        !ml_write(ml_active_sector, (size_t)ml_write_offset + ML_RECORD_HEADER_SIZE, scratch, encoded_len)) {
        ml_active_sector_closed = true;
        result = false;
        goto out;
    }

    if (ml_compute_pending_count() == 0) {
        ml_oldest_sector = ml_active_sector;
        ml_oldest_offset = ml_write_offset;
    }
    ml_undrained_in_sector[ml_active_sector]++;
    ml_write_offset = (uint16_t)((size_t)ml_write_offset + needed);
    ml_dedup_insert(node_id_hex, node_id_hex_len);
    ESP_LOGI(TAG, "mesh_log: recorded new %s node %.*s", network_str, (int)node_id_hex_len, node_id_hex);

out:
    xSemaphoreGive(ml_mutex);
    return result;
}

size_t mesh_log_pending_count(void)
{
    size_t count;

    if (ml_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(ml_mutex, portMAX_DELAY);
    count = ml_compute_pending_count();
    xSemaphoreGive(ml_mutex);
    return count;
}

size_t mesh_log_peek_pending(feb_mesh_log_record_t *out, size_t max_records,
                             uint8_t *scratch_buf, size_t scratch_buf_len)
{
    size_t produced = 0;

    ml_peek_valid = false;
    if (out == NULL || max_records == 0 || scratch_buf == NULL ||
        scratch_buf_len < ML_RECORD_MAX_PAYLOAD || ml_mutex == NULL) {
        return 0;
    }

    xSemaphoreTake(ml_mutex, portMAX_DELAY);

    if (ml_compute_pending_count() > 0) {
        size_t sector = ml_oldest_sector;
        size_t offset = ml_oldest_offset;
        ml_record_header_t parsed;
        feb_cbor_status_t decode_status = FEB_CBOR_OK;

        while (ml_try_read_valid_record(sector, offset, &parsed, NULL) && !parsed.undrained) {
            /* Already drained -- shouldn't happen (ml_oldest_offset always tracks the first
               undrained record), but skip forward defensively rather than get stuck, same
               convention as wardriving_log_peek_pending(). */
            offset += ml_record_on_flash_size(parsed.payload_len);
            if (offset + ML_RECORD_HEADER_SIZE > ML_SECTOR_SIZE) {
                sector = (sector + 1u) % ML_MAX_SECTORS;
                offset = ML_SECTOR_HEADER_SIZE;
            }
        }

        if (ml_try_read_valid_record(sector, offset, &parsed, scratch_buf) &&
            feb_cbor_decode_mesh_log_record(scratch_buf, parsed.payload_len, &out[0],
                                            &decode_status) != 0) {
            ml_peek_sector = (uint8_t)sector;
            ml_peek_offset = (uint16_t)offset;
            ml_peek_valid = true;
            produced = 1;
        } else if (ml_try_read_valid_record(sector, offset, &parsed, NULL)) {
            ESP_LOGW(TAG, "mesh log: stored record at sector %u offset %u failed to decode; "
                          "discarding stale record",
                     (unsigned)sector, (unsigned)offset);
            {
                uint8_t flag_byte = (uint8_t)(0xFFu & ~ML_RECORD_FLAG_UNDRAINED);

                if (ml_write(sector, offset + 6u, &flag_byte, 1u)) {
                    if (ml_undrained_in_sector[sector] > 0u) {
                        ml_undrained_in_sector[sector]--;
                    }
                    if (ml_oldest_sector == sector && ml_oldest_offset == offset) {
                        size_t next_offset = offset + ml_record_on_flash_size(parsed.payload_len);

                        if (next_offset + ML_RECORD_HEADER_SIZE > ML_SECTOR_SIZE) {
                            ml_oldest_sector = (uint8_t)((sector + 1u) % ML_MAX_SECTORS);
                            ml_oldest_offset = ML_SECTOR_HEADER_SIZE;
                        } else {
                            ml_oldest_offset = (uint16_t)next_offset;
                        }
                    }
                }
            }
        }
    }

    xSemaphoreGive(ml_mutex);
    return produced > max_records ? max_records : produced;
}

void mesh_log_mark_drained(size_t count)
{
    if (ml_mutex == NULL || count == 0) {
        return;
    }
    xSemaphoreTake(ml_mutex, portMAX_DELAY);

    if (!ml_peek_valid) {
        xSemaphoreGive(ml_mutex);
        return;
    }

    {
        uint8_t flag_byte = (uint8_t)(0xFFu & ~ML_RECORD_FLAG_UNDRAINED);

        if (!ml_write(ml_peek_sector, ml_peek_offset + 6u, &flag_byte, 1u)) {
            ESP_LOGW(TAG, "failed to mark mesh log record at sector %u offset %u drained; will be resent",
                     (unsigned)ml_peek_sector, (unsigned)ml_peek_offset);
        } else {
            /* Re-read this record's own header to learn its on-flash size, rather than
               caching "the position right after it" the way wardriving_log.c's own
               wd_peek_sector[N]/wd_peek_offset[N] do -- see ml_peek_sector's own comment on
               why this module trades one small extra flash read here for two fewer
               persistent fields. undrained now reads back false (just cleared above), so
               this only needs payload_len, not re-validation of the CRC. */
            ml_record_header_t parsed;
            size_t next_offset = ml_peek_offset;

            if (ml_undrained_in_sector[ml_peek_sector] > 0) {
                ml_undrained_in_sector[ml_peek_sector]--;
            }
            if (ml_try_read_valid_record(ml_peek_sector, ml_peek_offset, &parsed, NULL)) {
                next_offset += ml_record_on_flash_size(parsed.payload_len);
            }
            if (next_offset + ML_RECORD_HEADER_SIZE > ML_SECTOR_SIZE) {
                ml_oldest_sector = (uint8_t)((ml_peek_sector + 1u) % ML_MAX_SECTORS);
                ml_oldest_offset = ML_SECTOR_HEADER_SIZE;
            } else {
                ml_oldest_sector = ml_peek_sector;
                ml_oldest_offset = (uint16_t)next_offset;
            }
        }
    }
    ml_peek_valid = false;

    xSemaphoreGive(ml_mutex);
}

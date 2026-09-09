#include "wardriving_log.h"
#include "wardriving_record_format.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "wardriving_log";

#define WD_PARTITION_NAME "wardrive"
/* Self-imposed bound on this module's boot-scan bookkeeping arrays -- matches (with
   headroom) esp32/partitions.csv's "wardrive" partition size (0x270000) / WD_SECTOR_SIZE
   (4096) = 624 sectors. A partition larger than this is silently truncated to this many
   usable sectors (logged once at init) rather than overflowing a fixed array -- same
   accepted-self-imposed-bound treatment as FEB_WIFI_SCAN_RAW_MAX/FEB_BLE_SCAN_RAW_MAX
   elsewhere in this codebase. */
#define WD_MAX_SECTORS 700u

static const esp_partition_t *wd_partition;
static bool wd_ready;
static size_t wd_sector_count;
static uint32_t wd_next_generation;

static size_t wd_active_sector;
static uint32_t wd_active_generation;
static size_t wd_write_offset;
static bool wd_active_sector_closed;

static size_t wd_oldest_sector;
static size_t wd_oldest_offset;
static size_t wd_pending_count;

static bool wd_sector_occupied[WD_MAX_SECTORS];
static uint32_t wd_sector_generation[WD_MAX_SECTORS];
static uint16_t wd_undrained_in_sector[WD_MAX_SECTORS];

/* Populated by the most recent wardriving_log_peek_pending() call; wardriving_log_mark_drained()
   consumes this instead of re-walking the log, so peek/mark stay consistent with each other
   by construction. wd_peek_{sector,offset}[i] is where record i started;
   wd_peek_{sector,offset}[N] (N = the last peek's return value) is the position immediately
   after the last peeked record -- used by mark_drained() when count == that return value.
   static, not stack-local: this module runs on the NimBLE host task (called from
   write_complete()'s TX_DONE_CONTINUE_WARDRIVING case and the live-capture done-handlers),
   matching main.c's established static-buffer convention for that task's callback path. */
static size_t wd_peek_sector[FEB_WARDRIVING_MAX_RECORDS_PER_BATCH + 1u];
static size_t wd_peek_offset[FEB_WARDRIVING_MAX_RECORDS_PER_BATCH + 1u];
static size_t wd_peek_count;

static bool wd_read(size_t sector, size_t offset, void *out, size_t len)
{
    esp_err_t err = esp_partition_read(wd_partition, sector * WD_SECTOR_SIZE + offset, out, len);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_read(sector=%u,offset=%u,len=%u) failed: %s",
                 (unsigned)sector, (unsigned)offset, (unsigned)len, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool wd_write(size_t sector, size_t offset, const void *data, size_t len)
{
    esp_err_t err = esp_partition_write(wd_partition, sector * WD_SECTOR_SIZE + offset, data, len);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_write(sector=%u,offset=%u,len=%u) failed: %s",
                 (unsigned)sector, (unsigned)offset, (unsigned)len, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool wd_erase_sector(size_t sector)
{
    esp_err_t err = esp_partition_erase_range(wd_partition, sector * WD_SECTOR_SIZE, WD_SECTOR_SIZE);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_erase_range(sector=%u) failed: %s", (unsigned)sector, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Attempts to read and fully validate (header magic, in-sector bounds, CRC) one record at
   (sector,offset). Returns true and fills *parsed on success. Returns false for anything
   else -- erased space, a torn/corrupt header, an in-sector-bounds violation, or a CRC
   mismatch -- without distinguishing among them: every caller in this file treats "false"
   as "stop trusting this sector's data from here on" (see wardriving_record_format.h's
   crash-safety note). When payload_out is non-NULL (must be >= WD_RECORD_MAX_PAYLOAD
   bytes), the validated payload bytes are left there for the caller to keep; otherwise a
   static scratch buffer is used internally (only for CRC validation, discarded after). */
static uint8_t wd_scratch_payload[WD_RECORD_MAX_PAYLOAD];

static bool wd_try_read_valid_record(size_t sector, size_t offset, wd_record_header_t *parsed,
                                      uint8_t *payload_out)
{
    uint8_t rec_header[WD_RECORD_HEADER_SIZE];
    uint8_t *dest;
    uint32_t crc;

    if (offset + WD_RECORD_HEADER_SIZE > WD_SECTOR_SIZE) {
        return false;
    }
    if (!wd_read(sector, offset, rec_header, sizeof(rec_header))) {
        return false;
    }
    if (!wd_record_header_parse(rec_header, parsed)) {
        return false;
    }
    if (parsed->payload_len > WD_RECORD_MAX_PAYLOAD ||
        offset + wd_record_on_flash_size(parsed->payload_len) > WD_SECTOR_SIZE) {
        return false;
    }
    dest = (payload_out != NULL) ? payload_out : wd_scratch_payload;
    if (!wd_read(sector, offset + WD_RECORD_HEADER_SIZE, dest, parsed->payload_len)) {
        return false;
    }
    crc = wd_crc32(dest, parsed->payload_len);
    if (crc != parsed->crc32) {
        return false;
    }
    return true;
}

static bool wd_roll_to_next_sector(void)
{
    size_t next_sector = (wd_active_sector + 1u) % wd_sector_count;
    uint8_t header[WD_SECTOR_HEADER_SIZE];

    if (next_sector == wd_oldest_sector && wd_pending_count > 0) {
        /* Log is completely full: every sector is occupied and the sector we're about to
           reclaim still holds undrained records. docs/PROTOCOL.md's "Flash log eviction"
           note: evict one whole erase-sector's worth of the oldest records at once, not
           just the single oldest record. */
        size_t skip_sector = (next_sector + 1u) % wd_sector_count;

        ESP_LOGW(TAG, "wardriving log full; evicting sector %u (%u undrained record(s) lost)",
                 (unsigned)next_sector, (unsigned)wd_undrained_in_sector[next_sector]);
        wd_pending_count -= wd_undrained_in_sector[next_sector];
        wd_undrained_in_sector[next_sector] = 0;
        wd_oldest_sector = skip_sector;
        wd_oldest_offset = WD_SECTOR_HEADER_SIZE;
    }

    if (!wd_erase_sector(next_sector)) {
        return false;
    }
    wd_active_generation = wd_next_generation++;
    wd_sector_header_pack(header, wd_active_generation);
    if (!wd_write(next_sector, 0, header, sizeof(header))) {
        return false;
    }
    wd_active_sector = next_sector;
    wd_write_offset = WD_SECTOR_HEADER_SIZE;
    wd_active_sector_closed = false;
    wd_undrained_in_sector[next_sector] = 0;
    return true;
}

void wardriving_log_init(void)
{
    uint8_t header[WD_SECTOR_HEADER_SIZE];
    size_t i;
    size_t newest;
    size_t oldest;
    uint32_t max_gen = 0;

    wd_ready = false;
    wd_pending_count = 0;
    wd_peek_count = 0;
    memset(wd_undrained_in_sector, 0, sizeof(wd_undrained_in_sector));

    wd_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                            WD_PARTITION_NAME);
    if (wd_partition == NULL) {
        ESP_LOGE(TAG, "\"%s\" partition not found; wardriving log disabled", WD_PARTITION_NAME);
        return;
    }

    wd_sector_count = wd_partition->size / WD_SECTOR_SIZE;
    if (wd_sector_count > WD_MAX_SECTORS) {
        ESP_LOGW(TAG, "partition has %u sectors, exceeding this build's %u-sector bookkeeping "
                      "bound; only the first %u sectors are usable",
                 (unsigned)wd_sector_count, (unsigned)WD_MAX_SECTORS, (unsigned)WD_MAX_SECTORS);
        wd_sector_count = WD_MAX_SECTORS;
    }
    if (wd_sector_count == 0) {
        ESP_LOGE(TAG, "\"%s\" partition smaller than one sector; wardriving log disabled",
                 WD_PARTITION_NAME);
        return;
    }

    for (i = 0; i < wd_sector_count; i++) {
        uint32_t gen;

        if (!wd_read(i, 0, header, sizeof(header))) {
            wd_sector_occupied[i] = false;
            continue;
        }
        if (wd_sector_header_parse(header, &gen)) {
            wd_sector_occupied[i] = true;
            wd_sector_generation[i] = gen;
            if (gen > max_gen) {
                max_gen = gen;
            }
        } else if (wd_sector_header_is_erased(header)) {
            wd_sector_occupied[i] = false;
        } else {
            /* Torn sector-header write (a crash mid-eviction): self-heal by erasing now
               rather than trusting anything else about this sector's contents. */
            ESP_LOGW(TAG, "sector %u has a torn header; re-erasing", (unsigned)i);
            wd_erase_sector(i);
            wd_sector_occupied[i] = false;
        }
    }

    wd_next_generation = max_gen + 1u;
    newest = wd_find_newest_generation_index(wd_sector_occupied, wd_sector_generation, wd_sector_count);

    if (newest == wd_sector_count) {
        /* Fresh partition: nothing occupied anywhere. */
        wd_active_sector = 0;
        if (!wd_erase_sector(wd_active_sector)) {
            return;
        }
        wd_active_generation = wd_next_generation++;
        wd_sector_header_pack(header, wd_active_generation);
        if (!wd_write(wd_active_sector, 0, header, sizeof(header))) {
            return;
        }
        wd_write_offset = WD_SECTOR_HEADER_SIZE;
        wd_active_sector_closed = false;
        wd_oldest_sector = wd_active_sector;
        wd_oldest_offset = wd_write_offset;
        wd_ready = true;
        ESP_LOGI(TAG, "wardriving log initialized fresh (%u sectors x %u bytes)",
                 (unsigned)wd_sector_count, (unsigned)WD_SECTOR_SIZE);
        return;
    }

    wd_active_sector = newest;
    wd_active_generation = wd_sector_generation[newest];

    /* Find the append offset within the active sector. */
    {
        size_t offset = WD_SECTOR_HEADER_SIZE;
        wd_record_header_t parsed;

        while (wd_try_read_valid_record(wd_active_sector, offset, &parsed, NULL)) {
            offset += wd_record_on_flash_size(parsed.payload_len);
        }
        /* wd_try_read_valid_record() returning false here could mean "legitimately erased,
           more room available" or "torn/corrupt, sector closed" -- distinguish by checking
           whether this exact position is the fully-erased pattern. */
        if (offset + WD_RECORD_HEADER_SIZE > WD_SECTOR_SIZE) {
            wd_active_sector_closed = true;
        } else {
            uint8_t rec_header[WD_RECORD_HEADER_SIZE];

            if (wd_read(wd_active_sector, offset, rec_header, sizeof(rec_header)) &&
                wd_record_header_is_erased(rec_header)) {
                wd_active_sector_closed = false;
            } else {
                ESP_LOGW(TAG, "active sector %u has a torn/corrupt record at offset %u; "
                              "closing it for new writes",
                         (unsigned)wd_active_sector, (unsigned)offset);
                wd_active_sector_closed = true;
            }
        }
        wd_write_offset = offset;
    }

    /* Full pass over every occupied sector, oldest generation first (== oldest physical,
       per this module's strict-round-robin allocation invariant -- see
       wardriving_record_format.h), to compute total pending_count, per-sector undrained
       counts, and the true oldest-undrained position. */
    oldest = wd_find_oldest_generation_index(wd_sector_occupied, wd_sector_generation, wd_sector_count);
    {
        size_t occupied_total = 0;
        size_t sector = oldest;
        bool found_oldest_undrained = false;

        for (i = 0; i < wd_sector_count; i++) {
            if (wd_sector_occupied[i]) {
                occupied_total++;
            }
        }

        for (i = 0; i < occupied_total; i++) {
            size_t offset = WD_SECTOR_HEADER_SIZE;
            wd_record_header_t parsed;

            while (wd_try_read_valid_record(sector, offset, &parsed, NULL)) {
                if (parsed.undrained) {
                    wd_undrained_in_sector[sector]++;
                    wd_pending_count++;
                    if (!found_oldest_undrained) {
                        wd_oldest_sector = sector;
                        wd_oldest_offset = offset;
                        found_oldest_undrained = true;
                    }
                }
                offset += wd_record_on_flash_size(parsed.payload_len);
            }
            sector = (sector + 1u) % wd_sector_count;
        }

        if (!found_oldest_undrained) {
            /* Caught up: nothing undrained anywhere. Track the current write position, so
               the next fresh append becomes the new oldest (see wardriving_log_append()). */
            wd_oldest_sector = wd_active_sector;
            wd_oldest_offset = wd_write_offset;
        }
    }

    wd_ready = true;
    ESP_LOGI(TAG, "wardriving log resumed: active sector %u (gen %u), %u pending record(s)",
             (unsigned)wd_active_sector, (unsigned)wd_active_generation, (unsigned)wd_pending_count);
}

bool wardriving_log_append(const feb_wardriving_record_t *record)
{
    static uint8_t encode_buf[WD_RECORD_MAX_PAYLOAD];
    size_t encoded_len;
    size_t needed;
    uint8_t rec_header[WD_RECORD_HEADER_SIZE];
    uint32_t crc;
    bool was_empty;

    if (!wd_ready || record == NULL) {
        return false;
    }

    encoded_len = feb_cbor_encode_wardriving_record(encode_buf, sizeof(encode_buf), record);
    if (encoded_len == 0 || encoded_len > 0xFFFFu) {
        ESP_LOGE(TAG, "wardriving record encode failed (or exceeds the %u-byte on-flash bound)",
                 (unsigned)sizeof(encode_buf));
        return false;
    }

    needed = wd_record_on_flash_size(encoded_len);
    if (wd_active_sector_closed || wd_write_offset + needed > WD_SECTOR_SIZE) {
        if (!wd_roll_to_next_sector()) {
            return false;
        }
    }

    crc = wd_crc32(encode_buf, encoded_len);
    wd_record_header_pack(rec_header, (uint16_t)encoded_len, true, crc);

    if (!wd_write(wd_active_sector, wd_write_offset, rec_header, sizeof(rec_header)) ||
        !wd_write(wd_active_sector, wd_write_offset + WD_RECORD_HEADER_SIZE, encode_buf, encoded_len)) {
        /* Best-effort: close this sector for further writes so the next append doesn't try
           to build on top of a possibly-inconsistent write; it rolls to a fresh sector
           instead. */
        wd_active_sector_closed = true;
        return false;
    }

    was_empty = (wd_pending_count == 0);
    wd_undrained_in_sector[wd_active_sector]++;
    wd_pending_count++;
    if (was_empty) {
        wd_oldest_sector = wd_active_sector;
        wd_oldest_offset = wd_write_offset;
    }
    wd_write_offset += needed;
    return true;
}

size_t wardriving_log_pending_count(void)
{
    return wd_ready ? wd_pending_count : 0;
}

size_t wardriving_log_peek_pending(feb_wardriving_record_t *out, size_t max_records,
                                   uint8_t *scratch_buf, size_t scratch_buf_len)
{
    size_t sector;
    size_t offset;
    size_t produced = 0;
    size_t scratch_used = 0;

    wd_peek_count = 0;
    if (!wd_ready || out == NULL || scratch_buf == NULL || max_records == 0) {
        return 0;
    }
    if (max_records > FEB_WARDRIVING_MAX_RECORDS_PER_BATCH) {
        max_records = FEB_WARDRIVING_MAX_RECORDS_PER_BATCH;
    }

    sector = wd_oldest_sector;
    offset = wd_oldest_offset;

    while (produced < max_records && produced < wd_pending_count) {
        wd_record_header_t parsed;
        uint8_t *dest;
        feb_cbor_status_t decode_status = FEB_CBOR_OK;

        if (!wd_try_read_valid_record(sector, offset, &parsed, NULL)) {
            if (sector == wd_active_sector) {
                break; /* genuinely at the live write frontier -- nothing more exists yet */
            }
            /* This (non-active) sector's valid data ends before the physical sector
               boundary -- the normal case at every sector transition, not just a crash
               artifact (see wardriving_record_format.h). Advance to the next sector in the
               oldest-to-active chain. */
            sector = (sector + 1u) % wd_sector_count;
            offset = WD_SECTOR_HEADER_SIZE;
            continue;
        }
        if (!parsed.undrained) {
            /* Already drained -- shouldn't happen, since wd_oldest_offset always tracks the
               first undrained record, but skip forward defensively rather than get stuck. */
            offset += wd_record_on_flash_size(parsed.payload_len);
            continue;
        }
        if (scratch_used + parsed.payload_len > scratch_buf_len) {
            break; /* out of caller-supplied scratch space for this batch */
        }
        dest = scratch_buf + scratch_used;
        if (!wd_try_read_valid_record(sector, offset, &parsed, dest)) {
            break; /* shouldn't happen (just validated above); stop rather than trust dest */
        }
        if (feb_cbor_decode_wardriving_record(dest, parsed.payload_len, &out[produced], &decode_status) == 0) {
            ESP_LOGW(TAG, "wardriving log: stored record failed to decode (status %d); skipping",
                     (int)decode_status);
            offset += wd_record_on_flash_size(parsed.payload_len);
            continue;
        }
        scratch_used += parsed.payload_len;
        wd_peek_sector[produced] = sector;
        wd_peek_offset[produced] = offset;
        produced++;
        offset += wd_record_on_flash_size(parsed.payload_len);
        if (offset + WD_RECORD_HEADER_SIZE > WD_SECTOR_SIZE) {
            sector = (sector + 1u) % wd_sector_count;
            offset = WD_SECTOR_HEADER_SIZE;
        }
    }
    wd_peek_sector[produced] = sector;
    wd_peek_offset[produced] = offset;
    wd_peek_count = produced;
    return produced;
}

void wardriving_log_mark_drained(size_t count)
{
    size_t i;

    if (!wd_ready) {
        return;
    }
    if (count > wd_peek_count) {
        ESP_LOGE(TAG, "wardriving_log_mark_drained(%u) exceeds last peek's %u result; clamping",
                 (unsigned)count, (unsigned)wd_peek_count);
        count = wd_peek_count;
    }

    for (i = 0; i < count; i++) {
        size_t sector = wd_peek_sector[i];
        size_t offset = wd_peek_offset[i];
        uint8_t flag_byte = (uint8_t)(0xFFu & ~WD_RECORD_FLAG_UNDRAINED);

        if (!wd_write(sector, offset + 6u, &flag_byte, 1)) {
            ESP_LOGW(TAG, "failed to mark record at sector %u offset %u drained; will be resent",
                     (unsigned)sector, (unsigned)offset);
            continue; /* leave it undrained -- at-least-once delivery is acceptable here */
        }
        if (wd_undrained_in_sector[sector] > 0) {
            wd_undrained_in_sector[sector]--;
        }
        if (wd_pending_count > 0) {
            wd_pending_count--;
        }
    }

    if (count > 0) {
        /* wd_peek_{sector,offset}[count] is exactly where the new oldest-undrained record
           starts: either the next peeked-but-not-drained record (count < wd_peek_count), or
           the position right after the last peeked record (count == wd_peek_count) -- both
           cached as a byproduct of the peek walk above. */
        wd_oldest_sector = wd_peek_sector[count];
        wd_oldest_offset = wd_peek_offset[count];
    }
    wd_peek_count = 0; /* consumed -- a stale peek must not be reused by a later call */
}

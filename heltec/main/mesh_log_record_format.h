/* Pure, zero-ESP-IDF-dependency on-flash layout/checksum helpers for mesh_log.c's raw-flash
   circular log (docs/WARDRIVING_PUBLISH.md "Mesh node publishing", design frozen
   2026-09-27). Mirrors wardriving_record_format.h's architecture exactly (own header comment
   there explains the rationale for keeping this pure/host-testable, not repeated here) --
   deliberately a separate module rather than reusing wardriving_record_format.h's own `wd_`
   functions directly, even though they are already generic/payload-type-agnostic: this
   project's convention (meshcore_table.c/meshtastic_table.c each getting their own file
   rather than sharing one generic node-table module) is to mirror an established pattern into
   a textually independent module per capability, not couple two capabilities' on-flash
   formats to the same header/constants just because today's byte layouts happen to match.
   ESP32-only integration (esp_partition_read/write/erase_range) lives in mesh_log.c, exactly
   as wardriving_log.c holds it for wardriving_record_format.h.

   On-flash layout: byte-for-byte identical shape to wardriving_record_format.h's (sector
   header: magic(4) + generation(4); record: magic(4) + payload_len(2) + flags(1) +
   reserved(1) + crc32(4) + payload, padded to ML_RECORD_ALIGN) -- only the magic values and
   ML_RECORD_MAX_PAYLOAD differ, sized for this capability's much smaller
   <mesh-log-record> (node_id/network/lat_e7_offset/lon_e7_offset, see
   components/feb_protocol/cbor_mesh_log.h) rather than wardriving's wifi/ble payload union.
   Crash-safety properties are identical to wardriving_record_format.h's -- see that header's
   comment for the full rationale, not repeated here. */
#ifndef FEB_MESH_LOG_RECORD_FORMAT_H
#define FEB_MESH_LOG_RECORD_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ML_SECTOR_SIZE 4096u
#define ML_SECTOR_MAGIC 0x464D4C53u /* "FMLS" */
#define ML_SECTOR_HEADER_SIZE 8u

#define ML_RECORD_MAGIC 0x464D4C52u /* "FMLR" */
#define ML_RECORD_HEADER_SIZE 12u
#define ML_RECORD_ALIGN 4u
#define ML_RECORD_FLAG_UNDRAINED 0x01u

/* Hand-computed worst case for feb_cbor_encode_mesh_log_record()'s output (cbor_mesh_log.h):
   map header (1) + "node_id" key (8) + a maxed-out 16-char node_id value (17) + "network" key
   (8) + the longer "meshtastic" value (11) + "lat_e7_offset" key (14) + its worst-case 5-byte
   uint encoding (its max value 1,800,000,001 needs CBOR's 4-byte-argument form) +
   "lon_e7_offset" key (14) + its own worst-case 5-byte encoding = 83 bytes exactly -- an
   exact count, not a rough estimate, which is what makes using it directly (zero extra
   margin) an acceptable choice on a board this DRAM-starved (docs/BACKLOG.md BL23) rather
   than a guess; deliberately far less headroom than wardriving_record_format.h's own
   WD_RECORD_MAX_PAYLOAD affords, for the same reason meshtastic_table.h's constants were cut
   tighter than meshcore_table.h's. */
#define ML_RECORD_MAX_PAYLOAD 83u

static inline size_t ml_round_up_align(size_t value)
{
    return (value + (ML_RECORD_ALIGN - 1u)) & ~(size_t)(ML_RECORD_ALIGN - 1u);
}

static inline size_t ml_record_on_flash_size(size_t payload_len)
{
    return ML_RECORD_HEADER_SIZE + ml_round_up_align(payload_len);
}

/* CRC32 (poly 0xEDB88320, reflected), table-free -- same rationale as
   wardriving_record_format.h's wd_crc32() (this capability's records are smaller and rarer
   still, so the table-free cost is even less of a concern). */
uint32_t ml_crc32(const uint8_t *data, size_t len);

void ml_sector_header_pack(uint8_t out[ML_SECTOR_HEADER_SIZE], uint32_t generation);
bool ml_sector_header_parse(const uint8_t in[ML_SECTOR_HEADER_SIZE], uint32_t *out_generation);
bool ml_sector_header_is_erased(const uint8_t in[ML_SECTOR_HEADER_SIZE]);

typedef struct {
    uint16_t payload_len;
    bool undrained;
    uint32_t crc32;
} ml_record_header_t;

void ml_record_header_pack(uint8_t out[ML_RECORD_HEADER_SIZE], uint16_t payload_len,
                           bool undrained, uint32_t crc32);
bool ml_record_header_parse(const uint8_t in[ML_RECORD_HEADER_SIZE], ml_record_header_t *out);
bool ml_record_header_is_erased(const uint8_t in[ML_RECORD_HEADER_SIZE]);

size_t ml_find_oldest_generation_index(const bool *occupied, const uint32_t *generation, size_t count);
size_t ml_find_newest_generation_index(const bool *occupied, const uint32_t *generation, size_t count);

#endif /* FEB_MESH_LOG_RECORD_FORMAT_H */

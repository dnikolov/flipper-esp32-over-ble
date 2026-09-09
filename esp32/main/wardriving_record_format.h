/* Pure, zero-ESP-IDF-dependency on-flash layout/checksum helpers for wardriving_log.c's
   raw-flash circular log (docs/PLAN.md step 8's wardriving-log persistence, pulled forward
   per the 2026-09-07 reorder). Kept in its own header/translation unit, with no dependency
   on esp_partition.h or any other ESP-IDF header, so tests/esp32/test_wardriving_log.c can
   validate checksum/header-packing/eviction-ordering logic on the host without a flash
   driver -- ESP32-only integration (esp_partition_read/write/erase_range calls) lives in
   wardriving_log.c instead. Not part of the shared ESP32/Flipper protocol contract (this
   layout never crosses the wire, is not shared with any Flipper-side source file, and is
   not in tools/check_shared_headers.py's HEADER_PAIRS).

   On-flash layout (all little-endian; this board is little-endian RISC-V, so no explicit
   byte-swapping -- matches this project's existing convention of not targeting a
   big-endian host):

   Partition = wd_sector_count contiguous WD_SECTOR_SIZE-byte sectors, used strictly in
   ascending physical order (0, 1, 2, ..., wd_sector_count-1, wrap to 0, ...) -- eviction
   always reclaims the physically-next sector, never an arbitrary one, so the occupied
   sectors at any moment always form one contiguous circular run from oldest to newest
   (see wardriving_log.c). Each sector:

     offset 0                  : sector header (WD_SECTOR_HEADER_SIZE bytes) --
                                  magic(4) + generation(4). generation is a monotonically
                                  increasing counter assigned when a sector is erased and
                                  put into service; the occupied sector with the highest
                                  generation is the active (currently-being-written-to) one,
                                  the one with the lowest generation is the oldest.
     offset WD_SECTOR_HEADER_SIZE.. : records packed back-to-back, oldest-first, until
                                  either the sector is full or an erased/torn position is
                                  reached.

   Record:
     offset 0  (4 bytes) : magic -- WD_RECORD_MAGIC once a record has been fully written.
                           Reads back as 0xFFFFFFFF over never-written (erased) flash.
     offset 4  (2 bytes) : payload_len (little-endian uint16_t)
     offset 6  (1 byte)  : flags -- bit0 (WD_RECORD_FLAG_UNDRAINED) is 1 until the record
                           has been included in a status(state="data") batch at least once,
                           then cleared to 0 by a single-byte partial-program write. NOR
                           flash can always clear 1 bits to 0 without an erase cycle, so
                           marking a record drained never disturbs the rest of the record
                           and needs no read-modify-write of anything else.
     offset 7  (1 byte)  : reserved, always written as 0xFF
     offset 8  (4 bytes) : crc32 (little-endian) of the payload_len bytes that follow
     offset 12           : payload (payload_len bytes -- the CBOR bytes of one
                           feb_wardriving_record_t as produced by
                           feb_cbor_encode_wardriving_record())
     then padded to a multiple of WD_RECORD_ALIGN bytes so the next record's header starts
     aligned.

   Crash safety: a power loss mid-write leaves at most one torn record (payload
   half-written, or a header whose magic/crc doesn't validate) -- wardriving_log.c's boot
   scan detects this (wd_record_header_parse() returns false for anything that isn't
   exactly the erased pattern or a clean valid-magic header) and abandons the *rest of that
   sector* for future writes rather than trusting anything past the tear (a NOR flash byte
   that was partially programmed can't be blindly reprogrammed without an erase). This loses
   at most one sector's worth of trailing free space per crash, not any already-valid data. */
#ifndef FEB_WARDRIVING_RECORD_FORMAT_H
#define FEB_WARDRIVING_RECORD_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WD_SECTOR_SIZE 4096u
#define WD_SECTOR_MAGIC 0x46454253u /* "FEBS" */
#define WD_SECTOR_HEADER_SIZE 8u

#define WD_RECORD_MAGIC 0x46454252u /* "FEBR" */
#define WD_RECORD_HEADER_SIZE 12u
#define WD_RECORD_ALIGN 4u
#define WD_RECORD_FLAG_UNDRAINED 0x01u

/* Bound on feb_cbor_encode_wardriving_record()'s output for this protocol's
   wardriving-record shape (timestamp_ms/lat_e7_offset/lon_e7_offset/source/payload).
   Hand-computed worst case with a maxed-out 32-byte SSID and this firmware's longest real
   auth string ("wpa3_ext_psk_mixed_mode", 23 bytes) is ~188 bytes; 240 leaves ~50 bytes of
   margin without wasting much flash per record (a few hundred bytes across an entire
   partition's worth of records). */
#define WD_RECORD_MAX_PAYLOAD 240u

static inline size_t wd_round_up_align(size_t value)
{
    return (value + (WD_RECORD_ALIGN - 1u)) & ~(size_t)(WD_RECORD_ALIGN - 1u);
}

static inline size_t wd_record_on_flash_size(size_t payload_len)
{
    return WD_RECORD_HEADER_SIZE + wd_round_up_align(payload_len);
}

/* CRC32 (poly 0xEDB88320, reflected -- the zlib/IEEE-802.3 variant), table-free
   bit-at-a-time implementation. Wardriving records are small (<= WD_RECORD_MAX_PAYLOAD
   bytes) and this runs at most a few times per second at the fastest validated capture
   cadence, so the cost of a table-free implementation is not worth a 256-entry table's
   .rodata footprint for this workload. */
uint32_t wd_crc32(const uint8_t *data, size_t len);

void wd_sector_header_pack(uint8_t out[WD_SECTOR_HEADER_SIZE], uint32_t generation);
/* Returns true and fills *out_generation if `in` is a well-formed sector header (matching
   magic). Returns false for anything else, including the fully-erased pattern -- callers
   distinguish "erased" from "torn/garbage" with wd_sector_header_is_erased() separately, since
   wardriving_log.c treats the two differently (erased is expected/free; torn implies a
   crash mid-header-write and is defensively re-erased on boot). */
bool wd_sector_header_parse(const uint8_t in[WD_SECTOR_HEADER_SIZE], uint32_t *out_generation);
bool wd_sector_header_is_erased(const uint8_t in[WD_SECTOR_HEADER_SIZE]);

typedef struct {
    uint16_t payload_len;
    bool undrained;
    uint32_t crc32;
} wd_record_header_t;

void wd_record_header_pack(uint8_t out[WD_RECORD_HEADER_SIZE], uint16_t payload_len,
                            bool undrained, uint32_t crc32);
/* Returns true and fills *out if `in`'s magic matches a genuinely-written record header.
   Does NOT validate the payload/crc -- the caller must separately compute wd_crc32() over
   the payload_len bytes that follow and compare to out->crc32. Returns false (leaving *out
   untouched) for the erased pattern (0xFFFFFFFF magic) or for garbage/torn magic bytes
   alike: the caller cannot use this function alone to tell those two apart, and for this
   log's purposes does not need to -- both mean "stop trusting anything past this offset in
   this sector" (see the header's crash-safety note above). Use
   wd_sector_header_is_erased()-style direct magic comparison at the call site if the
   distinction matters (wardriving_log.c's boot scan does, to decide whether to keep this
   sector writable or discard its remainder). */
bool wd_record_header_parse(const uint8_t in[WD_RECORD_HEADER_SIZE], wd_record_header_t *out);
/* True only for the literal all-0xFF erased pattern (never-written flash), as opposed to a
   header that failed to parse for some other (torn/corrupt) reason. */
bool wd_record_header_is_erased(const uint8_t in[WD_RECORD_HEADER_SIZE]);

/* Sector-eviction-order bookkeeping, factored out as pure functions over caller-supplied
   occupied/generation arrays so the ordering logic (not just per-field checksum packing)
   is host-testable without a real partition. `count` is the total number of sectors in the
   partition; occupied[i]/generation[i] describe sector i as classified by
   wd_sector_header_parse()/wd_sector_header_is_erased() during wardriving_log.c's boot
   scan. Returns `count` (an otherwise-impossible sector index) if no sector is occupied. */
size_t wd_find_oldest_generation_index(const bool *occupied, const uint32_t *generation, size_t count);
size_t wd_find_newest_generation_index(const bool *occupied, const uint32_t *generation, size_t count);

#endif /* FEB_WARDRIVING_RECORD_FORMAT_H */

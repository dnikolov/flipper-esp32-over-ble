#include "wardriving_record_format.h"

#include <string.h>

uint32_t wd_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));

            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void put_u32le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t get_u32le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static void put_u16le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t get_u16le(const uint8_t *in)
{
    return (uint16_t)((uint32_t)in[0] | ((uint32_t)in[1] << 8));
}

void wd_sector_header_pack(uint8_t out[WD_SECTOR_HEADER_SIZE], uint32_t generation)
{
    put_u32le(out, WD_SECTOR_MAGIC);
    put_u32le(out + 4, generation);
}

bool wd_sector_header_parse(const uint8_t in[WD_SECTOR_HEADER_SIZE], uint32_t *out_generation)
{
    uint32_t magic = get_u32le(in);

    if (magic != WD_SECTOR_MAGIC) {
        return false;
    }
    if (out_generation != NULL) {
        *out_generation = get_u32le(in + 4);
    }
    return true;
}

bool wd_sector_header_is_erased(const uint8_t in[WD_SECTOR_HEADER_SIZE])
{
    size_t i;

    for (i = 0; i < WD_SECTOR_HEADER_SIZE; i++) {
        if (in[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

void wd_record_header_pack(uint8_t out[WD_RECORD_HEADER_SIZE], uint16_t payload_len,
                            bool undrained, uint32_t crc32)
{
    put_u32le(out, WD_RECORD_MAGIC);
    put_u16le(out + 4, payload_len);
    out[6] = undrained ? 0xFFu : (0xFFu & ~WD_RECORD_FLAG_UNDRAINED);
    out[7] = 0xFFu;
    put_u32le(out + 8, crc32);
}

bool wd_record_header_parse(const uint8_t in[WD_RECORD_HEADER_SIZE], wd_record_header_t *out)
{
    uint32_t magic = get_u32le(in);

    if (magic != WD_RECORD_MAGIC) {
        return false;
    }
    if (out != NULL) {
        out->payload_len = get_u16le(in + 4);
        out->undrained = (in[6] & WD_RECORD_FLAG_UNDRAINED) != 0;
        out->crc32 = get_u32le(in + 8);
    }
    return true;
}

bool wd_record_header_is_erased(const uint8_t in[WD_RECORD_HEADER_SIZE])
{
    size_t i;

    for (i = 0; i < WD_RECORD_HEADER_SIZE; i++) {
        if (in[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

size_t wd_find_oldest_generation_index(const bool *occupied, const uint32_t *generation, size_t count)
{
    size_t best = count;
    size_t i;

    for (i = 0; i < count; i++) {
        if (!occupied[i]) {
            continue;
        }
        if (best == count || generation[i] < generation[best]) {
            best = i;
        }
    }
    return best;
}

size_t wd_find_newest_generation_index(const bool *occupied, const uint32_t *generation, size_t count)
{
    size_t best = count;
    size_t i;

    for (i = 0; i < count; i++) {
        if (!occupied[i]) {
            continue;
        }
        if (best == count || generation[i] > generation[best]) {
            best = i;
        }
    }
    return best;
}

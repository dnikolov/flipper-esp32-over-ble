#pragma once

/* A-GPS SD file reader (docs/AGPS.md "SD file"). Pure logic, header-only, no Furi dependency
   so tests/flipper/test_agps.c compiles the same code: included into the unity build via
   app_internal.h. All functions are `static` so an unused one costs nothing in the FAP.

   The file is untrusted. Every field is validated against the real file size and the 4096 B
   window cap before any byte of it is used; any inconsistency anywhere in the table rejects
   the whole file (AgpsSelBadFormat), not just the window that was going to be used. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AGPS_FILE_MAGIC_0 'F'
#define AGPS_FILE_MAGIC_1 'A'
#define AGPS_FILE_MAGIC_2 'G'
#define AGPS_FILE_MAGIC_3 'P'
#define AGPS_FILE_VERSION 1u
#define AGPS_FILE_HEADER_LEN 16u
#define AGPS_FILE_ENTRY_LEN 16u
#define AGPS_WINDOW_MAX_LEN 4096u
/* 7 days of 4 h windows is 42; allow generous headroom but bound the table walk. */
#define AGPS_WINDOW_COUNT_MAX 256u
#define AGPS_CASIC_SYNC_0 0xBAu
#define AGPS_CASIC_SYNC_1 0xCEu

typedef enum {
    AgpsSelOk = 0,
    AgpsSelBadFormat, /* magic/version/offset/length/ordering violation, or unreadable file */
    AgpsSelExpired, /* every window ended at or before `now`; last_valid_to is set */
    AgpsSelNoWindowNow, /* windows exist but none covers `now` (not yet valid, or a gap) */
} AgpsSelResult;

typedef struct {
    uint32_t valid_from;
    uint32_t valid_to;
    uint32_t offset; /* from file start */
    uint16_t length;
    uint32_t last_valid_to; /* max valid_to over the whole table */
    uint16_t window_count;
} AgpsWindow;

/* Reads exactly `len` bytes at `offset` into `buf`; false on any short read/error. */
typedef bool (*AgpsReadFn)(void* ctx, uint32_t offset, uint8_t* buf, size_t len);

static inline uint16_t agps_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t agps_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Selects the window with valid_from <= now < valid_to (the last such one if windows overlap,
   since it is centred nearest `now`). `file_size` is the real size on disk. */
static AgpsSelResult agps_select_window(
    AgpsReadFn read_fn,
    void* ctx,
    uint32_t file_size,
    uint32_t now,
    AgpsWindow* out) {
    uint8_t header[AGPS_FILE_HEADER_LEN];
    memset(out, 0, sizeof(*out));
    if(file_size < AGPS_FILE_HEADER_LEN || !read_fn(ctx, 0, header, sizeof(header))) {
        return AgpsSelBadFormat;
    }
    if(header[0] != AGPS_FILE_MAGIC_0 || header[1] != AGPS_FILE_MAGIC_1 ||
       header[2] != AGPS_FILE_MAGIC_2 || header[3] != AGPS_FILE_MAGIC_3 ||
       header[4] != AGPS_FILE_VERSION || (header[5] & 0x01u) == 0u ||
       agps_le32(&header[12]) != 0u) {
        return AgpsSelBadFormat;
    }
    uint32_t count = agps_le16(&header[6]);
    if(count == 0u || count > AGPS_WINDOW_COUNT_MAX) {
        return AgpsSelBadFormat;
    }
    uint32_t table_end = AGPS_FILE_HEADER_LEN + count * AGPS_FILE_ENTRY_LEN;
    if(table_end > file_size) {
        return AgpsSelBadFormat;
    }

    bool found = false;
    uint32_t prev_from = 0;
    uint32_t last_to = 0;
    for(uint32_t i = 0; i < count; i++) {
        uint8_t entry[AGPS_FILE_ENTRY_LEN];
        if(!read_fn(ctx, AGPS_FILE_HEADER_LEN + i * AGPS_FILE_ENTRY_LEN, entry, sizeof(entry))) {
            return AgpsSelBadFormat;
        }
        uint32_t from = agps_le32(&entry[0]);
        uint32_t to = agps_le32(&entry[4]);
        uint32_t offset = agps_le32(&entry[8]);
        uint32_t length = agps_le16(&entry[12]);
        if(agps_le16(&entry[14]) != 0u || from >= to || from < prev_from || length == 0u ||
           length > AGPS_WINDOW_MAX_LEN || offset < table_end || offset > file_size ||
           length > file_size - offset) {
            return AgpsSelBadFormat;
        }
        prev_from = from;
        if(to > last_to) {
            last_to = to;
        }
        if(from <= now && now < to) {
            found = true;
            out->valid_from = from;
            out->valid_to = to;
            out->offset = offset;
            out->length = (uint16_t)length;
        }
    }
    out->last_valid_to = last_to;
    out->window_count = (uint16_t)count;
    if(found) {
        uint8_t sync[2];
        if(!read_fn(ctx, out->offset, sync, sizeof(sync)) || sync[0] != AGPS_CASIC_SYNC_0 ||
           sync[1] != AGPS_CASIC_SYNC_1) {
            return AgpsSelBadFormat;
        }
        return AgpsSelOk;
    }
    return now >= last_to ? AgpsSelExpired : AgpsSelNoWindowNow;
}

/* agps_sync_result.txt (written by the host script): first line `OK <window_count>
   <last_valid_to_unix>` or `ERR <message>`. Returns false if the first line is neither.
   `message` is only filled for ERR. */
static bool agps_parse_sync_result(
    const char* buf,
    size_t len,
    bool* ok,
    uint32_t* window_count,
    uint32_t* last_valid_to,
    char* message,
    size_t message_cap) {
    size_t line_len = 0;
    while(line_len < len && buf[line_len] != '\n' && buf[line_len] != '\r') {
        line_len++;
    }
    *ok = false;
    *window_count = 0;
    *last_valid_to = 0;
    if(message_cap > 0) {
        message[0] = '\0';
    }
    if(line_len >= 3 && memcmp(buf, "OK ", 3) == 0) {
        size_t pos = 3;
        uint32_t fields[2] = {0, 0};
        for(size_t field = 0; field < 2; field++) {
            size_t digits = 0;
            uint64_t value = 0;
            while(pos < line_len && buf[pos] >= '0' && buf[pos] <= '9') {
                value = value * 10u + (uint64_t)(buf[pos] - '0');
                if(value > 0xFFFFFFFFu) {
                    return false;
                }
                pos++;
                digits++;
            }
            if(digits == 0) {
                return false;
            }
            fields[field] = (uint32_t)value;
            if(field == 0) {
                if(pos >= line_len || buf[pos] != ' ') {
                    return false;
                }
                pos++;
            }
        }
        *ok = true;
        *window_count = fields[0];
        *last_valid_to = fields[1];
        return true;
    }
    if(line_len >= 4 && memcmp(buf, "ERR ", 4) == 0) {
        size_t copy = line_len - 4;
        if(message_cap > 0) {
            if(copy > message_cap - 1) {
                copy = message_cap - 1;
            }
            memcpy(message, buf + 4, copy);
            message[copy] = '\0';
        }
        return true;
    }
    return false;
}

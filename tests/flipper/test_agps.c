/* Host-native test for flipper/agps_file.h (SD file reader / window selection / sync-result
   parser) and flipper/agps_wire.h (`agps` command encoder / result decoder). Built with MSVC;
   see build_agps.ps1. Same CHECK() pattern as test_session.c. */
#include <stdio.h>
#include <string.h>

#include "cbor_codec.h"
#include "agps_file.h"
#include "agps_wire.h"

static int g_total = 0;
static int g_failed = 0;

#define CHECK(cond, desc)                                   \
    do {                                                    \
        g_total++;                                          \
        if(cond) {                                          \
            printf("PASS: %s\n", desc);                     \
        } else {                                            \
            g_failed++;                                     \
            printf("FAIL: %s (line %d)\n", desc, __LINE__); \
        }                                                   \
    } while(0)

#define FILE_MAX 8192
static uint8_t g_file[FILE_MAX];
static uint32_t g_size;

static bool mem_read(void* ctx, uint32_t offset, uint8_t* buf, size_t len) {
    (void)ctx;
    if(offset > g_size || len > g_size - offset) return false;
    memcpy(buf, g_file + offset, len);
    return true;
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Builds a file with `count` 4 h windows starting at `t0`, each `len` payload bytes beginning
   BA CE. */
static void build(uint16_t count, uint32_t t0, uint16_t len) {
    memset(g_file, 0, sizeof(g_file));
    memcpy(g_file, "FAGP", 4);
    g_file[4] = 1;
    g_file[5] = 1;
    put16(g_file + 6, count);
    put32(g_file + 8, t0);
    uint32_t table_end = 16u + 16u * count;
    for(uint16_t i = 0; i < count; i++) {
        uint8_t* e = g_file + 16 + 16 * i;
        put32(e, t0 + i * 14400u);
        put32(e + 4, t0 + i * 14400u + 14400u);
        put32(e + 8, table_end + i * len);
        put16(e + 12, len);
        g_file[table_end + i * len] = 0xBA;
        g_file[table_end + i * len + 1] = 0xCE;
    }
    g_size = table_end + count * len;
}

#define SEL(now) agps_select_window(mem_read, NULL, g_size, (now), &w)

int main(void) {
    AgpsWindow w;
    const uint32_t t0 = 1000000u;

    build(3, t0, 100);
    CHECK(SEL(t0 + 14400u + 5u) == AgpsSelOk && w.valid_from == t0 + 14400u && w.length == 100 &&
              w.window_count == 3 && w.offset == 16u + 48u + 100u,
          "selects the middle window");
    CHECK(SEL(t0) == AgpsSelOk && w.offset == 64u, "valid_from is inclusive");
    CHECK(SEL(t0 + 14400u) == AgpsSelOk && w.valid_from == t0 + 14400u,
          "valid_to is exclusive (boundary goes to next window)");
    CHECK(SEL(t0 + 3u * 14400u) == AgpsSelExpired && w.last_valid_to == t0 + 3u * 14400u,
          "expired exactly at last valid_to, reports it");
    CHECK(SEL(t0 - 1u) == AgpsSelNoWindowNow, "before first window is not expired");

    build(2, t0, 50);
    put32(g_file + 16 + 16 + 0, t0 + 20000u);
    put32(g_file + 16 + 16 + 4, t0 + 30000u);
    CHECK(SEL(t0 + 15000u) == AgpsSelNoWindowNow, "gap between windows reports no window now");

    build(2, t0, 50);
    put32(g_file + 16 + 16 + 0, t0 + 100u);
    CHECK(SEL(t0 + 200u) == AgpsSelOk && w.valid_from == t0 + 100u,
          "overlapping windows pick the later one");

    build(2, t0, 50);
    g_file[0] = 'X';
    CHECK(SEL(t0) == AgpsSelBadFormat, "bad magic");
    build(2, t0, 50);
    g_file[4] = 2;
    CHECK(SEL(t0) == AgpsSelBadFormat, "bad version");
    build(2, t0, 50);
    g_file[5] = 0;
    CHECK(SEL(t0) == AgpsSelBadFormat, "no GPS constellation bit");
    build(2, t0, 50);
    g_file[12] = 1;
    CHECK(SEL(t0) == AgpsSelBadFormat, "header reserved nonzero");
    build(2, t0, 50);
    put16(g_file + 6, 0);
    CHECK(SEL(t0) == AgpsSelBadFormat, "zero windows");
    build(2, t0, 50);
    put16(g_file + 6, 300);
    CHECK(SEL(t0) == AgpsSelBadFormat, "window_count over cap");
    build(2, t0, 50);
    put16(g_file + 6, 200);
    CHECK(SEL(t0) == AgpsSelBadFormat, "table larger than file");
    build(2, t0, 50);
    put16(g_file + 16 + 16 + 12, 51);
    CHECK(SEL(t0) == AgpsSelBadFormat, "last window runs past EOF");
    build(2, t0, 50);
    put16(g_file + 16 + 12, 4097);
    g_size = 16u + 32u + 8000u;
    CHECK(SEL(t0) == AgpsSelBadFormat, "window length over 4096");
    build(2, t0, 50);
    put16(g_file + 16 + 12, 0);
    CHECK(SEL(t0) == AgpsSelBadFormat, "window length zero");
    build(2, t0, 50);
    put32(g_file + 16 + 8, 20);
    CHECK(SEL(t0) == AgpsSelBadFormat, "window offset inside the table");
    build(2, t0, 50);
    put32(g_file + 16 + 8, 0xFFFFFFF0u);
    CHECK(SEL(t0) == AgpsSelBadFormat, "window offset huge (no overflow)");
    build(2, t0, 50);
    put32(g_file + 16 + 4, t0);
    CHECK(SEL(t0) == AgpsSelBadFormat, "valid_to <= valid_from");
    build(2, t0, 50);
    put32(g_file + 16 + 0, t0 + 999999u);
    put32(g_file + 16 + 4, t0 + 9999999u);
    CHECK(SEL(t0) == AgpsSelBadFormat, "table not sorted by valid_from");
    build(2, t0, 50);
    put16(g_file + 16 + 14, 1);
    CHECK(SEL(t0) == AgpsSelBadFormat, "entry reserved nonzero");
    build(2, t0, 50);
    g_file[16 + 32] = 0;
    CHECK(SEL(t0) == AgpsSelBadFormat, "selected window does not start with BA CE");
    build(2, t0, 50);
    CHECK(agps_select_window(mem_read, NULL, 10u, t0, &w) == AgpsSelBadFormat,
          "file smaller than header");
    build(1, t0, 4096);
    CHECK(SEL(t0) == AgpsSelOk && w.length == 4096, "max-length window accepted");

    bool ok;
    uint32_t windows, valid_to;
    char msg[24];
    const char* ok_text = "OK 42 1790000000\r\n";
    CHECK(agps_parse_sync_result(
              ok_text, strlen(ok_text), &ok, &windows, &valid_to, msg, sizeof(msg)) &&
              ok && windows == 42 && valid_to == 1790000000u,
          "parse OK line");
    const char* err_text = "ERR generator hash mismatch and a long tail\n";
    CHECK(agps_parse_sync_result(
              err_text, strlen(err_text), &ok, &windows, &valid_to, msg, sizeof(msg)) &&
              !ok && strcmp(msg, "generator hash mismatch") == 0,
          "parse ERR line, message truncated to cap");
    const char* bad1 = "OK 42\n";
    CHECK(!agps_parse_sync_result(bad1, strlen(bad1), &ok, &windows, &valid_to, msg, sizeof(msg)),
          "OK with missing field rejected");
    const char* bad2 = "OK 42 99999999999\n";
    CHECK(!agps_parse_sync_result(bad2, strlen(bad2), &ok, &windows, &valid_to, msg, sizeof(msg)),
          "OK with u32 overflow rejected");
    const char* bad3 = "garbage\n";
    CHECK(!agps_parse_sync_result(bad3, strlen(bad3), &ok, &windows, &valid_to, msg, sizeof(msg)),
          "unrecognized first line rejected");

    uint8_t chunk[AGPS_WIRE_CHUNK_MAX];
    memset(chunk, 0xA5, sizeof(chunk));
    uint8_t args[AGPS_WIRE_CHUNK_MAX + 48];
    size_t n = agps_wire_encode_chunk_arguments(
        args, sizeof(args), 1790000000u, 2700, 128, chunk, sizeof(chunk));
    CHECK(n != 0 && n <= sizeof(args) && args[0] == 0xA5, "chunk arguments encode (5-entry map)");
    CHECK(n + 39u <= 224u, "command payload with a full chunk fits FEB_CMD_PAYLOAD_MAX_LEN (224)");
    feb_cbor_status_t st = FEB_CBOR_OK;
    size_t count = 0;
    CHECK(feb_cbor_decode_map_header(args, n, &count, &st) == 1 && count == 5,
          "arguments decode as a map header with 5 entries");
    CHECK(args[1] == 0x66 && memcmp(args + 2, "action", 6) == 0 && args[8] == 0x64 &&
              memcmp(args + 9, "load", 4) == 0,
          "first field is action=\"load\"");
    CHECK(agps_wire_encode_chunk_arguments(args, 10, 1, 1, 0, chunk, sizeof(chunk)) == 0,
          "undersized output buffer rejected");

    uint8_t res[64];
    size_t rp = 0;
    rp += feb_cbor_encode_map_header(res + rp, sizeof(res) - rp, 5);
    const char* keys[5] = {"sent", "acked", "nacked", "timeout", "extra"};
    uint64_t vals[5] = {34, 31, 3, 0, 7};
    for(int i = 0; i < 5; i++) {
        rp += feb_cbor_encode_text(res + rp, sizeof(res) - rp, keys[i], strlen(keys[i]));
        rp += feb_cbor_encode_uint(res + rp, sizeof(res) - rp, vals[i]);
    }
    AgpsWireResult wr;
    CHECK(agps_wire_decode_result(res, rp, &wr) && wr.sent == 34 && wr.acked == 31 &&
              wr.nacked == 3 && wr.timeout == 0,
          "result decode (unknown key skipped)");
    CHECK(!agps_wire_decode_result(res, rp - 1, &wr), "truncated result rejected");
    uint8_t res2[32];
    size_t r2 = feb_cbor_encode_map_header(res2, sizeof(res2), 1);
    r2 += feb_cbor_encode_text(res2 + r2, sizeof(res2) - r2, "sent", 4);
    r2 += feb_cbor_encode_uint(res2 + r2, sizeof(res2) - r2, 5);
    CHECK(!agps_wire_decode_result(res2, r2, &wr), "result with missing counters rejected");

    printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}

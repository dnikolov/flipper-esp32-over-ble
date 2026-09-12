/* Host-native test driver for esp32/main/wardriving_record_format.c -- the pure,
   zero-ESP-IDF-dependency checksum/header-packing/eviction-ordering helpers underneath
   wardriving_log.c's raw-flash circular log (docs/PLAN.md step 8's wardriving-log
   persistence). wardriving_log.c itself is not exercised here since it depends directly on
   esp_partition.h and is only ever built as part of the real ESP32 firmware -- see that
   file's header comment. This test covers exactly what docs/PLAN.md/the wardriving task
   asked for host-native coverage of: checksum validation and sector-eviction bookkeeping. */
#include <stdio.h>
#include <string.h>

#include "wardriving_record_format.h"
#include "wardriving_validate.h"

static int g_failures = 0;

static void check(int condition, const char *name)
{
    if (condition) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        g_failures++;
    }
}

static void test_crc32_known_vector(void)
{
    /* Standard CRC-32/ISO-HDLC ("zlib") check value for the ASCII string "123456789". */
    static const uint8_t input[] = "123456789";
    uint32_t crc = wd_crc32(input, 9);

    check(crc == 0xCBF43926u, "crc32: standard check value for \"123456789\"");
    check(wd_crc32(NULL, 0) == 0x00000000u, "crc32: empty input is 0");
}

static void test_round_up_align(void)
{
    check(wd_round_up_align(0) == 0, "round_up_align(0) == 0");
    check(wd_round_up_align(1) == 4, "round_up_align(1) == 4");
    check(wd_round_up_align(4) == 4, "round_up_align(4) == 4");
    check(wd_round_up_align(5) == 8, "round_up_align(5) == 8");
    check(wd_record_on_flash_size(0) == WD_RECORD_HEADER_SIZE,
          "record_on_flash_size(0) == header size");
    check(wd_record_on_flash_size(1) == WD_RECORD_HEADER_SIZE + 4u,
          "record_on_flash_size(1) pads payload to 4 bytes");
    check(wd_record_on_flash_size(4) == WD_RECORD_HEADER_SIZE + 4u,
          "record_on_flash_size(4) needs no padding");
}

static void test_sector_header_round_trip(void)
{
    uint8_t buf[WD_SECTOR_HEADER_SIZE];
    uint8_t erased[WD_SECTOR_HEADER_SIZE];
    uint32_t gen = 0;

    memset(erased, 0xFF, sizeof(erased));
    check(wd_sector_header_is_erased(erased), "sector header: all-0xFF reads as erased");
    check(!wd_sector_header_parse(erased, &gen), "sector header: erased pattern does not parse as valid");

    wd_sector_header_pack(buf, 0x1234u);
    check(!wd_sector_header_is_erased(buf), "sector header: a packed header is not the erased pattern");
    check(wd_sector_header_parse(buf, &gen), "sector header: a packed header parses");
    check(gen == 0x1234u, "sector header: generation round-trips");

    memset(buf, 0x00, sizeof(buf));
    check(!wd_sector_header_is_erased(buf), "sector header: all-zero is not the erased pattern");
    check(!wd_sector_header_parse(buf, &gen), "sector header: all-zero (garbage) does not parse as valid");
}

static void test_record_header_round_trip(void)
{
    uint8_t buf[WD_RECORD_HEADER_SIZE];
    uint8_t erased[WD_RECORD_HEADER_SIZE];
    wd_record_header_t parsed;

    memset(erased, 0xFF, sizeof(erased));
    check(wd_record_header_is_erased(erased), "record header: all-0xFF reads as erased");
    check(!wd_record_header_parse(erased, &parsed), "record header: erased pattern does not parse as valid");

    wd_record_header_pack(buf, 123u, true, 0xDEADBEEFu);
    check(!wd_record_header_is_erased(buf), "record header: a packed header is not the erased pattern");
    check(wd_record_header_parse(buf, &parsed), "record header: a packed (undrained) header parses");
    check(parsed.payload_len == 123u, "record header: payload_len round-trips");
    check(parsed.undrained == true, "record header: undrained=true round-trips");
    check(parsed.crc32 == 0xDEADBEEFu, "record header: crc32 round-trips");

    /* Marking drained is a single-byte partial-program write of the flags byte (offset 6)
       in wardriving_log.c -- simulate that exact operation here and confirm parsing still
       recovers everything else unchanged, matching the "clearing one bit never disturbs the
       rest of the record" crash-safety claim in wardriving_record_format.h. */
    {
        uint8_t undrained_bit = (uint8_t)WD_RECORD_FLAG_UNDRAINED;

        buf[6] = (uint8_t)(buf[6] & ~undrained_bit);
    }
    check(wd_record_header_parse(buf, &parsed), "record header: still parses after clearing the undrained bit");
    check(parsed.undrained == false, "record header: undrained=false after clearing the flag bit");
    check(parsed.payload_len == 123u, "record header: payload_len unaffected by the flag-only rewrite");
    check(parsed.crc32 == 0xDEADBEEFu, "record header: crc32 unaffected by the flag-only rewrite");

    memset(buf, 0x00, sizeof(buf));
    check(!wd_record_header_is_erased(buf), "record header: all-zero is not the erased pattern");
    check(!wd_record_header_parse(buf, &parsed), "record header: all-zero (garbage/torn) does not parse as valid");
}

static void test_crc_detects_corruption(void)
{
    static const uint8_t payload[] = "wardriving-record-payload";
    uint32_t good_crc = wd_crc32(payload, sizeof(payload) - 1u);
    uint8_t corrupted[sizeof(payload)];

    memcpy(corrupted, payload, sizeof(payload));
    corrupted[3] ^= 0x01u; /* flip one bit, simulating a torn/partial flash write */
    check(wd_crc32(corrupted, sizeof(corrupted) - 1u) != good_crc,
          "crc32: a single flipped bit changes the checksum (torn-write detection)");
}

static void test_eviction_ordering(void)
{
    /* 5 sectors: 1 and 3 unoccupied (free), 0/2/4 occupied with generations out of physical
       order to confirm the ordering helpers key off generation, not array index. */
    bool occupied[5] = {true, false, true, false, true};
    uint32_t generation[5] = {30u, 0u, 10u, 0u, 20u};

    check(wd_find_oldest_generation_index(occupied, generation, 5) == 2,
          "eviction ordering: oldest occupied sector is index 2 (generation 10)");
    check(wd_find_newest_generation_index(occupied, generation, 5) == 0,
          "eviction ordering: newest occupied sector is index 0 (generation 30)");

    {
        bool none_occupied[3] = {false, false, false};
        uint32_t gens[3] = {0, 0, 0};

        check(wd_find_oldest_generation_index(none_occupied, gens, 3) == 3,
              "eviction ordering: no occupied sectors -> oldest returns count (3)");
        check(wd_find_newest_generation_index(none_occupied, gens, 3) == 3,
              "eviction ordering: no occupied sectors -> newest returns count (3)");
    }

    {
        /* A single occupied sector is trivially both oldest and newest. */
        bool one_occupied[4] = {false, false, true, false};
        uint32_t gens[4] = {0, 0, 7u, 0};

        check(wd_find_oldest_generation_index(one_occupied, gens, 4) == 2,
              "eviction ordering: single occupied sector is its own oldest");
        check(wd_find_newest_generation_index(one_occupied, gens, 4) == 2,
              "eviction ordering: single occupied sector is its own newest");
    }
}

/* docs/PROTOCOL.md "Interval bounds and defaults" vs. its field table's terser "required
   when X in sources" wording -- resolved 2026-09-09 in favor of the more specific "Interval
   bounds and defaults" statement: a requested source's interval field(s) may be omitted, in
   which case a default is substituted, since that's exactly what the v1 Flipper client (no
   interval-entry UI) always sends. wifi_interval_ms/ble_window_ms still default to their
   original point-4 (most-aggressive) values; ble_interval_ms was raised from point-4's 30ms
   to 500ms on 2026-09-10 (see wardriving_validate.h's FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS
   comment) -- this test only checks each default against its named constant, so it does not
   hardcode either value and needs no change from that fix. Covers the primary
   cross-firmware-compatibility case this fix targets, plus the surrounding validation rules
   that must still hold: bounds-checking on an explicit value, and rejecting an interval
   field present for a source that was never requested. */
static void test_start_interval_resolution(void)
{
    wardriving_start_request_t req;
    wardriving_resolved_intervals_t resolved;

    /* The exact shape a v1 Flipper client sends: both sources requested, no interval fields
       at all -- both must default to their named default constants. */
    memset(&req, 0, sizeof(req));
    req.want_wifi = true;
    req.want_ble = true;
    check(wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: both sources requested, all interval fields absent -> accepted");
    check(resolved.wifi_interval_ms == 30000u,
          "start intervals: wifi_interval_ms defaults to a conservative 30s cadence when absent");
    check(resolved.ble_window_ms == FEB_WARDRIVING_BLE_WINDOW_DEFAULT_MS &&
          resolved.ble_interval_ms == FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS,
          "start intervals: ble_window_ms/ble_interval_ms default to their default values when absent");

    /* wifi only, interval absent -> defaulted; ble not requested and absent, untouched. */
    memset(&req, 0, sizeof(req));
    req.want_wifi = true;
    check(wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: wifi-only requested, wifi_interval_ms absent -> accepted");
    check(resolved.wifi_interval_ms == FEB_WARDRIVING_WIFI_INTERVAL_DEFAULT_MS,
          "start intervals: wifi-only default substitution");

    /* ble only, params absent -> defaulted. */
    memset(&req, 0, sizeof(req));
    req.want_ble = true;
    check(wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: ble-only requested, ble params absent -> accepted");
    check(resolved.ble_window_ms == FEB_WARDRIVING_BLE_WINDOW_DEFAULT_MS &&
          resolved.ble_interval_ms == FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS,
          "start intervals: ble-only default substitution");

    /* An explicit, in-bounds value is used as-is, not overridden by the default. */
    memset(&req, 0, sizeof(req));
    req.want_wifi = true;
    req.has_wifi_interval_ms = 1;
    req.wifi_interval_ms = 15000u;
    check(wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: explicit in-bounds wifi_interval_ms -> accepted");
    check(resolved.wifi_interval_ms == 15000u,
          "start intervals: explicit wifi_interval_ms value is used, not the default");

    /* An explicit, in-bounds ble window/interval pair is used as-is. */
    memset(&req, 0, sizeof(req));
    req.want_ble = true;
    req.has_ble_params = 1;
    req.ble_window_ms = 100u;
    req.ble_interval_ms = 1000u;
    check(wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: explicit in-bounds ble window/interval (point-1) -> accepted");
    check(resolved.ble_window_ms == 100u && resolved.ble_interval_ms == 1000u,
          "start intervals: explicit ble window/interval values are used, not the defaults");

    /* Explicit, out-of-bounds wifi_interval_ms is rejected. */
    memset(&req, 0, sizeof(req));
    req.want_wifi = true;
    req.has_wifi_interval_ms = 1;
    req.wifi_interval_ms = FEB_WARDRIVING_WIFI_INTERVAL_MAX_MS + 1u;
    check(!wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: wifi_interval_ms above the max bound -> rejected");

    /* Explicit ble_window_ms below the min bound is rejected. */
    memset(&req, 0, sizeof(req));
    req.want_ble = true;
    req.has_ble_params = 1;
    req.ble_window_ms = FEB_WARDRIVING_BLE_WINDOW_MIN_MS - 1u;
    req.ble_interval_ms = FEB_WARDRIVING_BLE_INTERVAL_MIN_MS;
    check(!wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: ble_window_ms below the min bound -> rejected");

    /* Explicit ble_interval_ms above the max bound is rejected. */
    memset(&req, 0, sizeof(req));
    req.want_ble = true;
    req.has_ble_params = 1;
    req.ble_window_ms = FEB_WARDRIVING_BLE_WINDOW_MAX_MS;
    req.ble_interval_ms = FEB_WARDRIVING_BLE_INTERVAL_MAX_MS + 1u;
    check(!wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: ble_interval_ms above the max bound -> rejected");

    /* A window longer than its own interval is structurally rejected even within bounds. */
    memset(&req, 0, sizeof(req));
    req.want_ble = true;
    req.has_ble_params = 1;
    req.ble_window_ms = 100u;
    req.ble_interval_ms = 50u;
    check(!wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: ble_window_ms > ble_interval_ms -> rejected even though both are in-bounds individually");

    /* An interval field present for a source that was never requested is rejected --
       unaffected by the required-vs-default fix (this part of the rule was never ambiguous). */
    memset(&req, 0, sizeof(req));
    req.want_wifi = false;
    req.has_wifi_interval_ms = 1;
    req.wifi_interval_ms = 5000u;
    check(!wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: wifi_interval_ms present but wifi not requested -> rejected");

    memset(&req, 0, sizeof(req));
    req.want_ble = false;
    req.has_ble_params = 1;
    req.ble_window_ms = 30u;
    req.ble_interval_ms = 30u;
    check(!wardriving_resolve_start_intervals(&req, &resolved),
          "start intervals: ble params present but ble not requested -> rejected");
}

int main(void)
{
    test_crc32_known_vector();
    test_round_up_align();
    test_sector_header_round_trip();
    test_record_header_round_trip();
    test_crc_detects_corruption();
    test_eviction_ordering();
    test_start_interval_resolution();

    if (g_failures == 0) {
        printf("\nAll wardriving_record_format tests passed.\n");
    } else {
        printf("\n%d wardriving_record_format test(s) FAILED.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

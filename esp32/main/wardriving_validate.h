/* Pure, zero-ESP-IDF-dependency slice of handle_wardriving_command()'s (main.c) `start`
   validation: resolving each requested source's interval field(s) to either the caller-
   supplied explicit value (bounds-checked) or the point-4 default when omitted. Factored
   out into its own header/translation unit -- with no dependency on cbor_codec.h/NimBLE/
   ESP-IDF -- so tests/esp32/test_wardriving_log.c can exercise it on the host. Not part of
   the shared ESP32/Flipper protocol contract (this is ESP32-side validation policy, not a
   wire shape) and not in tools/check_shared_headers.py's HEADER_PAIRS.

   See main.c's handle_wardriving_command() top comment for the full interpretation note
   this function implements: docs/PROTOCOL.md's field table says wifi_interval_ms/
   ble_window_ms/ble_interval_ms are "required" whenever their source is requested, but the
   "Interval bounds and defaults" section is more specific ("The default when a `start`
   omits these fields is the maximum/point-4 values") -- resolved 2026-09-09 in favor of the
   more specific statement, since the v1 Flipper client (no interval-entry UI) always omits
   these fields entirely. This function implements exactly that: presence for a requested
   source is validated against bounds; *absence* for a requested source substitutes the
   default; presence for a source that was not requested is still rejected (unchanged from
   the field table's unambiguous part of the rule) -- but that check involves `sources`
   membership, which is decoded/validated by cbor_wardriving.h + main.c's own
   wardriving_source_requested()/duplicate-source checks, not this function; this function
   only resolves the two want_wifi/want_ble booleans it's handed. */
#ifndef FEB_WARDRIVING_VALIDATE_H
#define FEB_WARDRIVING_VALIDATE_H

#include <stdbool.h>
#include <stdint.h>

/* Bounds are step 4's validated points (docs/PLAN.md step 4 results table): minimum (most
   conservative) is point 1 (`ble_window_ms=100, ble_interval_ms=1000, wifi_interval_ms=30000`);
   maximum (most aggressive) is point 4 (`ble_window_ms=30, ble_interval_ms=30`, continuous
   back-to-back Wi-Fi scanning). "Continuous" has no literal wifi_interval_ms figure in step
   4's table, so 0 (immediately re-trigger the next scan, no gap) is the concrete stand-in
   used here and is also this function's wifi default. */
#define FEB_WARDRIVING_WIFI_INTERVAL_MIN_MS 0u
#define FEB_WARDRIVING_WIFI_INTERVAL_MAX_MS 30000u
#define FEB_WARDRIVING_BLE_WINDOW_MIN_MS 30u
#define FEB_WARDRIVING_BLE_WINDOW_MAX_MS 100u
#define FEB_WARDRIVING_BLE_INTERVAL_MIN_MS 30u
#define FEB_WARDRIVING_BLE_INTERVAL_MAX_MS 1000u
/* Defaults substituted when a requested source's interval field(s) are absent. Named
   separately from the MIN_MS constants so a future bound change can't silently change the
   default (or vice versa) without an explicit edit to both.

   ble_interval_ms's default was point-4's 30ms (100% BLE observer duty) through
   2026-09-09; raised to 500ms (~6% duty, window unchanged) on 2026-09-10 after real
   wardriving traffic on real hardware showed 100% duty starves the active BLE connection
   itself -- the continuous scan-restart cycle left no serviceable airtime for GATT
   traffic, so a `stop` command could never land and the link was torn down locally every
   ~30-40s (see docs/PROJECT_HISTORY.md's "wardriving BLE duty-cycle starvation" entry).
   wifi_interval_ms's default is still point-4's 0 (continuous, no gap) -- WiFi scanning
   was active in that same reproduction and is suspected to independently compete for the
   same shared 2.4GHz radio via IDF's coexistence arbiter, but this has not yet been
   isolated/validated the way step 4 validated the original points; flagged in
   docs/PLAN.md, not yet fixed. */
#define FEB_WARDRIVING_WIFI_INTERVAL_DEFAULT_MS 0u
#define FEB_WARDRIVING_BLE_WINDOW_DEFAULT_MS 30u
#define FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS 500u

typedef struct {
    bool want_wifi;
    bool want_ble;
    int has_wifi_interval_ms; /* mirrors feb_wardriving_command_payload_t's has_* flags --
                                  int, not bool, to match that struct's field type exactly */
    uint64_t wifi_interval_ms;
    int has_ble_params;
    uint64_t ble_window_ms;
    uint64_t ble_interval_ms;
} wardriving_start_request_t;

typedef struct {
    uint32_t wifi_interval_ms;
    uint32_t ble_window_ms;
    uint32_t ble_interval_ms;
} wardriving_resolved_intervals_t;

/* Returns true and fills *out with the final (explicit-and-valid, or defaulted) interval
   values when the request is acceptable. Returns false (leaving *out untouched) when it is
   not -- an interval field present for a source that wasn't requested, or an explicit value
   outside the bounds above; the caller maps false to `invalid_command`. Does not know about
   `sources`/`action`/busy-guard state at all -- purely this one slice of
   handle_wardriving_command()'s validation. */
bool wardriving_resolve_start_intervals(const wardriving_start_request_t *req,
                                        wardriving_resolved_intervals_t *out);

#endif /* FEB_WARDRIVING_VALIDATE_H */

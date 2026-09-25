#include "wardriving_validate.h"

#include <stddef.h>

bool wardriving_resolve_start_intervals(const wardriving_start_request_t *req,
                                        wardriving_resolved_intervals_t *out)
{
    uint32_t wifi_interval_ms = FEB_WARDRIVING_WIFI_INTERVAL_DEFAULT_MS;
    uint32_t ble_window_ms = FEB_WARDRIVING_BLE_WINDOW_DEFAULT_MS;
    uint32_t ble_interval_ms = FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS;

    if (req == NULL || out == NULL) {
        return false;
    }

    /* An interval field present for a source that wasn't requested is structurally
       nonsensical regardless of the required-vs-default question -- rejected unconditionally. */
    if ((!req->want_wifi && req->has_wifi_interval_ms) || (!req->want_ble && req->has_ble_params)) {
        return false;
    }

    if (req->want_wifi && req->has_wifi_interval_ms) {
        if (req->wifi_interval_ms > FEB_WARDRIVING_WIFI_INTERVAL_MAX_MS) {
            return false;
        }
        wifi_interval_ms = (uint32_t)req->wifi_interval_ms;
    }
    /* req->want_wifi && !req->has_wifi_interval_ms: leave the default in place. */

    if (req->want_ble && req->has_ble_params) {
        if (req->ble_window_ms < FEB_WARDRIVING_BLE_WINDOW_MIN_MS ||
            req->ble_window_ms > FEB_WARDRIVING_BLE_WINDOW_MAX_MS ||
            req->ble_interval_ms < FEB_WARDRIVING_BLE_INTERVAL_MIN_MS ||
            req->ble_interval_ms > FEB_WARDRIVING_BLE_INTERVAL_MAX_MS ||
            req->ble_window_ms > req->ble_interval_ms) {
            /* window > interval is a structural sanity check beyond docs/PROTOCOL.md's
               literal text (a scan window can't outlast the period between window starts)
               -- see wardriving_validate.h's top comment. Only checked when the fields are
               actually present; the substituted default (30/30) trivially satisfies it. */
            return false;
        }
        ble_window_ms = (uint32_t)req->ble_window_ms;
        ble_interval_ms = (uint32_t)req->ble_interval_ms;
    }
    /* req->want_ble && !req->has_ble_params: leave the defaults in place. */

    out->wifi_interval_ms = wifi_interval_ms;
    out->ble_window_ms = ble_window_ms;
    out->ble_interval_ms = ble_interval_ms;
    return true;
}

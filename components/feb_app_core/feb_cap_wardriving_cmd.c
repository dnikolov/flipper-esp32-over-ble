#include "feb_app_internal.h"

static bool wardriving_source_requested(const feb_wardriving_command_payload_t *payload, const char *name)
{
    size_t len = strlen(name);
    size_t i;

    for (i = 0; i < payload->source_count; i++) {
        if (payload->source_lens[i] == len && memcmp(payload->sources[i], name, len) == 0) {
            return true;
        }
    }
    return false;
}

/* docs/PROTOCOL.md "`wardriving` command and status payloads" / docs/CAPABILITIES.md's
   `wardriving` bullet. Field-presence/bounds validation this codec's decoder deliberately
   leaves to the caller (see cbor_wardriving.h's top-of-file comment) is all done here:
   action-dependent presence of `sources`/`wifi_interval_ms`/`ble_window_ms`+`ble_interval_ms`,
    source values restricted to "wifi"/"ble"/"ble_passive" with no duplicates, board-capability gating
   against feb_features[], and the interval bounds from docs/PLAN.md step 4.

   Interpretation note on "required" vs "default when omitted" (resolved 2026-09-09,
   supersedes an earlier pass's stricter reading -- see docs/SESSION_MEMORY.md):
   docs/PROTOCOL.md's field table says wifi_interval_ms/ble_window_ms/ble_interval_ms are
   each "required" on the wire whenever their source is requested, but the "Interval bounds
   and defaults" section is more specific and explicit: "The default when a `start` omits
   these fields is the maximum/point-4 values." Since this is exactly what a v1 Flipper
   client sends -- its wardriving control screen has no interval-entry UI at all
   (docs/PLAN.md), so it always requests a source via `sources` while leaving that source's
   interval field(s) out of the CBOR map entirely -- this function treats "required" as
   "applicable" rather than "must be present on the wire": a requested source's interval
   field(s) may be omitted, in which case the point-4 default is substituted; an interval
   field that IS present is still validated against the documented bounds, and an interval
   field present for a NOT-requested source is still invalid_command (that part of the field
   table's presence rule is unambiguous and unchanged). This resolution is implemented by
   wardriving_resolve_start_intervals() (wardriving_validate.h/.c, host-tested in
   tests/esp32/test_wardriving_log.c) rather than inline here, so it's exercised without
   needing a full command-dispatch harness. `sources` itself has no default/omission concept
   -- `action = "start"` with `sources` absent or empty is always invalid_command. */
void feb_handle_wardriving_command(uint16_t conn_handle, const feb_command_payload_t *cmd)
{
    feb_wardriving_command_payload_t payload;
    feb_cbor_status_t status;
    bool is_start;
    bool is_stop;
    bool is_status_query;
    bool want_wifi = false;
    bool want_ble = false;
    bool want_ble_passive = false;
    wardriving_swelling_mode_t wifi_swelling = WARDRIVING_SWELLING_NORMAL;
    wardriving_country_t country = WARDRIVING_COUNTRY_ROW;
#if FEB_WIFI_DUAL_BAND
    /* Wire values "2.4ghz"/"5ghz_fast"/"5ghz_full" map to 0/1/2 -- board_config.h-gated boards
       only (docs/PROTOCOL.md's `wifi_band` row); single-band boards decode-and-ignore this
       field entirely (feb_cbor_decode_wardriving_command_payload() always populates
       has_wifi_band/wifi_band regardless of board). */
    uint8_t wifi_band = 0;
#endif
    size_t i;

    status = feb_cbor_decode_wardriving_command_payload(cmd->arguments_span, cmd->arguments_span_len, &payload);
    if (status != FEB_CBOR_OK) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    is_start = (payload.action_len == strlen("start") && memcmp(payload.action, "start", payload.action_len) == 0);
    is_stop = (payload.action_len == strlen("stop") && memcmp(payload.action, "stop", payload.action_len) == 0);
    is_status_query =
        (payload.action_len == strlen("status") && memcmp(payload.action, "status", payload.action_len) == 0);
    if (!is_start && !is_stop && !is_status_query) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (is_status_query) {
        if (payload.has_sources || payload.has_wifi_interval_ms || payload.has_ble_params ||
            payload.has_wifi_swelling || payload.has_country
#if FEB_WIFI_DUAL_BAND
            || payload.has_wifi_band
#endif
            ) {
            if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        feb_status_payload_t status_payload = {0};
        size_t payload_len;

        status_payload.request_id = cmd->request_id;
        status_payload.state = (feb_wardriving_wifi_active || feb_wardriving_ble_active) ? "started" : "stopped";
        status_payload.state_len = strlen(status_payload.state);
        payload_len = feb_cbor_encode_status_payload(
            feb_pairing_payload_encode_buf, sizeof(feb_pairing_payload_encode_buf), &status_payload);
        if (payload_len == 0 ||
            !feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        ESP_LOGI(TAG, "wardriving status query answered (request_id=%llu, running=%d)",
                 (unsigned long long)cmd->request_id,
                 (int)(feb_wardriving_wifi_active || feb_wardriving_ble_active));
        return;
    }

    if (is_stop) {
        if (payload.has_sources || payload.has_wifi_interval_ms || payload.has_ble_params ||
            payload.has_wifi_swelling || payload.has_country
#if FEB_WIFI_DUAL_BAND
            || payload.has_wifi_band
#endif
            ) {
            if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
        if (!feb_wardriving_wifi_active && !feb_wardriving_ble_active) {
            if (!feb_send_protected_error(conn_handle, "not_running", strlen("not_running"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        feb_wardriving_stop_internal();
        feb_wardriving_persisted.enabled = false;
        wardriving_persist_save(&feb_wardriving_persisted);

        {
            feb_status_payload_t status_payload = {0};
            size_t payload_len;

            status_payload.request_id = cmd->request_id;
            status_payload.state = "stopped";
            status_payload.state_len = strlen("stopped");
            payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                         sizeof(feb_pairing_payload_encode_buf), &status_payload);
            if (payload_len == 0 ||
                !feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return;
            }
        }
        ESP_LOGI(TAG, "wardriving stopped (request_id=%llu)", (unsigned long long)cmd->request_id);
        return;
    }

    /* is_start */
    if (feb_wardriving_wifi_active || feb_wardriving_ble_active) {
        if (!feb_send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    if (!payload.has_sources || payload.source_count == 0) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    want_wifi = wardriving_source_requested(&payload, "wifi");
    want_ble = wardriving_source_requested(&payload, "ble");
    want_ble_passive = wardriving_source_requested(&payload, "ble_passive");
    {
        size_t recognized_count = (want_wifi ? 1u : 0u) + (want_ble ? 1u : 0u) +
                                  (want_ble_passive ? 1u : 0u);

        /* Catches both an unrecognized source string and duplicate entries
           in one comparison: source_count can only equal recognized_count if every entry is
           exactly one of "wifi"/"ble"/"ble_passive" and neither appears twice. Neither rule is an explicit
           docs/PROTOCOL.md sentence for the duplicate case -- a judgment call, since a
           repeated source is structurally nonsensical the same way an unrecognized one is. */
        if (payload.source_count != recognized_count || (want_ble && want_ble_passive)) {
            if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
    }

    {
        bool have_wifi_scan = false;
        bool have_ble_scan = false;

        for (i = 0; i < FEB_FEATURE_COUNT; i++) {
            if (strcmp(feb_features[i], "wifi_scan") == 0) have_wifi_scan = true;
            if (strcmp(feb_features[i], "ble_scan") == 0) have_ble_scan = true;
        }
        if ((want_wifi && !have_wifi_scan) || ((want_ble || want_ble_passive) && !have_ble_scan)) {
            if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
    }

    /* docs/WARDRIVING_REDESIGN.md (2026-09-21): wifi_swelling/country are required exactly
       when "wifi" is in sources, absent otherwise -- unlike wifi_interval_ms (which has a
       defined default substituted on omission, per wardriving_validate.h), these two have
       no defined default, so a missing value while want_wifi is a real invalid_command, not
       an omission to resolve. */
    if (want_wifi) {
        bool wifi_swelling_ok = false;
        bool country_ok = false;
#if FEB_WIFI_DUAL_BAND
        bool wifi_band_ok = false;
#endif

        if (!payload.has_wifi_swelling || !payload.has_country
#if FEB_WIFI_DUAL_BAND
            || !payload.has_wifi_band
#endif
            ) {
            if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        if (payload.wifi_swelling_len == strlen("normal") &&
            memcmp(payload.wifi_swelling, "normal", payload.wifi_swelling_len) == 0) {
            wifi_swelling = WARDRIVING_SWELLING_NORMAL;
            wifi_swelling_ok = true;
        } else if (payload.wifi_swelling_len == strlen("aggressive") &&
                  memcmp(payload.wifi_swelling, "aggressive", payload.wifi_swelling_len) == 0) {
            wifi_swelling = WARDRIVING_SWELLING_AGGRESSIVE;
            wifi_swelling_ok = true;
        } else if (payload.wifi_swelling_len == strlen("speed_based") &&
                  memcmp(payload.wifi_swelling, "speed_based", payload.wifi_swelling_len) == 0) {
            wifi_swelling = WARDRIVING_SWELLING_SPEED_BASED;
            wifi_swelling_ok = true;
        }

        if (payload.country_len == strlen("BG") &&
            memcmp(payload.country, "BG", payload.country_len) == 0) {
            country = WARDRIVING_COUNTRY_BG;
            country_ok = true;
        } else if (payload.country_len == strlen("RoW") &&
                  memcmp(payload.country, "RoW", payload.country_len) == 0) {
            country = WARDRIVING_COUNTRY_ROW;
            country_ok = true;
        }

#if FEB_WIFI_DUAL_BAND
        if (payload.wifi_band_len == strlen("2.4ghz") &&
            memcmp(payload.wifi_band, "2.4ghz", payload.wifi_band_len) == 0) {
            wifi_band = 0;
            wifi_band_ok = true;
        } else if (payload.wifi_band_len == strlen("5ghz_fast") &&
                  memcmp(payload.wifi_band, "5ghz_fast", payload.wifi_band_len) == 0) {
            wifi_band = 1;
            wifi_band_ok = true;
        } else if (payload.wifi_band_len == strlen("5ghz_full") &&
                  memcmp(payload.wifi_band, "5ghz_full", payload.wifi_band_len) == 0) {
            wifi_band = 2;
            wifi_band_ok = true;
        }
#endif

        if (!wifi_swelling_ok || !country_ok
#if FEB_WIFI_DUAL_BAND
            || !wifi_band_ok
#endif
            ) {
            if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }
    } else if (payload.has_wifi_swelling || payload.has_country
#if FEB_WIFI_DUAL_BAND
              || payload.has_wifi_band
#endif
              ) {
        if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    {
        wardriving_start_request_t req = {0};
        wardriving_resolved_intervals_t resolved;

        req.want_wifi = want_wifi;
        req.want_ble = want_ble || want_ble_passive;
        req.has_wifi_interval_ms = payload.has_wifi_interval_ms;
        req.wifi_interval_ms = payload.wifi_interval_ms;
        req.has_ble_params = payload.has_ble_params;
        req.ble_window_ms = payload.ble_window_ms;
        req.ble_interval_ms = payload.ble_interval_ms;

        if (!wardriving_resolve_start_intervals(&req, &resolved)) {
            if (!feb_send_protected_error(conn_handle, "invalid_command", strlen("invalid_command"), 1, cmd->request_id)) {
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        /* Stashed back into `payload` (not local-only) so the existing want_wifi/want_ble
           start blocks below, which already read payload.wifi_interval_ms/ble_window_ms/
           ble_interval_ms, pick up the resolved (explicit-and-valid, or defaulted) values
           unchanged. */
        payload.wifi_interval_ms = resolved.wifi_interval_ms;
        payload.ble_window_ms = resolved.ble_window_ms;
        payload.ble_interval_ms = resolved.ble_interval_ms;
    }

    if (want_wifi && feb_wifi_scan_in_progress) {
        if (!feb_send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    if ((want_ble || want_ble_passive) && feb_ble_scan_in_progress) {
        if (!feb_send_protected_error(conn_handle, "busy", strlen("busy"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    if (!feb_wardriving_start_internal(want_wifi, want_ble, want_ble_passive,
                                   (uint32_t)payload.wifi_interval_ms,
                                   (uint32_t)payload.ble_window_ms,
                                   (uint32_t)payload.ble_interval_ms,
                                   wifi_swelling, country
#if FEB_WIFI_DUAL_BAND
                                   , wifi_band
#endif
                                   )) {
        if (!feb_send_protected_error(conn_handle, "internal_error", strlen("internal_error"), 1, cmd->request_id)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }

    feb_wardriving_persisted.enabled = true;
    feb_wardriving_persisted.want_wifi = want_wifi;
    feb_wardriving_persisted.want_ble = want_ble;
    feb_wardriving_persisted.want_ble_passive = want_ble_passive;
    feb_wardriving_persisted.wifi_interval_ms = (uint32_t)payload.wifi_interval_ms;
    feb_wardriving_persisted.ble_window_ms = (uint32_t)payload.ble_window_ms;
    feb_wardriving_persisted.ble_interval_ms = (uint32_t)payload.ble_interval_ms;
    /* Wi-Fi settings persistence: wifi_swelling/country (and wifi_band on dual-band boards)
       now survive a reboot the same way the sources/intervals above already do, so boot
       autostart and the boot-button toggle-on can reuse the last `start` command's values
       instead of always falling back to Normal/RoW -- see board_hooks.c/main.c's autostart
       and button-toggle blocks. Only meaningful while want_wifi is true; left at whatever
       wifi_swelling/country/wifi_band were initialized to (Normal/RoW/0) otherwise, which is
       harmless since a Wi-Fi-less autostart never reads them. */
    feb_wardriving_persisted.wifi_swelling = (uint8_t)wifi_swelling;
    feb_wardriving_persisted.country = (uint8_t)country;
#if FEB_WIFI_DUAL_BAND
    feb_wardriving_persisted.wifi_band = wifi_band;
#endif
    wardriving_persist_save(&feb_wardriving_persisted);

    {
        feb_status_payload_t status_payload = {0};
        size_t payload_len;

        status_payload.request_id = cmd->request_id;
        status_payload.state = "started";
        status_payload.state_len = strlen("started");
        payload_len = feb_cbor_encode_status_payload(feb_pairing_payload_encode_buf,
                                                     sizeof(feb_pairing_payload_encode_buf), &status_payload);
        if (payload_len == 0 ||
            !feb_send_protected(conn_handle, "status", strlen("status"), feb_pairing_payload_encode_buf, payload_len)) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
    }
    ESP_LOGI(TAG, "wardriving started (request_id=%llu, wifi=%d ble=%d ble_passive=%d)",
             (unsigned long long)cmd->request_id, (int)want_wifi, (int)want_ble, (int)want_ble_passive);
}

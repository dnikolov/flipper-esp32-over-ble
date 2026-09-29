#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "wardriving_persist.h"
#include "wardriving_validate.h"

static const char *TAG = "feb_wardriving_persist";

#define FEB_WARDRIVING_PERSIST_NVS_NAMESPACE "feb_wardrv"
#define FEB_WARDRIVING_PERSIST_NVS_KEY "state"

/* Version-1 layout (before wifi_swelling/country/wifi_band were persisted), kept only so an
   existing saved run survives the upgrade instead of silently reverting to defaults with
   autostart off. The three new fields migrate as 0 (normal / RoW / 2.4ghz), which is what
   the v1 autostart path always ran at. */
typedef struct {
    uint32_t version;
    bool enabled;
    bool want_wifi;
    bool want_ble;
    bool want_ble_passive;
    uint32_t wifi_interval_ms;
    uint32_t ble_window_ms;
    uint32_t ble_interval_ms;
} wardriving_persisted_state_v1_t;

static void fill_defaults(feb_wardriving_persisted_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = FEB_WARDRIVING_PERSIST_VERSION;
    out->enabled = false;
    out->want_wifi = true;
    out->want_ble = true;
    out->want_ble_passive = false;
    out->wifi_interval_ms = FEB_WARDRIVING_WIFI_INTERVAL_DEFAULT_MS;
    out->ble_window_ms = FEB_WARDRIVING_BLE_WINDOW_DEFAULT_MS;
    out->ble_interval_ms = FEB_WARDRIVING_BLE_INTERVAL_DEFAULT_MS;
}

/* Re-validates a loaded blob's fields through the same bounds handle_wardriving_command()
   enforces on an explicit `start`, so a corrupted-but-right-size-and-version blob can't
   resolve to an out-of-range or structurally-nonsensical (e.g. window > interval, or
   neither source wanted) combination -- see wardriving_persist.h's top comment. */
static bool validate_and_resolve(feb_wardriving_persisted_state_t *out)
{
    wardriving_start_request_t req = {0};
    wardriving_resolved_intervals_t resolved;

    bool any_ble = out->want_ble || out->want_ble_passive;

    if ((!out->want_wifi && !any_ble) || (out->want_ble && out->want_ble_passive)) {
        return false;
    }
    if (out->wifi_swelling > FEB_WARDRIVING_PERSIST_SWELLING_MAX ||
        out->country > FEB_WARDRIVING_PERSIST_COUNTRY_MAX ||
        out->wifi_band > FEB_WARDRIVING_PERSIST_WIFI_BAND_MAX) {
        return false;
    }
    req.want_wifi = out->want_wifi;
    req.want_ble = any_ble;
    req.has_wifi_interval_ms = out->want_wifi ? 1 : 0;
    req.wifi_interval_ms = out->wifi_interval_ms;
    req.has_ble_params = any_ble ? 1 : 0;
    req.ble_window_ms = out->ble_window_ms;
    req.ble_interval_ms = out->ble_interval_ms;

    if (!wardriving_resolve_start_intervals(&req, &resolved)) {
        return false;
    }
    out->wifi_interval_ms = resolved.wifi_interval_ms;
    out->ble_window_ms = resolved.ble_window_ms;
    out->ble_interval_ms = resolved.ble_interval_ms;
    return true;
}

bool wardriving_persist_load(feb_wardriving_persisted_state_t *out)
{
    nvs_handle_t handle;
    esp_err_t err;
    union {
        feb_wardriving_persisted_state_t v2;
        wardriving_persisted_state_v1_t v1;
    } blob;
    size_t len = sizeof(blob);

    fill_defaults(out);

    err = nvs_open(FEB_WARDRIVING_PERSIST_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }
    memset(&blob, 0, sizeof(blob));
    err = nvs_get_blob(handle, FEB_WARDRIVING_PERSIST_NVS_KEY, &blob, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    if (len == sizeof(blob.v2) && blob.v2.version == FEB_WARDRIVING_PERSIST_VERSION) {
        *out = blob.v2;
    } else if (len == sizeof(blob.v1) && blob.v1.version == 1u) {
        out->enabled = blob.v1.enabled;
        out->want_wifi = blob.v1.want_wifi;
        out->want_ble = blob.v1.want_ble;
        out->want_ble_passive = blob.v1.want_ble_passive;
        out->wifi_interval_ms = blob.v1.wifi_interval_ms;
        out->ble_window_ms = blob.v1.ble_window_ms;
        out->ble_interval_ms = blob.v1.ble_interval_ms;
    } else {
        return false;
    }
    if (!validate_and_resolve(out)) {
        ESP_LOGW(TAG, "persisted wardriving state failed validation; using defaults");
        fill_defaults(out);
        return false;
    }
    return true;
}

void wardriving_persist_save(const feb_wardriving_persisted_state_t *state)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FEB_WARDRIVING_PERSIST_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(handle, FEB_WARDRIVING_PERSIST_NVS_KEY, state, sizeof(*state));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist wardriving state: %s", esp_err_to_name(err));
    }
}

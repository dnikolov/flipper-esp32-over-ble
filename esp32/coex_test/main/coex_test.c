#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"

/*
 * Throwaway plan-step-4 radio-coexistence sweep harness. Not wired into
 * esp32/main/main.c, not shared with the future step-7 wifi_scan/ble_scan
 * capability code. See docs/PLAN.md step 4 and docs/SESSION_MEMORY.md's
 * 2026-09-03 step 4 entries for the authoritative design this file
 * implements.
 */

static const char *TAG = "coex_test";

#define COEX_NVS_NAMESPACE "coex_test"
#define COEX_NVS_KEY_POINT "pt_idx"
#define COEX_NVS_KEY_ATTEMPT "attempt"

#define COEX_POINT_WINDOW_MS (30UL * 60UL * 1000UL)
#define COEX_MAX_ATTEMPTS 3U
#define COEX_SUMMARY_INTERVAL_MS (60UL * 1000UL)
#define COEX_IDLE_INTERVAL_MS (60UL * 1000UL)
#define COEX_POLL_INTERVAL_MS 1000UL

#define COEX_RECONNECT_BACKOFF_CEILING_RETRIES 5U
#define COEX_RECONNECT_SLOW_CADENCE_MS (60UL * 1000UL)
/*
 * A recovered disconnect counts as a (non-fatal) degradation signal only if
 * it lands within this bound. 45s comfortably covers the full five-step
 * exponential backoff ceiling (1+2+4+8+16 = 31s) plus scan/connect/MTU/
 * discovery overhead; the extra margin up to 120s also tolerates one cycle
 * of the revised policy's indefinite slow-cadence retry (60s) actually
 * being needed. Anything slower is treated as a hard fail even if the link
 * eventually comes back, because it exceeded the reconnect policy's
 * expected timing, not because it never recovered at all.
 */
#define COEX_RECONNECT_EXPECTED_MAX_GAP_MS (120UL * 1000UL)
#define COEX_WEDGE_TIMEOUT_MS (20UL * 1000UL)
#define COEX_HEARTBEAT_INTERVAL_MS (5UL * 1000UL)
#define COEX_WEDGE_CHECK_INTERVAL_MS (5UL * 1000UL)

static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
    0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x11);
static const uint8_t advertised_service_uuid[] = {
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
    0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x11};
static const ble_uuid128_t write_uuid = BLE_UUID128_INIT(
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
    0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x12);
static const ble_uuid128_t notify_uuid = BLE_UUID128_INIT(
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
    0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x13);

typedef enum {
    COEX_CFG_PAUSED = 0,
    COEX_CFG_CONCURRENT = 1,
} coex_config_kind_t;

typedef struct {
    const char *label;
    coex_config_kind_t kind;
    uint32_t wifi_interval_ms; /* delay after a Wi-Fi scan completes before starting the next; 0 = continuous */
    uint32_t ble_window_ms;    /* 0 with kind==PAUSED is a sentinel meaning "no observer scan" */
    uint32_t ble_interval_ms;
} coex_point_cfg_t;

static const coex_point_cfg_t POINTS[] = {
    {"baseline-paused", COEX_CFG_PAUSED, 0, 0, 0},
    {"concurrent-conservative-10pct", COEX_CFG_CONCURRENT, 30000, 100, 1000},
    {"concurrent-moderate-50pct", COEX_CFG_CONCURRENT, 15000, 100, 200},
    {"concurrent-aggressive-90pct", COEX_CFG_CONCURRENT, 0, 135, 150},
    {"concurrent-max-100pct", COEX_CFG_CONCURRENT, 0, 30, 30},
};
#define COEX_POINT_COUNT (sizeof(POINTS) / sizeof(POINTS[0]))

typedef enum {
    RECONNECT_VIA_NONE = 0,
    RECONNECT_VIA_MERGED,
    RECONNECT_VIA_DEDICATED,
} reconnect_via_t;

static nvs_handle_t g_nvs_handle;

static uint8_t own_addr_type;
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t service_start_handle;
static uint16_t service_end_handle;
static uint16_t write_value_handle;
static uint16_t notify_value_handle;
static uint16_t notify_cccd_handle;
static uint8_t reconnect_retries;
static bool reconnect_task_active;

static SemaphoreHandle_t g_state_mutex;

static coex_config_kind_t g_point_kind;
static uint32_t g_point_ble_window_ms;
static uint32_t g_point_ble_interval_ms;
static volatile uint32_t g_point_wifi_gap_ms;
static volatile bool g_wifi_scan_active;

static bool g_connected;
static bool g_disconnect_pending;
static bool g_disconnect_was_wedge_forced;
static uint32_t g_disconnect_time_ms;
static uint32_t g_last_link_activity_ms;
static uint32_t g_heartbeat_counter;

typedef struct {
    uint32_t disconnects;
    uint32_t hard_fails;
    uint32_t degradations;
    uint32_t longest_gap_ms;
    uint32_t reconnect_successes;
    reconnect_via_t last_reconnect_via;
} coex_point_stats_t;

static coex_point_stats_t g_stats;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void stats_lock(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
}

static void stats_unlock(void)
{
    xSemaphoreGive(g_state_mutex);
}

static void nvs_persist_progress(uint8_t point_index, uint8_t attempt)
{
    esp_err_t err;

    err = nvs_set_u8(g_nvs_handle, COEX_NVS_KEY_POINT, point_index);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(point) failed: %s", esp_err_to_name(err));
    }
    err = nvs_set_u8(g_nvs_handle, COEX_NVS_KEY_ATTEMPT, attempt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(attempt) failed: %s", esp_err_to_name(err));
    }
    err = nvs_commit(g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }
}

static uint8_t nvs_read_u8_default(const char *key, uint8_t default_value)
{
    uint8_t value = default_value;
    esp_err_t err = nvs_get_u8(g_nvs_handle, key, &value);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_get_u8(%s) failed: %s", key, esp_err_to_name(err));
    }
    return (err == ESP_OK) ? value : default_value;
}

static bool uuid128_matches(const ble_uuid_any_t *uuid, const ble_uuid128_t *expected)
{
    return uuid->u.type == BLE_UUID_TYPE_128 &&
           memcmp(uuid->u128.value, expected->value, sizeof(expected->value)) == 0;
}

static bool scan_record_matches(const uint8_t *data, uint8_t length)
{
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, data, length) != 0) {
        return false;
    }
    for (uint8_t index = 0; index < fields.num_uuids128; index++) {
        if (memcmp(fields.uuids128[index].value, advertised_service_uuid,
                   sizeof(advertised_service_uuid)) == 0) {
            return true;
        }
    }
    return false;
}

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_discovery(bool passive, uint32_t window_ms, uint32_t interval_ms)
{
    struct ble_gap_disc_params params = {0};
    int rc;

    params.passive = passive ? 1 : 0;
    params.filter_duplicates = 1;
    if (window_ms == 0 && interval_ms == 0) {
        params.itvl = 0;
        params.window = 0;
    } else {
        params.itvl = (uint16_t)BLE_GAP_SCAN_ITVL_MS(interval_ms);
        params.window = (uint16_t)BLE_GAP_SCAN_WIN_MS(window_ms);
    }
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "discovery start failed: %d", rc);
    }
}

/*
 * Called after every connect/disconnect and every point-config change.
 * Concurrent points keep the observer scan running at all times (including
 * while connected) so it doubles as the merged reconnect scan. Paused
 * points only scan while not connected, using the proven default active
 * scan from esp32/main/main.c's step 2 pattern.
 */
static void sync_discovery_state(void)
{
    bool want_active = (g_point_kind == COEX_CFG_CONCURRENT) ||
                        (connection_handle == BLE_HS_CONN_HANDLE_NONE);

    if (!want_active) {
        if (ble_gap_disc_active()) {
            ble_gap_disc_cancel();
        }
        return;
    }
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    if (g_point_kind == COEX_CFG_CONCURRENT) {
        start_discovery(true, g_point_ble_window_ms, g_point_ble_interval_ms);
    } else {
        start_discovery(false, 0, 0);
    }
}

static void reconnect_task(void *arg)
{
    TickType_t delay = (TickType_t)(uintptr_t)arg;

    vTaskDelay(delay);
    reconnect_task_active = false;
    sync_discovery_state();
    vTaskDelete(NULL);
}

static void schedule_dedicated_reconnect(void)
{
    uint32_t delay_ms;

    if (reconnect_task_active) {
        return;
    }
    if (reconnect_retries < COEX_RECONNECT_BACKOFF_CEILING_RETRIES) {
        reconnect_retries++;
        delay_ms = 1000UL << (reconnect_retries - 1);
    } else {
        delay_ms = COEX_RECONNECT_SLOW_CADENCE_MS;
    }
    reconnect_task_active = true;
    ESP_LOGI(TAG, "dedicated reconnect in %" PRIu32 " ms (retry %u)", delay_ms, reconnect_retries);
    if (xTaskCreate(reconnect_task, "coex_reconnect", 3072,
                    (void *)(uintptr_t)pdMS_TO_TICKS(delay_ms), 4, NULL) != pdPASS) {
        reconnect_task_active = false;
        ESP_LOGE(TAG, "could not schedule dedicated reconnect");
    }
}

static int mtu_exchanged(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg);
static int service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg);
static int characteristic_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                      const struct ble_gatt_chr *characteristic, void *arg);
static int descriptor_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 uint16_t characteristic_handle,
                                 const struct ble_gatt_dsc *descriptor, void *arg);
static int write_complete(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg);

static int mtu_exchanged(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    int rc;

    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "MTU exchange unavailable: %d", error->status);
    } else {
        ESP_LOGI(TAG, "negotiated ATT MTU: %u", mtu);
    }
    rc = ble_gattc_disc_svc_by_uuid(conn_handle, &service_uuid.u, service_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "service discovery start failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    int rc;

    if (error->status == 0 && service != NULL && uuid128_matches(&service->uuid, &service_uuid)) {
        service_start_handle = service->start_handle;
        service_end_handle = service->end_handle;
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (service_start_handle == 0) {
        ESP_LOGE(TAG, "v2 service not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    rc = ble_gattc_disc_all_chrs(conn_handle, service_start_handle, service_end_handle,
                                 characteristic_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "characteristic discovery start failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int characteristic_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                      const struct ble_gatt_chr *characteristic, void *arg)
{
    int rc;

    if (error->status == 0 && characteristic != NULL) {
        if (uuid128_matches(&characteristic->uuid, &write_uuid)) {
            write_value_handle = characteristic->val_handle;
        } else if (uuid128_matches(&characteristic->uuid, &notify_uuid)) {
            notify_value_handle = characteristic->val_handle;
        }
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (write_value_handle == 0 || notify_value_handle == 0) {
        ESP_LOGE(TAG, "required v2 characteristics not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    rc = ble_gattc_disc_all_dscs(conn_handle, service_start_handle, service_end_handle,
                                 descriptor_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD discovery start failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int descriptor_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 uint16_t characteristic_handle,
                                 const struct ble_gatt_dsc *descriptor, void *arg)
{
    int rc;
    static const uint8_t notify_enabled[] = {0x01, 0x00};

    if (error->status == 0 && descriptor != NULL &&
        descriptor->uuid.u.type == BLE_UUID_TYPE_16 &&
        descriptor->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16) {
        notify_cccd_handle = descriptor->handle;
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (notify_cccd_handle == 0) {
        ESP_LOGE(TAG, "notification CCCD not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    rc = ble_gattc_write_flat(conn_handle, notify_cccd_handle, notify_enabled, sizeof(notify_enabled),
                              write_complete, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "notification subscribe failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int write_complete(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "GATT write failed: %d", error->status);
        return 0;
    }
    if (notify_cccd_handle != 0 && attr != NULL && attr->handle == notify_cccd_handle) {
        ESP_LOGI(TAG, "notifications subscribed; connection ready");
        sync_discovery_state();
    }
    return 0;
}

static int heartbeat_write_complete(uint16_t conn_handle, const struct ble_gatt_error *error,
                                    struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "heartbeat write failed: %d", error->status);
        return 0;
    }
    stats_lock();
    g_last_link_activity_ms = now_ms();
    stats_unlock();
    return 0;
}

static void classify_reconnect(uint32_t gap_ms)
{
    if (g_disconnect_was_wedge_forced) {
        /* Already counted as a hard fail when the wedge was detected. */
        g_disconnect_was_wedge_forced = false;
        return;
    }
    if (gap_ms <= COEX_RECONNECT_EXPECTED_MAX_GAP_MS) {
        g_stats.degradations++;
    } else {
        g_stats.hard_fails++;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (connection_handle == BLE_HS_CONN_HANDLE_NONE &&
            scan_record_matches(event->disc.data, event->disc.length_data)) {
            ESP_LOGI(TAG, "found v2 peer, connecting");
            ble_gap_disc_cancel();
            rc = ble_gap_connect(own_addr_type, &event->disc.addr, 30000, NULL, gap_event, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "connect start failed: %d", rc);
                sync_discovery_state();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        sync_discovery_state();
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection attempt failed: %d", event->connect.status);
            if (g_point_kind == COEX_CFG_PAUSED) {
                schedule_dedicated_reconnect();
            } else {
                sync_discovery_state();
            }
            return 0;
        }
        connection_handle = event->connect.conn_handle;
        reconnect_retries = 0;
        service_start_handle = 0;
        service_end_handle = 0;
        write_value_handle = 0;
        notify_value_handle = 0;
        notify_cccd_handle = 0;

        stats_lock();
        g_connected = true;
        g_last_link_activity_ms = now_ms();
        if (g_disconnect_pending) {
            uint32_t gap_ms = now_ms() - g_disconnect_time_ms;

            if (gap_ms > g_stats.longest_gap_ms) {
                g_stats.longest_gap_ms = gap_ms;
            }
            classify_reconnect(gap_ms);
            g_stats.reconnect_successes++;
            g_stats.last_reconnect_via =
                (g_point_kind == COEX_CFG_CONCURRENT) ? RECONNECT_VIA_MERGED : RECONNECT_VIA_DEDICATED;
            g_disconnect_pending = false;
        }
        stats_unlock();

        ESP_LOGI(TAG, "connected; exchanging MTU");
        rc = ble_gattc_exchange_mtu(connection_handle, mtu_exchanged, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "MTU exchange start failed: %d", rc);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected: reason=%d", event->disconnect.reason);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        service_start_handle = 0;
        service_end_handle = 0;
        write_value_handle = 0;
        notify_value_handle = 0;
        notify_cccd_handle = 0;

        stats_lock();
        g_connected = false;
        g_disconnect_pending = true;
        g_disconnect_time_ms = now_ms();
        g_stats.disconnects++;
        stats_unlock();

        if (g_point_kind == COEX_CFG_PAUSED) {
            schedule_dedicated_reconnect();
        } else {
            sync_discovery_state();
        }
        return 0;

    default:
        return 0;
    }
}

static void apply_point_ble_config(const coex_point_cfg_t *point)
{
    stats_lock();
    g_point_kind = point->kind;
    g_point_ble_window_ms = point->ble_window_ms;
    g_point_ble_interval_ms = point->ble_interval_ms;
    stats_unlock();
    sync_discovery_state();
}

static void host_synced(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);

    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        return;
    }
    sync_discovery_state();
}

static void nimble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

static void heartbeat_task(void *arg)
{
    static uint8_t payload[4];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(COEX_HEARTBEAT_INTERVAL_MS));
        if (connection_handle == BLE_HS_CONN_HANDLE_NONE || write_value_handle == 0) {
            continue;
        }
        g_heartbeat_counter++;
        memcpy(payload, &g_heartbeat_counter, sizeof(payload));
        ble_gattc_write_flat(connection_handle, write_value_handle, payload, sizeof(payload),
                             heartbeat_write_complete, NULL);
    }
}

static void wedge_monitor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(COEX_WEDGE_CHECK_INTERVAL_MS));
        stats_lock();
        if (g_connected && (now_ms() - g_last_link_activity_ms) > COEX_WEDGE_TIMEOUT_MS) {
            ESP_LOGE(TAG, "connection appears wedged; forcing disconnect to recover");
            g_stats.hard_fails++;
            g_disconnect_was_wedge_forced = true;
            g_last_link_activity_ms = now_ms();
            stats_unlock();
            if (connection_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            continue;
        }
        stats_unlock();
    }
}

static void wifi_scan_task(void *arg)
{
    esp_err_t err;

    for (;;) {
        if (!g_wifi_scan_active) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        err = esp_wifi_scan_start(NULL, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint32_t gap_ms = g_point_wifi_gap_ms;

        if (gap_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

static void wifi_init_once(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static const char *config_kind_label(coex_config_kind_t kind)
{
    return (kind == COEX_CFG_PAUSED) ? "paused" : "concurrent";
}

static const char *reconnect_via_label(reconnect_via_t via)
{
    switch (via) {
    case RECONNECT_VIA_MERGED:
        return "merged";
    case RECONNECT_VIA_DEDICATED:
        return "dedicated";
    default:
        return "none";
    }
}

static void run_point(uint8_t point_index, uint8_t attempt)
{
    const coex_point_cfg_t *point = &POINTS[point_index];
    uint32_t start_ms = now_ms();
    uint32_t elapsed_ms;
    uint32_t next_summary_ms = COEX_SUMMARY_INTERVAL_MS;

    stats_lock();
    memset(&g_stats, 0, sizeof(g_stats));
    g_disconnect_was_wedge_forced = false;
    stats_unlock();

    apply_point_ble_config(point);
    g_point_wifi_gap_ms = point->wifi_interval_ms;
    g_wifi_scan_active = true;

    ESP_LOGI(TAG,
             "COEX_POINT_START point=%u config=%s label=%s wifi_interval_ms=%" PRIu32
             " ble_window_ms=%" PRIu32 " ble_interval_ms=%" PRIu32 " attempt=%u",
             point_index, config_kind_label(point->kind), point->label, point->wifi_interval_ms,
             point->ble_window_ms, point->ble_interval_ms, attempt);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(COEX_POLL_INTERVAL_MS));
        elapsed_ms = now_ms() - start_ms;
        if (elapsed_ms >= next_summary_ms) {
            stats_lock();
            uint32_t disconnects = g_stats.disconnects;
            uint32_t hard_fails = g_stats.hard_fails;
            uint32_t degradations = g_stats.degradations;
            uint32_t longest_gap_ms = g_stats.longest_gap_ms;
            reconnect_via_t last_via = g_stats.last_reconnect_via;
            stats_unlock();
            ESP_LOGI(TAG,
                     "COEX_SUMMARY point=%u t_ms=%" PRIu32 " disconnects=%" PRIu32
                     " hard_fails=%" PRIu32 " degradations=%" PRIu32 " longest_gap_ms=%" PRIu32
                     " last_reconnect_via=%s",
                     point_index, elapsed_ms, disconnects, hard_fails, degradations, longest_gap_ms,
                     reconnect_via_label(last_via));
            next_summary_ms += COEX_SUMMARY_INTERVAL_MS;
        }
        if (elapsed_ms >= COEX_POINT_WINDOW_MS) {
            break;
        }
    }

    g_wifi_scan_active = false;

    stats_lock();
    if (g_disconnect_pending) {
        uint32_t gap_ms = now_ms() - g_disconnect_time_ms;

        if (gap_ms > g_stats.longest_gap_ms) {
            g_stats.longest_gap_ms = gap_ms;
        }
        g_stats.hard_fails++;
        ESP_LOGE(TAG, "point %u ended still disconnected after %" PRIu32 " ms", point_index, gap_ms);
    }
    uint32_t disconnects = g_stats.disconnects;
    uint32_t hard_fails = g_stats.hard_fails;
    uint32_t degradations = g_stats.degradations;
    uint32_t longest_gap_ms = g_stats.longest_gap_ms;
    uint32_t reconnect_successes = g_stats.reconnect_successes;
    stats_unlock();

    const char *merged_ok;

    if (point->kind == COEX_CFG_PAUSED || disconnects == 0) {
        merged_ok = "na";
    } else {
        merged_ok = (reconnect_successes > 0) ? "true" : "false";
    }

    uint32_t duration_ms = now_ms() - start_ms;
    const char *result = (hard_fails > 0) ? "HARD_FAIL" : "PASS";

    ESP_LOGI(TAG,
             "COEX_POINT_RESULT point=%u config=%s result=%s disconnects=%" PRIu32
             " hard_fails=%" PRIu32 " degradations=%" PRIu32 " longest_gap_ms=%" PRIu32
             " merged_reconnect_ok=%s duration_ms=%" PRIu32 " attempt=%u",
             point_index, config_kind_label(point->kind), result, disconnects, hard_fails,
             degradations, longest_gap_ms, merged_ok, duration_ms, attempt);
}

static void point_runner_task(void *arg)
{
    uint8_t point_index = nvs_read_u8_default(COEX_NVS_KEY_POINT, 0);

    while (point_index < COEX_POINT_COUNT) {
        uint8_t attempt = nvs_read_u8_default(COEX_NVS_KEY_ATTEMPT, 0);

        attempt++;
        nvs_persist_progress(point_index, attempt);
        ESP_LOGI(TAG, "COEX_BOOT point=%u attempt=%u", point_index, attempt);

        if (attempt > COEX_MAX_ATTEMPTS) {
            ESP_LOGE(TAG, "COEX_POINT_UNSTABLE point=%u reason=retry_budget_exceeded attempts=%u",
                     point_index, COEX_MAX_ATTEMPTS);
            point_index++;
            nvs_persist_progress(point_index, 0);
            continue;
        }

        run_point(point_index, attempt);
        point_index++;
        nvs_persist_progress(point_index, 0);
    }

    ESP_LOGI(TAG, "COEX_SWEEP_DONE points_completed=%u", (unsigned)COEX_POINT_COUNT);
    g_wifi_scan_active = false;
    uint32_t idle_start_ms = now_ms();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(COEX_IDLE_INTERVAL_MS));
        ESP_LOGI(TAG, "COEX_IDLE t_ms=%" PRIu32, now_ms() - idle_start_ms);
    }
}

static void nvs_init_once(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(COEX_NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle));
}

void app_main(void)
{
    esp_err_t err;

    nvs_init_once();

    g_state_mutex = xSemaphoreCreateMutex();
    if (g_state_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create state mutex");
        return;
    }

    wifi_init_once();

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = host_synced;
    nimble_port_freertos_init(nimble_host_task);

    xTaskCreate(wifi_scan_task, "coex_wifi_scan", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "coex_heartbeat", 3072, NULL, 5, NULL);
    xTaskCreate(wedge_monitor_task, "coex_wedge_mon", 3072, NULL, 5, NULL);
    xTaskCreate(point_runner_task, "coex_point_runner", 4096, NULL, 5, NULL);
}

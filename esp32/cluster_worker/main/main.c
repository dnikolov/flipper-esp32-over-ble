#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "cluster_link.h"

/* Phase 9 cluster-mode C6 worker (docs/CLUSTER.md's "Roles" table: C6 = wifi_scan,
   2.4GHz only, never talks to the Flipper, no BLE stack). Sibling build to
   esp32/main/ (the standalone board firmware) -- this is a separate, opt-in
   configuration; esp32/main/ is untouched and remains the default build.

   Wiring: UART1, TX=GPIO19 (-> Heltec RX/GPIO33), RX=GPIO18 (<- Heltec TX/GPIO32),
   115200 8N1 -- hardware-confirmed 2026-09-26, see
   docs/hardware/esp32-c6-devkitc-1/README.md's "Phase 9 cluster inter-board UART
   link" section and esp32/uart_link_test/ (the throwaway bring-up test this
   firmware supersedes).

   All Wi-Fi scan lifecycle state (s_mode/s_dwell_mode/s_scan_in_progress) is owned
   exclusively by scan_ctl_task: both the UART-RX task (on a new scan_config_set)
   and the Wi-Fi scan-done event handler (on the sys_evt task) only ever post an
   event onto s_control_queue and never touch that state directly, so there is
   nothing to lock. */

#define UART_PORT UART_NUM_1
#define UART_TX_PIN 19
#define UART_RX_PIN 18
#define UART_BAUD_RATE 115200
#define UART_RX_BUF_SIZE 2048
#define UART_RX_CHUNK_SIZE 256
#define UART_RX_TASK_STACK_SIZE 6144

#define WORKER_HELLO_INTERVAL_MS 1000

/* Mirrors esp32/main/main.c's FEB_WIFI_SCAN_RAW_MAX: a memory-bounded cap on one
   scan pass's raw results, not a report-size cap -- the coordinator (not this
   board) truncates to the top-32-by-RSSI it actually reports to the Flipper
   (docs/CLUSTER.md "Composite behaviors"), so every AP within this bound is
   forwarded as its own SCAN_RESULT frame. */
#define WIFI_SCAN_RAW_MAX 64u

#define CONTROL_QUEUE_DEPTH 8

static const char *TAG = "cluster_worker";

typedef enum {
    CLUSTER_EVT_CONFIG_SET,
    CLUSTER_EVT_SCAN_DONE,
} cluster_worker_event_type_t;

typedef struct {
    cluster_worker_event_type_t type;
    feb_cluster_scan_config_t config; /* valid only when type == CLUSTER_EVT_CONFIG_SET */
} cluster_worker_event_t;

static QueueHandle_t s_control_queue;

/* Owned exclusively by scan_ctl_task (see file header comment). */
static feb_cluster_scan_mode_t s_mode = FEB_CLUSTER_SCAN_MODE_IDLE;
static feb_cluster_dwell_mode_t s_dwell_mode = FEB_CLUSTER_DWELL_NORMAL;
static bool s_scan_in_progress;

/* Written by wifi_scan_done_handler() (sys_evt task) strictly before it enqueues
   CLUSTER_EVT_SCAN_DONE, read by handle_scan_done() (scan_ctl_task) strictly after
   it dequeues that same event -- the queue send/receive pair is the synchronizing
   handoff, so no additional lock is needed (same shape as esp32/main/main.c's
   wifi_scan_raw_records/wifi_scan_raw_count handoff via wifi_scan_done_co). */
static wifi_ap_record_t s_scan_raw_records[WIFI_SCAN_RAW_MAX];
static uint16_t s_scan_raw_count;

/* docs/PROTOCOL.md's phy string enum, collapsed the same "highest generation wins"
   way esp32/main/main.c's wifi_scan_phy_str() already does -- this board has no
   5GHz radio, so phy_11a/phy_11ac are never observably set here either. */
static uint8_t cluster_worker_phy_num(const wifi_ap_record_t *rec)
{
    if (rec->phy_11ax) {
        return (uint8_t)FEB_CLUSTER_PHY_11AX;
    }
    if (rec->phy_11n) {
        return (uint8_t)FEB_CLUSTER_PHY_11N;
    }
    if (rec->phy_11g) {
        return (uint8_t)FEB_CLUSTER_PHY_11G;
    }
    return (uint8_t)FEB_CLUSTER_PHY_11B;
}

/* wifi_auth_mode_t's numeric values match FEB_CLUSTER_AUTH_* 0..16 exactly (both
   pinned to this same ESP-IDF v5.5.2 enum order, per cluster_link.h's own comment
   and docs/PROTOCOL.md's auth string table) -- mirrors esp32/main/main.c's
   wifi_scan_auth_str() switch shape, but returns the numeric ID instead of text. */
static uint8_t cluster_worker_auth_num(wifi_auth_mode_t mode)
{
    if ((uint32_t)mode <= (uint32_t)FEB_CLUSTER_AUTH_WPA_ENTERPRISE) {
        return (uint8_t)mode;
    }
    return (uint8_t)FEB_CLUSTER_AUTH_UNKNOWN;
}

/* Mirrors esp32/main/main.c's wardriving_apply_wifi_swelling(): "normal" leaves
   scan_cfg zeroed (default dwell), "aggressive" pins the same fixed 85ms active
   dwell per channel. WARDRIVING_SWELLING_SPEED_BASED's dynamic aggressive/normal
   toggle there is driven by a fresh on-board GPS speed read -- this cluster worker
   has no GPS (GPS is Heltec-only, docs/CLUSTER.md "Physical wiring"), and
   SCAN_CONFIG_SET (docs/CLUSTER.md's frozen frame set) carries no speed or
   aggressive-flag field for the coordinator to forward one. Treated as NORMAL
   (default dwell) until/unless a future protocol revision adds a way to convey
   it -- flagged, not silently guessed at. */
static void apply_dwell(wifi_scan_config_t *scan_cfg)
{
    if (s_dwell_mode == FEB_CLUSTER_DWELL_AGGRESSIVE) {
        scan_cfg->scan_time.active.min = 85;
        scan_cfg->scan_time.active.max = 85;
    }
}

static void start_one_scan(void)
{
    wifi_scan_config_t scan_cfg;
    esp_err_t err;

    memset(&scan_cfg, 0, sizeof(scan_cfg));
    apply_dwell(&scan_cfg);
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return;
    }
    s_scan_in_progress = true;
}

static void handle_config_set(const feb_cluster_scan_config_t *cfg)
{
    feb_cluster_scan_mode_t new_mode = (feb_cluster_scan_mode_t)cfg->mode;

    if (new_mode != FEB_CLUSTER_SCAN_MODE_IDLE && new_mode != FEB_CLUSTER_SCAN_MODE_CONTINUOUS &&
        new_mode != FEB_CLUSTER_SCAN_MODE_MANUAL) {
        ESP_LOGW(TAG, "scan_config_set: unknown mode %u, ignored", (unsigned)cfg->mode);
        return;
    }

    s_dwell_mode = (feb_cluster_dwell_mode_t)cfg->dwell_mode;
    /* cfg->band_filter only means anything to the 5GHz (C5) worker per
       docs/CLUSTER.md -- this board is 2.4GHz-only, so it's read and ignored. */

    if (new_mode == FEB_CLUSTER_SCAN_MODE_IDLE) {
        s_mode = FEB_CLUSTER_SCAN_MODE_IDLE;
        if (s_scan_in_progress) {
            esp_err_t err = esp_wifi_scan_stop();

            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_scan_stop failed: %s", esp_err_to_name(err));
            }
            /* The in-flight scan still fires WIFI_EVENT_SCAN_DONE once more (ESP-IDF's
               own behavior for a stopped scan) -- handle_scan_done() checks
               s_mode == IDLE and drops those results instead of re-arming, same shape
               as esp32/main/main.c's wardriving_wifi_interval_cb()'s
               "if (!wardriving_wifi_active) return" guard. */
        }
        return;
    }

    s_mode = new_mode;
    if (!s_scan_in_progress) {
        start_one_scan();
    }
    /* else: a scan from the previous mode is still in flight; handle_scan_done()
       will apply the just-updated s_mode/s_dwell_mode starting with the next pass
       instead of racing esp_wifi_scan_start() against an already-running scan. */
}

static void handle_scan_done(void)
{
    uint16_t sent = 0;
    uint16_t i;

    s_scan_in_progress = false;

    if (s_mode == FEB_CLUSTER_SCAN_MODE_IDLE) {
        return;
    }

    for (i = 0; i < s_scan_raw_count; i++) {
        wifi_ap_record_t *rec = &s_scan_raw_records[i];
        feb_cluster_scan_result_t result;
        uint8_t frame[FEB_CLUSTER_MAX_FRAME_SIZE];
        size_t ssid_len = strnlen((const char *)rec->ssid, sizeof(rec->ssid) - 1u);
        size_t frame_len;

        memset(&result, 0, sizeof(result));
        memcpy(result.ssid, rec->ssid, ssid_len);
        result.ssid_len = (uint8_t)ssid_len;
        memcpy(result.bssid, rec->bssid, FEB_CLUSTER_SCAN_RESULT_BSSID_LEN);
        result.rssi = (int8_t)rec->rssi;
        result.channel = rec->primary;
        result.phy = cluster_worker_phy_num(rec);
        result.auth = cluster_worker_auth_num(rec->authmode);

        frame_len = feb_cluster_encode_scan_result(frame, sizeof(frame), &result);
        if (frame_len == 0) {
            ESP_LOGW(TAG, "scan_result encode failed, dropped one AP");
            continue;
        }
        uart_write_bytes(UART_PORT, frame, frame_len);
        sent++;
    }

    if (s_mode == FEB_CLUSTER_SCAN_MODE_MANUAL) {
        feb_cluster_scan_batch_done_t done = { .count = sent };
        uint8_t frame[FEB_CLUSTER_MAX_FRAME_SIZE];
        size_t frame_len = feb_cluster_encode_scan_batch_done(frame, sizeof(frame), &done);

        if (frame_len > 0) {
            uart_write_bytes(UART_PORT, frame, frame_len);
        } else {
            ESP_LOGW(TAG, "scan_batch_done encode failed");
        }
        s_mode = FEB_CLUSTER_SCAN_MODE_IDLE;
        return;
    }

    /* continuous: re-arm immediately, same "back-to-back" shape as
       esp32/main/main.c's wardriving_wifi_interval_ms == 0 default. */
    start_one_scan();
}

/* Runs on the sys_evt task (esp_event's default loop task) -- kept minimal, per
   esp32/main/main.c's wifi_scan_done_handler() precedent: copy the raw records out
   of the driver's own scan-result buffer, then hand off to scan_ctl_task for
   everything else (phy/auth translation, UART framing/writes, re-arming). */
static void wifi_scan_done_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint16_t total_found = 0;
    uint16_t raw_count;
    cluster_worker_event_t evt;

    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    if (esp_wifi_scan_get_ap_num(&total_found) != ESP_OK) {
        total_found = 0;
    }
    raw_count = (total_found > WIFI_SCAN_RAW_MAX) ? WIFI_SCAN_RAW_MAX : total_found;
    if (raw_count > 0 && esp_wifi_scan_get_ap_records(&raw_count, s_scan_raw_records) != ESP_OK) {
        raw_count = 0;
    }
    if (total_found > WIFI_SCAN_RAW_MAX) {
        ESP_LOGW(TAG, "wifi scan found %u APs, exceeding the %u-entry raw-fetch bound; only "
                      "the first %u (driver order) are forwarded",
                 (unsigned)total_found, (unsigned)WIFI_SCAN_RAW_MAX, (unsigned)WIFI_SCAN_RAW_MAX);
    }
    s_scan_raw_count = raw_count;

    evt.type = CLUSTER_EVT_SCAN_DONE;
    if (xQueueSend(s_control_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "control queue full, scan-done event dropped");
    }
}

static void scan_ctl_task(void *arg)
{
    (void)arg;

    for (;;) {
        cluster_worker_event_t evt;

        if (xQueueReceive(s_control_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt.type) {
        case CLUSTER_EVT_CONFIG_SET:
            handle_config_set(&evt.config);
            break;
        case CLUSTER_EVT_SCAN_DONE:
            handle_scan_done();
            break;
        default:
            break;
        }
    }
}

/* static (not stack-local): sole reader of this UART is this task -- no cross-task
   access, so there is no lifetime/locking concern -- and the C6's DRAM budget has ample
   room for this ~524-byte struct living in .bss permanently (unlike the classic-ESP32
   Heltec side of this same link, whose tight DRAM/.bss budget forced its equivalent
   decoder to be task-stack-local instead -- see heltec/main/main.c's cluster_link_rx_task).
   Only one feb_cluster_frame_t is ever in scope at a time (byte-at-a-time decode via
   feb_cluster_decoder_feed_byte(), no multi-frame batching), so that stays an ordinary
   stack-local instead of needing its own static storage. */
static void uart_rx_task(void *arg)
{
    static feb_cluster_decoder_t decoder;
    uint8_t chunk[UART_RX_CHUNK_SIZE];
    feb_cluster_frame_t frame;

    (void)arg;
    feb_cluster_decoder_init(&decoder);

    for (;;) {
        int read = uart_read_bytes(UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        int i;

        if (read < 0) {
            ESP_LOGE(TAG, "uart_read_bytes failed: %d", read);
            continue;
        }
        for (i = 0; i < read; i++) {
            feb_cluster_decode_result_t result =
                feb_cluster_decoder_feed_byte(&decoder, chunk[i], &frame);
            cluster_worker_event_t evt;

            if (result != FEB_CLUSTER_DECODE_FRAME_READY) {
                continue;
            }
            if (frame.msg_type != (uint8_t)FEB_CLUSTER_MSG_SCAN_CONFIG_SET) {
                /* Only SCAN_CONFIG_SET ever flows coordinator -> worker (docs/CLUSTER.md). */
                continue;
            }
            evt.type = CLUSTER_EVT_CONFIG_SET;
            if (!feb_cluster_decode_scan_config_set(&frame, &evt.config)) {
                ESP_LOGW(TAG, "malformed scan_config_set frame, dropped");
                continue;
            }
            if (xQueueSend(s_control_queue, &evt, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "control queue full, dropped scan_config_set");
            }
        }
    }
}

static void hello_task(void *arg)
{
    feb_cluster_worker_hello_t hello = { .band = (uint8_t)FEB_CLUSTER_BAND_24GHZ };

    (void)arg;
    for (;;) {
        uint8_t frame[FEB_CLUSTER_MAX_FRAME_SIZE];
        size_t frame_len = feb_cluster_encode_worker_hello(frame, sizeof(frame), &hello);

        if (frame_len > 0) {
            uart_write_bytes(UART_PORT, frame, frame_len);
        } else {
            ESP_LOGW(TAG, "worker_hello encode failed");
        }
        vTaskDelay(pdMS_TO_TICKS(WORKER_HELLO_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_err_t err;
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    s_control_queue = xQueueCreate(CONTROL_QUEUE_DEPTH, sizeof(cluster_worker_event_t));
    if (s_control_queue == NULL) {
        ESP_LOGE(TAG, "control queue creation failed");
        return;
    }

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART1 up: TX=GPIO%d RX=GPIO%d baud=%d", UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return;
    }
    (void)esp_netif_create_default_wifi_sta();

    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return;
        }
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                               &wifi_scan_done_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi scan-done handler registration failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return;
    }
    /* This board has no 5GHz radio at all (esp32c6) -- pinning band mode explicitly
       is belt-and-suspenders per docs/CLUSTER.md's role assignment ("C6: wifi_scan,
       2.4GHz only"), not expected to change behavior. Best-effort: a failure here
       doesn't stop the worker from starting, matching esp32/main/main.c's treatment
       of esp_wifi_set_country_code(). */
    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_band_mode(2G_ONLY) failed: %s", esp_err_to_name(err));
    }

    xTaskCreate(scan_ctl_task, "scan_ctl", 4096, NULL, 6, NULL);
    xTaskCreate(uart_rx_task, "uart_rx", UART_RX_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(hello_task, "worker_hello", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "cluster_worker up: band=2.4GHz, idle, waiting for scan_config_set");
}

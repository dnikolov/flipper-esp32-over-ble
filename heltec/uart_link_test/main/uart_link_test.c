#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Throwaway Phase 9 step 1 bring-up: confirm raw bytes round-trip over the
 * Heltec<->C6 wired UART link (docs/CLUSTER.md, docs/PLAN.md's Phase 9 step 1).
 * No framing, no CBOR, nothing from components/feb_protocol -- deliberately
 * disposable, mirroring heltec/gps_probe/'s precedent (built, used, deleted).
 *
 * Pins per docs/hardware/heltec-wifi-lora-32-v2/README.md's "Phase 9 cluster
 * inter-board UART link" section: Heltec GPIO32 = TX (-> C6 RX GPIO18),
 * Heltec GPIO33 = RX (<- C6 TX GPIO19). UART0 is left alone since it is this
 * board's CP2102 USB-console/log path.
 *
 * UNRESOLVED CAVEAT carried from that doc section: GPIO32/33 are labeled
 * XTAL32 on the vendor pinout diagram used to pick them -- shared with an
 * optional 32.768kHz crystal footprint some ESP32 boards populate for
 * deep-sleep timing. It has not been visually confirmed whether this
 * specific physical board populates that crystal. If it does, these pins
 * are not usable as plain GPIO and this test's result should not be trusted
 * without that visual check first. */

static const char *TAG = "uart_link_test";

#define LINK_UART_PORT UART_NUM_1
#define LINK_UART_TX_GPIO 32
#define LINK_UART_RX_GPIO 33
#define LINK_UART_BAUD 115200
#define LINK_UART_RX_BUF_SIZE 512
#define LINK_UART_TX_BUF_SIZE 0

static void uart_link_tx_task(void *arg)
{
    (void)arg;
    static const char kMessage[] = "HELTEC->C6\n";

    for (;;) {
        int written = uart_write_bytes(LINK_UART_PORT, kMessage, strlen(kMessage));
        if (written < 0) {
            ESP_LOGE(TAG, "uart_write_bytes failed: %d", written);
        } else {
            ESP_LOGI(TAG, "tx: %d bytes", written);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void uart_link_rx_task(void *arg)
{
    (void)arg;
    static uint8_t rx_buf[128];

    for (;;) {
        int read_len = uart_read_bytes(LINK_UART_PORT, rx_buf, sizeof(rx_buf) - 1,
                                        pdMS_TO_TICKS(200));
        if (read_len < 0) {
            ESP_LOGE(TAG, "uart_read_bytes failed: %d", read_len);
            continue;
        }
        if (read_len == 0) {
            continue;
        }
        rx_buf[read_len] = '\0';
        ESP_LOGI(TAG, "rx: %d bytes: %s", read_len, (const char *)rx_buf);
        ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buf, (size_t)read_len, ESP_LOG_INFO);
    }
}

void app_main(void)
{
    uart_config_t uart_config = {
        .baud_rate = LINK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_PORT, LINK_UART_RX_BUF_SIZE,
                                         LINK_UART_TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_PORT, LINK_UART_TX_GPIO, LINK_UART_RX_GPIO,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d up: TX=GPIO%d RX=GPIO%d baud=%d", LINK_UART_PORT,
             LINK_UART_TX_GPIO, LINK_UART_RX_GPIO, LINK_UART_BAUD);
    ESP_LOGW(TAG, "GPIO32/33 double as XTAL32 -- unconfirmed whether this board "
                  "populates that crystal footprint. Visually check for a small "
                  "2-pin crystal can near an XTAL/32.768 silkscreen mark before "
                  "trusting this link's results.");

    xTaskCreate(uart_link_tx_task, "uart_link_tx", 3072, NULL, 5, NULL);
    xTaskCreate(uart_link_rx_task, "uart_link_rx", 3072, NULL, 5, NULL);
}

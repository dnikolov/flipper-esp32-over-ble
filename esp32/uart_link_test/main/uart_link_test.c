#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Phase 9 step 1 throwaway bring-up: raw byte round-trip over the C6<->Heltec
 * UART link (docs/CLUSTER.md, docs/hardware/esp32-c6-devkitc-1/README.md's
 * "Phase 9 cluster inter-board UART link" section). No framing, no CBOR,
 * nothing from components/feb_protocol - this is not real project firmware
 * and is not meant to be built on top of. TX=GPIO19 (-> Heltec RX/GPIO32),
 * RX=GPIO18 (<- Heltec TX/GPIO33), UART1, 115200 8N1 (no baud constraint on
 * this link, unlike the GPS UART sharing these same pins on the standalone
 * build).
 */

#define UART_PORT UART_NUM_1
#define PIN_TX 19
#define PIN_RX 18
#define BAUD_RATE 115200
#define RX_BUF_SIZE 1024
#define TX_MSG "C6->HELTEC\n"

static const char *TAG = "uart_link_test";

static void tx_task(void *arg)
{
    (void)arg;

    for (;;) {
        int written = uart_write_bytes(UART_PORT, TX_MSG, strlen(TX_MSG));
        if (written < 0) {
            ESP_LOGE(TAG, "uart_write_bytes failed: %d", written);
        } else {
            ESP_LOGI(TAG, "sent %d bytes: %s", written, TX_MSG);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void rx_task(void *arg)
{
    uint8_t chunk[128];

    (void)arg;

    for (;;) {
        int read = uart_read_bytes(UART_PORT, chunk, sizeof(chunk) - 1, pdMS_TO_TICKS(200));
        if (read < 0) {
            ESP_LOGE(TAG, "uart_read_bytes failed: %d", read);
            continue;
        }
        if (read == 0) {
            continue;
        }
        chunk[read] = '\0';
        ESP_LOGI(TAG, "recv %d bytes: %s", read, (const char *)chunk);
    }
}

void app_main(void)
{
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART1 up: TX=GPIO%d RX=GPIO%d baud=%d", PIN_TX, PIN_RX, BAUD_RATE);

    xTaskCreate(tx_task, "uart_tx", 4096, NULL, 5, NULL);
    xTaskCreate(rx_task, "uart_rx", 4096, NULL, 5, NULL);
}

#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"

/*
 * Persistent GPS antenna smoke-test firmware. Listens at the confirmed
 * 9600 8N1 rate (UART1, RX=GPIO18, TX=GPIO19) and logs each complete NMEA
 * sentence as its own ESP_LOGI line under tag "nmea", so
 * tools/test_gps_antenna.ps1 can parse the monitor stream without dealing
 * with hex dumps. No TX traffic on purpose -- an earlier wake/cold-start
 * command burst was proven unnecessary and actively harmful (it force-
 * resets the receiver's acquisition state on every monitor reconnect).
 * See docs/SESSION_MEMORY.md for the GPS antenna investigation this
 * replaces the scratchpad multi-baud probe with.
 */

#define UART_PORT UART_NUM_1
#define PIN_RX 18
#define PIN_TX 19
#define BAUD_RATE 9600
#define RX_BUF_SIZE 2048
#define LINE_BUF_SIZE 128
#define CHUNK_SIZE 128

static const char *TAG = "nmea";

void app_main(void)
{
    uart_config_t cfg = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "gps_antenna_test ready: RX=GPIO%d TX=GPIO%d %d 8N1", PIN_RX, PIN_TX, BAUD_RATE);

    static char line[LINE_BUF_SIZE];
    static uint8_t chunk[CHUNK_SIZE];
    size_t line_len = 0;

    while (1) {
        int n = uart_read_bytes(UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    if (line[0] == '$') {
                        ESP_LOGI(TAG, "%s", line);
                    }
                    line_len = 0;
                }
                continue;
            }
            if (line_len < LINE_BUF_SIZE - 1) {
                line[line_len++] = c;
            } else {
                line_len = 0; /* oversized/malformed line, drop and resync */
            }
        }
    }
}

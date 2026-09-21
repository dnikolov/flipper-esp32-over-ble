#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

/*
 * Persistent GPS antenna smoke-test firmware. Listens at the confirmed
 * 9600 8N1 rate (UART1, RX=GPIO18, TX=GPIO19) and logs each complete NMEA
 * sentence as its own ESP_LOGI line under tag "nmea", so
 * tools/test_gps_antenna.ps1 can parse the monitor stream without dealing
 * with hex dumps. This inspection image polls UBX-CFG-GNSS and reports the
 * current constellation configuration before it switches to the normal
 * NMEA logging loop. It does not write receiver settings.
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
static const char *TTFF_TAG = "ttff";

static bool read_ubx_cfg_gnss(uint8_t *payload, size_t payload_cap, size_t *payload_len, uint32_t timeout_ms)
{
    static const uint8_t poll_frame[] = {0xB5, 0x62, 0x06, 0x3E, 0x00, 0x00, 0x44, 0x88};
    uint8_t frame[128];
    size_t frame_len = 0;
    uint32_t started_ms;

    uart_flush(UART_PORT);
    if (uart_write_bytes(UART_PORT, poll_frame, sizeof(poll_frame)) != (int)sizeof(poll_frame)) {
        ESP_LOGE(TTFF_TAG, "uart_write_bytes failed to send CFG-GNSS poll frame");
        return false;
    }
    ESP_ERROR_CHECK(uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(1000)));
    started_ms = (uint32_t)(esp_timer_get_time() / 1000);

    while ((uint32_t)(esp_timer_get_time() / 1000) - started_ms < timeout_ms) {
        uint8_t byte;

        if (uart_read_bytes(UART_PORT, &byte, 1, pdMS_TO_TICKS(100)) != 1) {
            continue;
        }
        if (frame_len == 0 && byte != 0xB5) {
            continue;
        }
        if (frame_len == 1 && byte != 0x62) {
            frame_len = 0;
            continue;
        }
        if (frame_len >= sizeof(frame)) {
            frame_len = 0;
            continue;
        }
        frame[frame_len++] = byte;
        if (frame_len < 8) {
            continue;
        }
        if (frame_len == 8) {
            uint16_t payload_len = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
            if (payload_len > sizeof(frame) - 8 || payload_len > payload_cap) {
                frame_len = 0;
            }
            continue;
        }
        {
            uint16_t frame_payload_len = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
            if (frame_len < (size_t)frame_payload_len + 8u) {
                continue;
            }
            if (frame_len == (size_t)frame_payload_len + 8u && frame[2] == 0x06 &&
                frame[3] == 0x3E) {
                uint8_t ck_a = 0;
                uint8_t ck_b = 0;
                size_t i;

                for (i = 2; i < frame_len - 2; i++) {
                    ck_a = (uint8_t)(ck_a + frame[i]);
                    ck_b = (uint8_t)(ck_b + ck_a);
                }
                if (ck_a == frame[frame_len - 2] && ck_b == frame[frame_len - 1]) {
                    memcpy(payload, &frame[6], frame_payload_len);
                    *payload_len = frame_payload_len;
                    return true;
                }
            }
            frame_len = 0;
        }
    }
    return false;
}

static void inspect_gnss(void)
{
    uint8_t payload[96];
    size_t payload_len = 0;
    size_t offset;

    ESP_LOGI(TAG, "reading current GPS CFG-GNSS; no settings will be changed");
    if (!read_ubx_cfg_gnss(payload, sizeof(payload), &payload_len, 1000)) {
        ESP_LOGE(TAG, "GPS did not return CFG-GNSS");
        return;
    }
    if (payload_len < 4 || payload_len != 4u + (size_t)payload[3] * 8u) {
        ESP_LOGE(TAG, "GPS returned malformed CFG-GNSS payload (%u bytes)", (unsigned)payload_len);
        return;
    }
    ESP_LOGI(TAG, "CFG-GNSS version=%u hardware_channels=%u used_channels=%u blocks=%u",
             payload[0], payload[1], payload[2], payload[3]);
    for (offset = 4; offset < payload_len; offset += 8) {
        uint32_t flags = (uint32_t)payload[offset + 4] |
                         ((uint32_t)payload[offset + 5] << 8) |
                         ((uint32_t)payload[offset + 6] << 16) |
                         ((uint32_t)payload[offset + 7] << 24);
        ESP_LOGI(TAG, "CFG-GNSS block id=%u %s reserved=%u max=%u flags=0x%08lx",
                 payload[offset], (flags & 1u) ? "enabled" : "disabled",
                 payload[offset + 1], payload[offset + 2], (unsigned long)flags);
    }
}

static bool nmea_hex_nibble(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(c - 'A' + 10);
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
        return true;
    }
    return false;
}

static bool nmea_checksum_ok(const char *sentence)
{
    const char *star = strchr(sentence, '*');
    uint8_t hi;
    uint8_t lo;
    uint8_t sum = 0;
    const char *p;

    if (!star || star == sentence || star[1] == '\0' || star[2] == '\0') {
        return false;
    }
    if (!nmea_hex_nibble(star[1], &hi) || !nmea_hex_nibble(star[2], &lo)) {
        return false;
    }
    for (p = sentence + 1; p < star; p++) {
        sum ^= (uint8_t)*p;
    }
    return sum == (uint8_t)((hi << 4) | lo);
}

static bool nmea_field(const char *sentence, int index, char *out, size_t out_cap)
{
    const char *p = sentence;
    const char *end;
    size_t field_len;
    int cur = 0;

    while (cur < index) {
        p = strchr(p, ',');
        if (!p) {
            return false;
        }
        p++;
        cur++;
    }
    end = p;
    while (*end != '\0' && *end != ',' && *end != '*') {
        end++;
    }
    field_len = (size_t)(end - p);
    if (field_len >= out_cap) {
        field_len = out_cap - 1;
    }
    memcpy(out, p, field_len);
    out[field_len] = '\0';
    return true;
}

static void check_ttff(const char *sentence, int64_t t0, bool *fix_logged)
{
    char sentence_id[8];
    char quality_str[8];
    char sats_str[8];
    char hdop_str[16];
    size_t id_len;
    int quality;

    if (*fix_logged || !nmea_checksum_ok(sentence)) {
        return;
    }
    if (!nmea_field(sentence, 0, sentence_id, sizeof(sentence_id))) {
        return;
    }
    id_len = strlen(sentence_id);
    if (id_len < 3 || strcmp(sentence_id + id_len - 3, "GGA") != 0) {
        return;
    }
    if (!nmea_field(sentence, 6, quality_str, sizeof(quality_str)) || quality_str[0] == '\0') {
        return;
    }
    quality = atoi(quality_str);
    if (quality <= 0) {
        return;
    }
    if (!nmea_field(sentence, 7, sats_str, sizeof(sats_str)) || sats_str[0] == '\0') {
        strcpy(sats_str, "0");
    }
    if (!nmea_field(sentence, 8, hdop_str, sizeof(hdop_str)) || hdop_str[0] == '\0') {
        strcpy(hdop_str, "?");
    }
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TTFF_TAG, "first fix at t+%lums sats=%u hdop=%s", (unsigned long)elapsed_ms,
             (unsigned int)atoi(sats_str), hdop_str);
    *fix_logged = true;
}

void app_main(void)
{
    int64_t t0 = esp_timer_get_time();
    bool fix_logged = false;

    ESP_LOGI(TTFF_TAG, "t0 anchor captured; TTFF timer started");

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
    inspect_gnss();

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
                        check_ttff(line, t0, &fix_logged);
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

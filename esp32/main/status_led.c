#include <stdbool.h>
#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "status_led.h"

static const char *TAG = "feb_status_led";

#define FEB_STATUS_LED_GPIO GPIO_NUM_8
#define FEB_STATUS_LED_RESOLUTION_HZ 10000000u

/* Onboard LED is an addressable WS2812 (confirmed in
   docs/hardware/esp32-c6-devkitc-1/source/guide/user_guide.rst: "RGB LED - Addressable RGB
   LED, driven by GPIO8" plus its WS2812-driving-circuit note), not a plain on/off GPIO --
   gpio_set_level() on GPIO8 would not reliably produce a visible color. Driven here via the
   core RMT TX driver with a byte-serializing callback encoder, following the pattern in
   $IDF_PATH/examples/peripherals/rmt/led_strip_simple_encoder (no external managed
   component/network fetch required). Timings are the common WS2812 T0H/T0L/T1H/T1L values in
   0.1us ticks at the 10MHz resolution below. */
static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 3,
    .level1 = 0,
    .duration1 = 9,
};
static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 9,
    .level1 = 0,
    .duration1 = 3,
};
static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = 250,
    .level1 = 0,
    .duration1 = 250,
};

static rmt_channel_handle_t led_channel;
static rmt_encoder_handle_t led_encoder;
static bool led_ready;

static feb_status_led_state_t current_state = FEB_STATUS_LED_CONNECTING;
static bool factory_reset_active;
static bool blink_on;
static bool wardriving_active;

static size_t led_encoder_callback(const void *data, size_t data_size,
                                   size_t symbols_written, size_t symbols_free,
                                   rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t byte_index;
    size_t symbol_pos;
    int bitmask;

    (void)arg;
    if (symbols_free < 8) {
        return 0;
    }
    byte_index = symbols_written / 8;
    if (byte_index < data_size) {
        symbol_pos = 0;
        for (bitmask = 0x80; bitmask != 0; bitmask >>= 1) {
            symbols[symbol_pos++] = (bytes[byte_index] & bitmask) ? ws2812_one : ws2812_zero;
        }
        return symbol_pos;
    }
    symbols[0] = ws2812_reset;
    *done = true;
    return 1;
}

static void led_init(void)
{
    rmt_tx_channel_config_t chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = FEB_STATUS_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = FEB_STATUS_LED_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    rmt_simple_encoder_config_t encoder_config = {
        .callback = led_encoder_callback,
    };
    esp_err_t err;

    err = rmt_new_tx_channel(&chan_config, &led_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s; status LED disabled",
                 esp_err_to_name(err));
        return;
    }
    err = rmt_new_simple_encoder(&encoder_config, &led_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_simple_encoder failed: %s; status LED disabled",
                 esp_err_to_name(err));
        rmt_del_channel(led_channel);
        led_channel = NULL;
        return;
    }
    err = rmt_enable(led_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s; status LED disabled",
                 esp_err_to_name(err));
        rmt_del_encoder(led_encoder);
        led_encoder = NULL;
        rmt_del_channel(led_channel);
        led_channel = NULL;
        return;
    }
    led_ready = true;
}

void feb_ws2812_set(uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t pixel[3] = {green, red, blue}; /* WS2812 wire byte order is GRB. */
    rmt_transmit_config_t tx_config = {.loop_count = 0};

    if (!led_ready) {
        return;
    }
    /* Fire-and-forget: callers include feb_status_led_tick(), invoked from
       reassembly_timeout_cb() on NimBLE's own host event queue. Blocking here for
       rmt_tx_wait_all_done() (as this used to, when the only caller was factory_reset.c's
       own dedicated task) stalls that shared queue -- observed to delay BLE handshake
       processing enough to break runtime auth. trans_queue_depth=4 on the channel means a
       new transmit safely queues behind one still in flight without this wait. */
    rmt_transmit(led_channel, led_encoder, pixel, sizeof(pixel), &tx_config);
}

static void apply_current_state(void)
{
    if (factory_reset_active) {
        return;
    }
    switch (current_state) {
    case FEB_STATUS_LED_CONNECTED:
        feb_ws2812_set(wardriving_active ? 24 : 0, 0, 32);
        break;
    case FEB_STATUS_LED_FLUSHING:
        feb_ws2812_set(0, 32, 0);
        break;
    case FEB_STATUS_LED_CONNECTING:
    default:
        feb_ws2812_set(wardriving_active ? 24 : 0, 0, 32);
        blink_on = true;
        break;
    }
}

void feb_status_led_init(void)
{
    led_init();
    apply_current_state();
}

void feb_status_led_set(feb_status_led_state_t state)
{
    current_state = state;
    apply_current_state();
}

void feb_status_led_set_wardriving_active(bool active)
{
    wardriving_active = active;
    apply_current_state();
}

void feb_status_led_tick(void)
{
    if (factory_reset_active || current_state != FEB_STATUS_LED_CONNECTING) {
        return;
    }
    blink_on = !blink_on;
    feb_ws2812_set((blink_on && wardriving_active) ? 24 : 0, 0, blink_on ? 32 : 0);
}

void feb_status_led_factory_reset_begin(void)
{
    factory_reset_active = true;
}

void feb_status_led_factory_reset_end(void)
{
    factory_reset_active = false;
    feb_status_led_set(current_state);
}

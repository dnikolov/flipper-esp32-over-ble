#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "factory_reset.h"

static const char *TAG = "feb_factory_reset";

#define FEB_FACTORY_RESET_BOOT_GPIO GPIO_NUM_9
#define FEB_FACTORY_RESET_LED_GPIO GPIO_NUM_8
#define FEB_FACTORY_RESET_HOLD_MS 5000u
#define FEB_FACTORY_RESET_POLL_MS 50u
#define FEB_FACTORY_RESET_BLINK_MS 200u
#define FEB_FACTORY_RESET_LED_RESOLUTION_HZ 10000000u

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
        .gpio_num = FEB_FACTORY_RESET_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = FEB_FACTORY_RESET_LED_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    rmt_simple_encoder_config_t encoder_config = {
        .callback = led_encoder_callback,
    };
    esp_err_t err;

    err = rmt_new_tx_channel(&chan_config, &led_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s; factory-reset LED feedback disabled",
                 esp_err_to_name(err));
        return;
    }
    err = rmt_new_simple_encoder(&encoder_config, &led_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_simple_encoder failed: %s; factory-reset LED feedback disabled",
                 esp_err_to_name(err));
        rmt_del_channel(led_channel);
        led_channel = NULL;
        return;
    }
    err = rmt_enable(led_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s; factory-reset LED feedback disabled",
                 esp_err_to_name(err));
        rmt_del_encoder(led_encoder);
        led_encoder = NULL;
        rmt_del_channel(led_channel);
        led_channel = NULL;
        return;
    }
    led_ready = true;
}

static void led_set(uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t pixel[3] = {green, red, blue}; /* WS2812 wire byte order is GRB. */
    rmt_transmit_config_t tx_config = {.loop_count = 0};

    if (!led_ready) {
        return;
    }
    if (rmt_transmit(led_channel, led_encoder, pixel, sizeof(pixel), &tx_config) != ESP_OK) {
        return;
    }
    rmt_tx_wait_all_done(led_channel, pdMS_TO_TICKS(50));
}

static void perform_factory_reset(void)
{
    esp_err_t err;

    ESP_LOGW(TAG, "factory-reset gesture confirmed (BOOT held %u ms); erasing NVS and restarting",
             (unsigned)FEB_FACTORY_RESET_HOLD_MS);
    led_set(0, 0, 0);
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
    }
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init after erase failed: %s", esp_err_to_name(err));
    }
    feb_wipe_pairing_secrets();
    /* No new post-erase path (docs/PLAN.md): esp_restart() falls straight into the existing
       app_main() boot logic, which finds no stored pairing_secret and opens a pairing
       window, reused verbatim. */
    esp_restart();
}

static void factory_reset_task(void *arg)
{
    bool held = false;
    bool led_on = false;
    uint32_t hold_start_ms = 0;
    uint32_t last_blink_ms = 0;

    (void)arg;
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool pressed = gpio_get_level(FEB_FACTORY_RESET_BOOT_GPIO) == 0;

        if (pressed) {
            if (!held) {
                held = true;
                hold_start_ms = now_ms;
                last_blink_ms = now_ms;
                led_on = true;
                led_set(32, 0, 0);
            } else if (now_ms - hold_start_ms >= FEB_FACTORY_RESET_HOLD_MS) {
                perform_factory_reset();
            } else if (now_ms - last_blink_ms >= FEB_FACTORY_RESET_BLINK_MS) {
                last_blink_ms = now_ms;
                led_on = !led_on;
                led_set(led_on ? 32 : 0, 0, 0);
            }
        } else if (held) {
            /* Early release: abort silently, no erase, LED off, no other signal
               (docs/PLAN.md). */
            held = false;
            led_set(0, 0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(FEB_FACTORY_RESET_POLL_MS));
    }
}

void feb_factory_reset_start(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << FEB_FACTORY_RESET_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config for BOOT button failed: %s; factory-reset gesture disabled",
                 esp_err_to_name(err));
        return;
    }
    led_init();
    if (xTaskCreate(factory_reset_task, "factory_reset", 2560, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start factory-reset monitor task");
    }
}

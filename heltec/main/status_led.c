#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "status_led.h"

static const char *TAG = "feb_status_led";

#define FEB_STATUS_LED_GPIO GPIO_NUM_25

/* Cadence, in units of feb_status_led_tick() calls (piggybacked on
   reassembly_timeout_cb(), i.e. roughly every FEB_REASSEMBLY_CHECK_INTERVAL_MS -- see
   main.c): CONNECTING toggles every FEB_STATUS_LED_CONNECTING_TICKS ticks (slow blink),
   FLUSHING toggles every tick (fast blink). Judgment call, not a protocol detail -- see
   docs/PLAN.md Phase 4 step 3's brief. */
#define FEB_STATUS_LED_CONNECTING_TICKS 4u

static feb_status_led_state_t current_state = FEB_STATUS_LED_CONNECTING;
static bool factory_reset_active;
static bool led_on;
static uint32_t connecting_tick_count;
/* See status_led.h's comment on feb_status_led_set_wardriving_active(): recorded for call-site
   parity with the C6 build, not currently read by anything in this file. */
static bool wardriving_active;

void feb_led_set(bool on)
{
    led_on = on;
    gpio_set_level(FEB_STATUS_LED_GPIO, on ? 1 : 0);
}

static void apply_current_state(void)
{
    if (factory_reset_active) {
        return;
    }
    switch (current_state) {
    case FEB_STATUS_LED_CONNECTED:
        feb_led_set(true);
        break;
    case FEB_STATUS_LED_FLUSHING:
        feb_led_set(true);
        break;
    case FEB_STATUS_LED_CONNECTING:
    default:
        feb_led_set(false);
        connecting_tick_count = 0;
        break;
    }
}

void feb_status_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << FEB_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config for status LED failed: %s; status LED disabled",
                 esp_err_to_name(err));
        return;
    }
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
}

void feb_status_led_tick(void)
{
    if (factory_reset_active) {
        return;
    }
    switch (current_state) {
    case FEB_STATUS_LED_CONNECTING: {
        /* Blinks twice as fast while a wardriving capture is active, the closest this
           board's single on/off LED can get to the C6's blue-vs-purple color distinction
           (see status_led.h's comment on feb_status_led_set_wardriving_active()). */
        uint32_t threshold = wardriving_active ?
            (FEB_STATUS_LED_CONNECTING_TICKS / 2u) : FEB_STATUS_LED_CONNECTING_TICKS;

        connecting_tick_count++;
        if (connecting_tick_count >= threshold) {
            connecting_tick_count = 0;
            feb_led_set(!led_on);
        }
        break;
    }
    case FEB_STATUS_LED_FLUSHING:
        feb_led_set(!led_on);
        break;
    case FEB_STATUS_LED_CONNECTED:
    default:
        break;
    }
}

void feb_status_led_factory_reset_begin(void)
{
    factory_reset_active = true;
}

void feb_status_led_factory_reset_end(void)
{
    factory_reset_active = false;
    apply_current_state();
}

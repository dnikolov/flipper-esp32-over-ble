#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "status_led.h"

static const char *TAG = "feb_status_led";

/* docs/hardware/olimex-mod-esp32-c5/README.md: USER_LED1 (green) / USER_LED2 (red), both
   plain digital LEDs via 2.2k series resistors, both also boot-mode strapping pins -- see
   status_led.h's top-of-file comment for the two-LED mapping this file implements. */
#define FEB_STATUS_LED_GREEN_GPIO GPIO_NUM_27
#define FEB_STATUS_LED_RED_GPIO GPIO_NUM_26

/* Cadence, in units of feb_status_led_tick() calls (piggybacked on reassembly_timeout_cb(),
   i.e. roughly every FEB_REASSEMBLY_CHECK_INTERVAL_MS -- see main.c): CONNECTING toggles green
   every FEB_STATUS_LED_CONNECTING_TICKS ticks (slow blink), FLUSHING toggles red every tick
   (fast blink). Same judgment call as the Heltec/C6 ports, not a protocol detail. */
#define FEB_STATUS_LED_CONNECTING_TICKS 4u

static feb_status_led_state_t current_state = FEB_STATUS_LED_CONNECTING;
static bool green_on;
static bool red_on;
static uint32_t connecting_tick_count;
/* See status_led.h's comment on feb_status_led_set_wardriving_active(): recorded for
   call-site parity with the C6/Heltec builds, not currently read by anything in this file. */
static bool wardriving_active;

static void feb_led_set_green(bool on)
{
    green_on = on;
    gpio_set_level(FEB_STATUS_LED_GREEN_GPIO, on ? 1 : 0);
}

static void feb_led_set_red(bool on)
{
    red_on = on;
    gpio_set_level(FEB_STATUS_LED_RED_GPIO, on ? 1 : 0);
}

static void apply_current_state(void)
{
    switch (current_state) {
    case FEB_STATUS_LED_CONNECTED:
        feb_led_set_green(true);
        break;
    case FEB_STATUS_LED_FLUSHING:
        /* Green is left as-is -- FLUSHING is only ever entered from an authenticated session,
           so green should already be solid on; only red starts its blink here. */
        feb_led_set_red(true);
        break;
    case FEB_STATUS_LED_CONNECTING:
    default:
        feb_led_set_green(false);
        feb_led_set_red(false);
        connecting_tick_count = 0;
        break;
    }
}

void feb_status_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FEB_STATUS_LED_GREEN_GPIO) | (1ULL << FEB_STATUS_LED_RED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config for status LEDs failed: %s; status LEDs disabled",
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
    switch (current_state) {
    case FEB_STATUS_LED_CONNECTING:
        connecting_tick_count++;
        if (connecting_tick_count >= FEB_STATUS_LED_CONNECTING_TICKS) {
            connecting_tick_count = 0;
            feb_led_set_green(!green_on);
        }
        break;
    case FEB_STATUS_LED_FLUSHING:
        feb_led_set_red(!red_on);
        break;
    case FEB_STATUS_LED_CONNECTED:
    default:
        break;
    }
}

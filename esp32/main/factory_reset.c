#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "factory_reset.h"
#include "status_led.h"

static const char *TAG = "feb_factory_reset";

#define FEB_FACTORY_RESET_BOOT_GPIO GPIO_NUM_9
#define FEB_FACTORY_RESET_HOLD_MS 5000u
#define FEB_FACTORY_RESET_POLL_MS 50u
#define FEB_FACTORY_RESET_BLINK_MS 200u
/* A release is treated as a wardriving on/off toggle press only within this window: at
   least two poll ticks (rejects a single-tick electrical-bounce read, which
   `press_ms >= FEB_FACTORY_RESET_POLL_MS` alone can't -- one tick of "pressed" already
   guarantees press_ms >= FEB_FACTORY_RESET_POLL_MS by construction, so that comparison
   never actually rejected anything), and short enough that it can't be an aborted
   factory-reset hold (a user releasing at, say, 4s to back out of the reset gesture must
   not also silently toggle wardriving). Anything released between MAX_MS and
   FEB_FACTORY_RESET_HOLD_MS is treated as "gesture aborted, no action" -- not a toggle. */
#define FEB_WARDRIVING_TOGGLE_MIN_MS (2u * FEB_FACTORY_RESET_POLL_MS)
#define FEB_WARDRIVING_TOGGLE_MAX_MS 1000u

static void perform_factory_reset(void)
{
    esp_err_t err;

    ESP_LOGW(TAG, "factory-reset gesture confirmed (BOOT held %u ms); erasing NVS and restarting",
             (unsigned)FEB_FACTORY_RESET_HOLD_MS);
    feb_ws2812_set(0, 0, 0);
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
                feb_status_led_factory_reset_begin();
                feb_ws2812_set(32, 0, 0);
            } else if (now_ms - hold_start_ms >= FEB_FACTORY_RESET_HOLD_MS) {
                perform_factory_reset();
            } else if (now_ms - last_blink_ms >= FEB_FACTORY_RESET_BLINK_MS) {
                last_blink_ms = now_ms;
                led_on = !led_on;
                feb_ws2812_set(led_on ? 32 : 0, 0, 0);
            }
        } else if (held) {
            /* Early release: abort the factory-reset gesture, no erase, hand the LED back
               to the real connection-status state (docs/PLAN.md). Only a release inside
               [FEB_WARDRIVING_TOGGLE_MIN_MS, FEB_WARDRIVING_TOGGLE_MAX_MS) is treated as a
               deliberate short press that toggles wardriving; a bounce-length blip or a
               long-but-aborted reset hold does nothing (see those constants' comment). */
            uint32_t press_ms = now_ms - hold_start_ms;

            held = false;
            feb_status_led_factory_reset_end();
            if (press_ms >= FEB_WARDRIVING_TOGGLE_MIN_MS && press_ms < FEB_WARDRIVING_TOGGLE_MAX_MS) {
                feb_wardriving_request_button_toggle();
            }
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
    if (xTaskCreate(factory_reset_task, "factory_reset", 2560, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start factory-reset monitor task");
    }
}

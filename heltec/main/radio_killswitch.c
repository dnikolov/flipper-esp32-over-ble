#include <stdint.h>

#include "driver/touch_pad.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "radio_killswitch.h"

static const char *TAG = "feb_radio_ks";

#define FEB_RADIO_KS_TOUCH_CHANNEL TOUCH_PAD_NUM2
#define FEB_RADIO_KS_CALIBRATION_SAMPLES 16u
#define FEB_RADIO_KS_CALIBRATION_SAMPLE_MS 20u
/* Classic-ESP32 touch counters get smaller when touched (larger equivalent capacitance) --
   a reading below this fraction of the boot-time no-touch baseline is read as "pressed". Not
   a datasheet constant: raw counts are highly board/environment dependent, hence the fresh
   calibration pass below rather than a hardcoded threshold. */
#define FEB_RADIO_KS_THRESHOLD_PERCENT 70u
#define FEB_RADIO_KS_POLL_MS 50u
/* Same debounce shape as factory_reset.c's FEB_WARDRIVING_TOGGLE_MIN_MS/MAX_MS (reused
   deliberately, see radio_killswitch.h's top comment): rejects both a capacitive-noise blip
   and a finger left resting on the pad from being read as a deliberate toggle gesture. */
#define FEB_RADIO_KS_TOUCH_MIN_MS (2u * FEB_RADIO_KS_POLL_MS)
#define FEB_RADIO_KS_TOUCH_MAX_MS 1000u

#define FEB_RADIO_NVS_NAMESPACE "feb_radio"
#define FEB_RADIO_NVS_KEY "on"

bool feb_radio_kill_switch_load_persisted(void)
{
    nvs_handle_t handle;
    uint8_t value = 1u;
    esp_err_t err = nvs_open(FEB_RADIO_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        return true;
    }
    err = nvs_get_u8(handle, FEB_RADIO_NVS_KEY, &value);
    nvs_close(handle);
    if (err != ESP_OK) {
        return true;
    }
    return value != 0u;
}

void feb_radio_kill_switch_persist(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FEB_RADIO_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s; radio kill-switch state not persisted",
                 esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(handle, FEB_RADIO_NVS_KEY, enabled ? 1u : 0u);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist radio kill-switch state: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static void radio_kill_switch_task(void *arg)
{
    bool touched = false;
    uint32_t touch_start_ms = 0;
    uint16_t baseline;
    uint16_t threshold;
    esp_err_t err;

    (void)arg;

    err = touch_pad_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_init failed: %s; radio kill-switch disabled",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_set_voltage(TOUCH_PAD_HIGH_VOLTAGE_THRESHOLD, TOUCH_PAD_LOW_VOLTAGE_THRESHOLD,
                          TOUCH_PAD_ATTEN_VOLTAGE_THRESHOLD);
    touch_pad_config(FEB_RADIO_KS_TOUCH_CHANNEL, 0);
    /* Deliberately touch_pad_read() (a single on-demand hardware measurement), not
       touch_pad_filter_start()+touch_pad_read_filtered() -- this board's DRAM/IRAM budget is
       already down to single-digit/low-hundreds bytes of headroom (docs/BACKLOG.md BL23/BL24),
       and pulling in the touch driver's IIR filter subsystem (a background FreeRTOS timer, its
       own filter struct, and two IRAM_ATTR-placed functions this firmware would otherwise never
       reference) overflowed both segments on a real idf.py build. A plain unfiltered read plus
       this poll loop's own debounce window is noisier but sufficient for a deliberate,
       once-in-a-while gesture, not a fast/precise slider. */

    {
        uint32_t sum = 0;
        unsigned int valid_samples = 0;
        unsigned int index;

        for (index = 0; index < FEB_RADIO_KS_CALIBRATION_SAMPLES; index++) {
            uint16_t sample;

            if (touch_pad_read(FEB_RADIO_KS_TOUCH_CHANNEL, &sample) == ESP_OK) {
                sum += sample;
                valid_samples++;
            }
            vTaskDelay(pdMS_TO_TICKS(FEB_RADIO_KS_CALIBRATION_SAMPLE_MS));
        }
        if (valid_samples == 0u) {
            ESP_LOGE(TAG, "touch calibration got no valid samples; radio kill-switch disabled");
            vTaskDelete(NULL);
            return;
        }
        baseline = (uint16_t)(sum / valid_samples);
    }
    threshold = (uint16_t)((uint32_t)baseline * FEB_RADIO_KS_THRESHOLD_PERCENT / 100u);
    ESP_LOGI(TAG, "touch kill-switch calibrated: baseline=%u threshold=%u", baseline, threshold);

    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint16_t value = baseline;
        bool pressed;

        (void)touch_pad_read(FEB_RADIO_KS_TOUCH_CHANNEL, &value);
        pressed = value < threshold;

        if (pressed) {
            if (!touched) {
                touched = true;
                touch_start_ms = now_ms;
            }
        } else if (touched) {
            uint32_t press_ms = now_ms - touch_start_ms;

            touched = false;
            if (press_ms >= FEB_RADIO_KS_TOUCH_MIN_MS && press_ms < FEB_RADIO_KS_TOUCH_MAX_MS) {
                feb_radio_kill_switch_toggle();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(FEB_RADIO_KS_POLL_MS));
    }
}

void feb_radio_kill_switch_start(void)
{
    if (xTaskCreate(radio_kill_switch_task, "radio_ks", 3072, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start radio kill-switch monitor task");
    }
}

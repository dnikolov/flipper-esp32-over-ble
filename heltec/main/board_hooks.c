#include "feb_app_internal.h"
#include "board_hooks.h"
#include "factory_reset.h"

/* A plain ble_npl_event, not a ble_npl_callout -- see esp32/main/main.c's fuller comment on
   why (ESP-IDF's NimBLE FreeRTOS port caps the host-side callout pool at a hard-coded 8
   slots, already close to exhausted by this file's other callouts once wardriving_wifi/
   ble_interval_co are added). */
static struct ble_npl_event wardriving_button_toggle_ev;
/* Same "raw event, not a callout" shape as wardriving_button_toggle_ev, same reason (host-side
   callout pool budget) -- HARDENING_PLAN.md HP-13: serializes the factory-reset gesture's NVS
   erase against every other NVS writer in this firmware (persist_pairing_secret(),
   wardriving_persist_save(), the radio kill-switch's own persist), which previously ran
   uncoordinated on factory_reset_task()'s own polling task. */
static struct ble_npl_event factory_reset_ev;
static volatile bool wardriving_control_ready;
static volatile uint32_t wardriving_button_toggle_pending;
static void wardriving_button_toggle_cb(struct ble_npl_event *ev);
static void factory_reset_cb(struct ble_npl_event *ev);

/* Runs on the NimBLE host task (posted via wardriving_button_toggle_ev). Ported unchanged
   from esp32/main/main.c -- see that file's fuller comment (and factory_reset.c's
   FEB_WARDRIVING_TOGGLE_MIN_MS/MAX_MS, ported alongside this for the same GPIO0 button). */
static void wardriving_button_toggle_cb(struct ble_npl_event *ev)
{
    uint32_t pending = __atomic_exchange_n(&wardriving_button_toggle_pending, 0, __ATOMIC_SEQ_CST);

    (void)ev;
    for (; pending > 0; pending--) {
        if (feb_wardriving_wifi_active || feb_wardriving_ble_active) {
            feb_wardriving_stop_internal();
            feb_wardriving_persisted.enabled = false;
            wardriving_persist_save(&feb_wardriving_persisted);
            ESP_LOGI(TAG, "wardriving stopped via boot-button toggle");
            continue;
        }
        if (feb_wardriving_start_internal(feb_wardriving_persisted.want_wifi, feb_wardriving_persisted.want_ble,
                                      feb_wardriving_persisted.want_ble_passive,
                                      feb_wardriving_persisted.wifi_interval_ms,
                                      feb_wardriving_persisted.ble_window_ms,
                                      feb_wardriving_persisted.ble_interval_ms,
                                      (wardriving_swelling_mode_t)feb_wardriving_persisted.wifi_swelling,
                                      (wardriving_country_t)feb_wardriving_persisted.country)) {
            feb_wardriving_persisted.enabled = true;
            wardriving_persist_save(&feb_wardriving_persisted);
            ESP_LOGI(TAG, "wardriving started via boot-button toggle (wifi=%d ble=%d ble_passive=%d)",
                     (int)feb_wardriving_persisted.want_wifi, (int)feb_wardriving_persisted.want_ble,
                     (int)feb_wardriving_persisted.want_ble_passive);
        } else {
            ESP_LOGW(TAG, "wardriving boot-button toggle-on rejected (radio busy or start failed)");
        }
    }
}

/* Declared in factory_reset.h. Ported unchanged from esp32/main/main.c -- see that file's
   fuller comment. */
void feb_wardriving_request_button_toggle(void)
{
    if (!wardriving_control_ready) {
        ESP_LOGW(TAG, "boot-button press ignored: wardriving control not ready yet");
        return;
    }
    __atomic_fetch_add(&wardriving_button_toggle_pending, 1, __ATOMIC_SEQ_CST);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &wardriving_button_toggle_ev);
}

/* Runs on the NimBLE host task (posted via factory_reset_ev). HARDENING_PLAN.md HP-13. */
static void factory_reset_cb(struct ble_npl_event *ev)
{
    (void)ev;
    feb_factory_reset_perform();
}

/* Declared in factory_reset.h. HARDENING_PLAN.md HP-13: factory_reset_task() used to call
   feb_factory_reset_perform() (nvs_flash_erase()+nvs_flash_init()) directly from its own
   polling task, uncoordinated with persist_pairing_secret()/wardriving_persist_save()/the
   radio kill-switch's own NVS persist, all of which run on the NimBLE host task (or, for the
   kill-switch, radio_ks -- see feb_radio_kill_switch_persist()'s call sites). Serialized the
   same way wardriving's own boot-button toggle already is: hand off to the host task via a raw
   ble_npl_event, so the erase can never interleave with another NVS writer's own
   nvs_open()/nvs_set_*()/nvs_commit() sequence. Falls back to performing the reset inline when
   the host task isn't currently running its event loop (radio kill-switch persisted off, or
   still early in boot) -- there is no concurrent NVS writer to race in that state, and NVS
   itself has no reason to remain reachable only through the host task. */
void feb_factory_reset_request(void)
{
    if (!wardriving_control_ready) {
        feb_factory_reset_perform();
        return;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &factory_reset_ev);
}

/* Called from feb_board_host_synced() at the point feb_board_host_synced() used to init these two raw
   events inline. */
void feb_board_button_events_init(void)
{
    ble_npl_event_init(&wardriving_button_toggle_ev, wardriving_button_toggle_cb, NULL);
    ble_npl_event_init(&factory_reset_ev, factory_reset_cb, NULL);
}

void feb_board_control_ready_set(void)
{
    wardriving_control_ready = true;
}

void feb_board_control_ready_clear(void)
{
    wardriving_control_ready = false;
}

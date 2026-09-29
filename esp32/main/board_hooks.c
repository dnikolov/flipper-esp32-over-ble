#include "feb_app_internal.h"
#include "board_hooks.h"
#include "factory_reset.h"

/* A plain ble_npl_event, NOT a ble_npl_callout -- ESP-IDF's NimBLE FreeRTOS port caps the
   *entire application's* host-side callout (timer) pool at a hard-coded 8 slots
   (BLE_HOST_CO_COUNT in npl_os_freertos.c), shared with however many NimBLE's own internal
   host procedures need; this project's other 6 callouts (feb_reassembly_timeout_co,
   feb_wifi_scan_done_co, feb_ble_scan_done_co, feb_wardriving_wifi_interval_co,
   feb_wardriving_ble_interval_co, feb_reconnect_co) already sit close enough to that ceiling that
   a 7th (this one, in an earlier version of this feature) reliably overflowed the pool and
   crashed host_synced() with "assert failed: npl_freertos_callout_init" before scanning or
   pairing could ever start -- hardware-verified 2026-09-15. A callout's timer semantics
   were never actually needed here (feb_wardriving_request_button_toggle() always fires it
   at 0ms delay, i.e. "run on the host task ASAP"), so a raw event -- drawn from the much
   larger BLE_HOST_EV_COUNT (19) pool instead -- is the correct fix, not a workaround. */
static struct ble_npl_event wardriving_button_toggle_ev;
/* Same rationale as wardriving_button_toggle_ev above (raw event, not a callout) -- posted
   by feb_factory_reset_request() (HP-13) so the NVS erase runs on the NimBLE host task,
   serialized with persist_pairing_secret()/wardriving_persist_save(). */
static struct ble_npl_event factory_reset_ev;
/* Set once host_synced() has initialized wardriving_button_toggle_ev -- guards
   feb_wardriving_request_button_toggle() against a boot-button press landing before the
   NimBLE host task has finished starting up. Also guards feb_factory_reset_request(). */
static volatile bool wardriving_control_ready;
/* Count of button presses not yet applied, incremented by
   feb_wardriving_request_button_toggle() (any task) and drained by
   wardriving_button_toggle_cb() (NimBLE host task only). ble_npl_eventq_put() no-ops if
   wardriving_button_toggle_ev is already queued, so a second press landing before the
   first has been processed doesn't post a second event -- without this counter that
   second press would be silently lost instead of queuing a second toggle whenever the
   host task is busy (e.g. mid backlog-drain) when the button is pressed twice in quick
   succession. Accessed with GCC atomic builtins rather than a lock since it's a single
   word shared between exactly one writer-task-at-a-time pattern and one reader. */
static volatile uint32_t wardriving_button_toggle_pending;
static void wardriving_button_toggle_cb(struct ble_npl_event *ev);
static void factory_reset_perform_cb(struct ble_npl_event *ev);

/* Runs on the NimBLE host task (posted via wardriving_button_toggle_ev) -- armed via
   feb_wardriving_request_button_toggle() from factory_reset_task(), which is a different
   FreeRTOS task and must never touch wardriving_{wifi,ble}_active/feb_connection_handle
   directly (see feb_wardriving_self_stop()'s comment above for why). Toggles based on current
   state, reusing feb_wardriving_persisted's last-used sources/intervals to start. Drains
   wardriving_button_toggle_pending in one pass so no press queued while this callback (or
   an earlier invocation of it) was still running gets lost -- see that variable's comment. */
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
                                      /* wardriving_persist.h (version 2) now persists the last
                                         `start` command's wifi_swelling/country
                                         (feb_cap_wardriving_cmd.c) -- boot-button toggle-on
                                         reuses them instead of an unconfigured-radio default. */
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

/* Declared in factory_reset.h, defined here since it needs wardriving_button_toggle_ev,
   file-scope state owned by this translation unit. Safe to call from any task -- only
   increments a counter and posts an event; the actual toggle work happens on the NimBLE
   host task above. ble_npl_eventq_put() is itself a no-op if the event is already queued
   (npl_freertos_eventq_put()'s "if (event->queued) return;" guard) rather than an error,
   so calling it again while a prior press's toggle hasn't run yet is always safe -- and
   since factory_reset_task() is the only caller, this is also never racing itself across
   tasks the way a shared callout's re-arm would need to. */
void feb_wardriving_request_button_toggle(void)
{
    if (!wardriving_control_ready) {
        ESP_LOGW(TAG, "boot-button press ignored: wardriving control not ready yet");
        return;
    }
    __atomic_fetch_add(&wardriving_button_toggle_pending, 1, __ATOMIC_SEQ_CST);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &wardriving_button_toggle_ev);
}

/* Runs on the NimBLE host task (posted via factory_reset_ev) -- armed via
   feb_factory_reset_request() from factory_reset_task(), a different FreeRTOS task, so this
   is where the actual erase happens, serialized with persist_pairing_secret()/
   wardriving_persist_save() (HP-13; both of those also only ever run on this task). Never
   returns in practice: esp_restart() ends the process. */
static void factory_reset_perform_cb(struct ble_npl_event *ev)
{
    esp_err_t err;

    (void)ev;
    ESP_LOGW(TAG, "factory-reset: erasing NVS and restarting");
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

/* Declared in factory_reset.h, defined here since it needs factory_reset_ev/
   wardriving_control_ready, file-scope state owned by this translation unit. Safe to call
   from any task. Before the NimBLE host task has finished starting up there is nothing to
   serialize with yet (no other task can be mid-NVS-write that early in boot), so this falls
   back to performing the erase inline rather than posting an event nobody will ever drain. */
void feb_factory_reset_request(void)
{
    if (!wardriving_control_ready) {
        ESP_LOGW(TAG, "factory-reset requested before host task ready; erasing NVS directly");
        factory_reset_perform_cb(NULL);
        return;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &factory_reset_ev);
}

/* Called from host_synced() at the point it used to init these two raw events inline. */
void feb_board_button_events_init(void)
{
    ble_npl_event_init(&wardriving_button_toggle_ev, wardriving_button_toggle_cb, NULL);
    ble_npl_event_init(&factory_reset_ev, factory_reset_perform_cb, NULL);
}

void feb_board_control_ready_set(void)
{
    wardriving_control_ready = true;
}

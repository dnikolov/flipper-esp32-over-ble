#ifndef FEB_FACTORY_RESET_H
#define FEB_FACTORY_RESET_H

/* docs/PLAN.md Phase 4 step 3: ported from esp32/main/factory_reset.c onto this board's
   GPIO0 BOOT/PRG button (the C6 uses GPIO9 -- see docs/hardware/heltec-wifi-lora-32-v2/
   README.md). Holding the button for FEB_FACTORY_RESET_HOLD_MS continuous milliseconds
   erases the NVS partition and restarts, falling back into the existing no-stored-secret
   boot path (pairing window). Starts a low-priority background task; safe to call once from
   app_main() after gpio-owning peripherals are otherwise idle. A short release (see
   factory_reset.c's FEB_WARDRIVING_TOGGLE_MIN_MS/MAX_MS) toggles wardriving on/off, ported
   unchanged from esp32/main/factory_reset.c now that wardriving is ported on this board too. */
void feb_factory_reset_start(void);

/* Defined in main.c: zeroizes the in-RAM stored pairing secret plus any live
   pairing/session-auth scratch (reusing pairing_attempt_zeroize()/runtime_auth_zeroize()).
   Must be called before esp_restart() in the factory-reset path so a warm-boot crash
   between NVS erase and restart cannot leave the old secret sitting in SRAM. */
void feb_wipe_pairing_secrets(void);

/* Defined in main.c: hands a boot-button short press off to the wardriving on/off toggle,
   which only ever runs on the NimBLE host task -- this function itself just arms a
   ble_npl_event and is safe to call from factory_reset_task(). A no-op (logs a warning)
   if called before the NimBLE host task has finished its own startup. */
void feb_wardriving_request_button_toggle(void);

/* Defined in factory_reset.c: the actual NVS erase + restart (nvs_flash_erase()/
   nvs_flash_init()/feb_wipe_pairing_secrets()/esp_restart()). Call feb_factory_reset_request()
   below instead of this directly from factory_reset_task() -- exposed here only so main.c's
   host-task callback can invoke it once the request has been serialized (HARDENING_PLAN.md
   HP-13). */
void feb_factory_reset_perform(void);

/* Defined in main.c: hands the factory-reset gesture off to the NimBLE host task, the same
   way feb_wardriving_request_button_toggle() does, so the NVS erase inside
   feb_factory_reset_perform() can never interleave with another NVS writer's own
   nvs_open()/nvs_set_*()/nvs_commit() sequence (persist_pairing_secret(),
   wardriving_persist_save(), the radio kill-switch's persist). Falls back to calling
   feb_factory_reset_perform() directly when the host task isn't currently running (radio
   kill-switch persisted off) -- nothing there to race in that state. Safe to call from
   factory_reset_task(). */
void feb_factory_reset_request(void);

#endif

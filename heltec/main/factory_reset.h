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

#endif

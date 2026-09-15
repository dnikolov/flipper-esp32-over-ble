#ifndef FEB_FACTORY_RESET_H
#define FEB_FACTORY_RESET_H

/* docs/PLAN.md backlog "In-firmware, no-PC/no-session physical factory-reset gesture"
   (designed 2026-09-06). ESP32-only: no FAP/BLE involvement. Holding the onboard BOOT
   button (GPIO9) for FEB_FACTORY_RESET_HOLD_MS continuous milliseconds erases the NVS
   partition and restarts, falling back into the existing no-stored-secret boot path
   (pairing window). Starts a low-priority background task; safe to call once from
   app_main() after gpio/rmt-owning peripherals are otherwise idle. */
void feb_factory_reset_start(void);

/* Defined in main.c: zeroizes the in-RAM stored pairing secret plus any live
   pairing/session-auth scratch (reusing pairing_attempt_zeroize()/runtime_auth_zeroize()).
   Must be called before esp_restart() in the factory-reset path so a warm-boot crash
   between NVS erase and restart cannot leave the old secret sitting in SRAM. */
void feb_wipe_pairing_secrets(void);

/* Defined in main.c: hands a boot-button short press off to the wardriving on/off toggle,
   which only ever runs on the NimBLE host task -- this function itself just arms a
   ble_npl_callout and is safe to call from factory_reset_task(). A no-op (logs a warning)
   if called before the NimBLE host task has finished its own startup. */
void feb_wardriving_request_button_toggle(void);

#endif

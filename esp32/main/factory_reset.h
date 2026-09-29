#ifndef FEB_FACTORY_RESET_H
#define FEB_FACTORY_RESET_H

/* docs/PLAN.md backlog "In-firmware, no-PC/no-session physical factory-reset gesture"
   (designed 2026-09-06). ESP32-only: no FAP/BLE involvement. Holding the onboard BOOT
   button (GPIO9) for FEB_FACTORY_RESET_HOLD_MS continuous milliseconds erases the NVS
   partition and restarts, falling back into the existing no-stored-secret boot path
   (pairing window). Starts a low-priority background task; safe to call once from
   app_main() after gpio/rmt-owning peripherals are otherwise idle. */
void feb_factory_reset_start(void);

/* Defined in feb_app_core (feb_link.c): zeroizes the in-RAM stored pairing secret plus any live
   pairing/session-auth scratch (reusing feb_pairing_attempt_zeroize()/feb_runtime_auth_zeroize()).
   Called from board_hooks.c's factory-reset erase path before esp_restart(), so a warm-boot
   crash between NVS erase and restart cannot leave the old secret sitting in SRAM. */
void feb_wipe_pairing_secrets(void);

/* Defined in board_hooks.c: hands a boot-button short press off to the wardriving on/off toggle,
   which only ever runs on the NimBLE host task -- this function itself just arms a
   ble_npl_callout and is safe to call from factory_reset_task(). A no-op (logs a warning)
   if called before the NimBLE host task has finished its own startup. */
void feb_wardriving_request_button_toggle(void);

/* Defined in board_hooks.c: hands the confirmed factory-reset gesture off to the NimBLE host task
   (HP-13), which is where persist_pairing_secret()/wardriving_persist_save() also run their
   NVS writes -- nvs_flash_erase() de-inits the whole partition, so running it from
   factory_reset_task() (a separate FreeRTOS task) uncoordinated with those writers could
   erase mid-write or tear down a handle another task still holds open. This function itself
   only posts a ble_npl_event and is safe to call from factory_reset_task(); if the host task
   hasn't finished starting up yet (wardriving_control_ready still false), it performs the
   erase+restart directly instead, since nothing else can be mid-NVS-write that early in
   boot. Either way this call does not return: it always leads to esp_restart(). */
void feb_factory_reset_request(void);

#endif

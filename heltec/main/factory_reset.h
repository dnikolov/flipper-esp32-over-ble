#ifndef FEB_FACTORY_RESET_H
#define FEB_FACTORY_RESET_H

/* docs/PLAN.md Phase 4 step 3: ported from esp32/main/factory_reset.c onto this board's
   GPIO0 BOOT/PRG button (the C6 uses GPIO9 -- see docs/hardware/heltec-wifi-lora-32-v2/
   README.md). Holding the button for FEB_FACTORY_RESET_HOLD_MS continuous milliseconds
   erases the NVS partition and restarts, falling back into the existing no-stored-secret
   boot path (pairing window). Starts a low-priority background task; safe to call once from
   app_main() after gpio-owning peripherals are otherwise idle. Unlike the C6 version, there
   is no short-press wardriving toggle -- wardriving isn't ported on this board. */
void feb_factory_reset_start(void);

/* Defined in main.c: zeroizes the in-RAM stored pairing secret plus any live
   pairing/session-auth scratch (reusing pairing_attempt_zeroize()/runtime_auth_zeroize()).
   Must be called before esp_restart() in the factory-reset path so a warm-boot crash
   between NVS erase and restart cannot leave the old secret sitting in SRAM. */
void feb_wipe_pairing_secrets(void);

#endif

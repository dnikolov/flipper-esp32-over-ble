#ifndef FEB_FACTORY_RESET_H
#define FEB_FACTORY_RESET_H

/* docs/PLAN.md backlog "In-firmware, no-PC/no-session physical factory-reset gesture"
   (designed 2026-09-06). ESP32-only: no FAP/BLE involvement. Holding the onboard BOOT
   button (GPIO9) for FEB_FACTORY_RESET_HOLD_MS continuous milliseconds erases the NVS
   partition and restarts, falling back into the existing no-stored-secret boot path
   (pairing window). Starts a low-priority background task; safe to call once from
   app_main() after gpio/rmt-owning peripherals are otherwise idle. */
void feb_factory_reset_start(void);

#endif

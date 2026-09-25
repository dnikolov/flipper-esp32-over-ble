/* Connection-status indicator on this board's two plain (non-addressable) LEDs -- USER_LED1
   (GPIO27, green) and USER_LED2 (GPIO26, red); see docs/hardware/olimex-mod-esp32-c5/README.md.
   Unlike the C6's addressable WS2812 (single GPIO, color-encoded state) or the Heltec's single
   plain LED (blink-cadence-encoded state), this board has two independent plain GPIOs to spend,
   so the two dimensions this project already tracks -- "connection/session state" and "backlog
   actively flushing" -- get one LED each instead of being folded onto one via blink-rate or
   color, the same two states the C6 distinguishes with a blue-vs-purple color swap:

     - Green (LED1): connection/session state. Off/slow-blink while scanning or connected-but-
       not-yet-authenticated (FEB_STATUS_LED_CONNECTING), solid on once the runtime session is
       authenticated (FEB_STATUS_LED_CONNECTED).
     - Red (LED2): backlog-flushing activity (FEB_STATUS_LED_FLUSHING) -- fast blink while a
       backlog batch is actively draining to the Flipper, off otherwise. Wired up 2026-09-25
       (docs/PLAN.md Phase 8 Step 3) once `wardriving` was ported -- main.c's
       wardriving_maybe_kick_send()/wardriving_send_next_batch() drive this exactly like the
       C6/Heltec builds.

   Both GPIO27/GPIO26 are also boot-mode strapping pins (sampled at reset alongside GPIO28) --
   safe to drive as outputs post-boot (this module is only initialized from app_main(), well
   after reset-time sampling completes), but nothing here should ever run before boot. */
#ifndef FEB_STATUS_LED_H
#define FEB_STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FEB_STATUS_LED_CONNECTING = 0, /* scanning, or connected but not yet authenticated -- green slow blink */
    FEB_STATUS_LED_CONNECTED,      /* authenticated runtime session -- green solid on */
    FEB_STATUS_LED_FLUSHING,       /* backlog draining to the Flipper -- red fast blink */
} feb_status_led_state_t;

/* Configures GPIO27/GPIO26 as outputs and applies the initial FEB_STATUS_LED_CONNECTING state.
   Call once from app_main(), after boot-time strapping sampling has already completed. */
void feb_status_led_init(void);

/* Records the new connection-status state and applies it to the LEDs immediately. */
void feb_status_led_set(feb_status_led_state_t state);

/* Call-site parity with the C6/Heltec builds' wardriving_sync_status_led() (main.c) -- unlike
   those boards, which render an active wardriving capture as a color/blink-rate change on
   their single LED, this board's green LED is scoped (docs/hardware/olimex-mod-esp32-c5/
   README.md's "Status LED mapping") to exactly two states (CONNECTING/CONNECTED); a wardriving
   capture running or not has no third visual state reserved for it here. Deliberately a no-op
   today (recorded, not applied) rather than silently omitting the call main.c's shared
   wardriving code makes -- see docs/BACKLOG.md for the open question of whether a distinct
   wardriving-active indication should be added to this board's mapping later. */
void feb_status_led_set_wardriving_active(bool active);

/* Periodic tick (expected roughly every FEB_REASSEMBLY_CHECK_INTERVAL_MS, piggybacked on
   reassembly_timeout_cb()): advances the green CONNECTING blink / red FLUSHING blink cadence.
   No-op for CONNECTED (solid green). */
void feb_status_led_tick(void);

#endif

#ifndef FEB_STATUS_LED_H
#define FEB_STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

/* Connection-status indicator on the onboard plain LED (GPIO25, docs/hardware/
   heltec-wifi-lora-32-v2/README.md) -- unlike the C6's addressable WS2812 (GPIO8), this is a
   single on/off GPIO, so states are distinguished by blink cadence rather than color: slow
   blink while scanning/connected-but-unauthenticated, solid on once the runtime session is
   authenticated, fast blink while a wardriving-style backlog batch is actively flushing.
   There is no wardriving-active color variant (wardriving isn't ported on this board -- see
   docs/PLAN.md Phase 4 step 3). factory_reset.c suppresses this module's own redraws for the
   duration of its BOOT-hold gesture via _begin()/_end(), then this module restores whatever
   the real connection state was on a cancelled gesture. */
typedef enum {
    FEB_STATUS_LED_CONNECTING = 0, /* scanning, or connected but not yet authenticated -- slow blink */
    FEB_STATUS_LED_CONNECTED,      /* authenticated runtime session -- solid on */
    FEB_STATUS_LED_FLUSHING,       /* backlog draining to the Flipper -- fast blink */
} feb_status_led_state_t;

/* Configures GPIO25 as output and applies the initial FEB_STATUS_LED_CONNECTING state. Call
   once from app_main() before any other status_led API use. */
void feb_status_led_init(void);

/* Records the new connection-status state and, unless a factory-reset gesture is currently
   suppressing redraws, applies it to the LED immediately. */
void feb_status_led_set(feb_status_led_state_t state);

/* Periodic tick (expected roughly every FEB_REASSEMBLY_CHECK_INTERVAL_MS, piggybacked on
   reassembly_timeout_cb()): advances the CONNECTING/FLUSHING blink cadence. No-op for
   CONNECTED (solid) or while a factory-reset gesture is active. */
void feb_status_led_tick(void);

/* Suppresses this module's own LED redraws so factory_reset.c's own blink pattern is not
   clobbered by a connection-state change or blink tick while the gesture is in progress. */
void feb_status_led_factory_reset_begin(void);

/* Re-enables this module's redraws and re-applies the current connection-status state --
   restores the real LED state after a cancelled (early-release) factory-reset gesture. */
void feb_status_led_factory_reset_end(void);

/* Low-level plain-GPIO setter, shared with factory_reset.c for its own hold-gesture blink. */
void feb_led_set(bool on);

#endif

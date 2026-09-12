#ifndef FEB_STATUS_LED_H
#define FEB_STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

/* Connection-status indicator on the onboard WS2812 (GPIO8): blinking blue while
   scanning/connected-but-unauthenticated, solid blue once the runtime session is
   authenticated, solid green while a wardriving backlog batch is actively flushing. The
   blinking/solid blue states render purple instead while a wardriving capture session is
   active (see feb_status_led_set_wardriving_active()); FLUSHING is unaffected either way.
   factory_reset.c suppresses this module's own redraws for the duration of its BOOT-hold
   gesture (dim red blink) via _begin()/_end(), then this module restores whatever the real
   connection state was on a cancelled gesture. */
typedef enum {
    FEB_STATUS_LED_CONNECTING = 0, /* scanning, or connected but not yet authenticated -- blinks blue */
    FEB_STATUS_LED_CONNECTED,      /* authenticated runtime session -- solid blue */
    FEB_STATUS_LED_FLUSHING,       /* wardriving backlog draining to the Flipper -- solid green */
} feb_status_led_state_t;

/* Sets up the RMT channel/encoder driving the WS2812 and applies the initial
   FEB_STATUS_LED_CONNECTING state. Call once from app_main() before any other status_led
   API use. */
void feb_status_led_init(void);

/* Records the new connection-status state and, unless a factory-reset gesture is currently
   suppressing redraws, applies it to the LED immediately. */
void feb_status_led_set(feb_status_led_state_t state);

/* Toggles whether the CONNECTING/CONNECTED states render as purple (wardriving capture
   active) or their normal blue; FLUSHING is unaffected. Applies immediately unless a
   factory-reset gesture is currently suppressing redraws. */
void feb_status_led_set_wardriving_active(bool active);

/* Periodic tick (expected roughly every FEB_REASSEMBLY_CHECK_INTERVAL_MS, piggybacked on
   reassembly_timeout_cb()): advances the FEB_STATUS_LED_CONNECTING blink. No-op in any other
   state or while a factory-reset gesture is active. */
void feb_status_led_tick(void);

/* Suppresses this module's own LED redraws so factory_reset.c's dim-red blink is not
   clobbered by a connection-state change or blink tick while the gesture is in progress. */
void feb_status_led_factory_reset_begin(void);

/* Re-enables this module's redraws and re-applies the current connection-status state --
   restores the real LED state after a cancelled (early-release) factory-reset gesture. */
void feb_status_led_factory_reset_end(void);

/* Low-level WS2812 setter (GRB wire order handled internally), shared with factory_reset.c
   for its own red blink. */
void feb_ws2812_set(uint8_t red, uint8_t green, uint8_t blue);

#endif

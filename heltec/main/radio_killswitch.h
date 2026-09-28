#ifndef FEB_RADIO_KILLSWITCH_H
#define FEB_RADIO_KILLSWITCH_H

#include <stdbool.h>

/* NVS-backed persisted radio-enabled flag ("feb_radio" namespace, single u8 under key "on").
   feb_radio_kill_switch_load_persisted() defaults to true (radio on) if no value has ever
   been stored -- normal out-of-box behavior, matching every other capability's boot default. */
bool feb_radio_kill_switch_load_persisted(void);
void feb_radio_kill_switch_persist(bool enabled);

/* Configures GPIO2 (touch channel T2, confirmed free -- see docs/hardware/
   heltec-wifi-lora-32-v2/README.md) as a capacitive kill-switch pad, calibrates a baseline
   threshold against this board/environment at boot (classic-ESP32 touch counts are not a
   portable constant), and starts the debounce task. Safe to call regardless of whether Wi-Fi/
   BLE are currently running at boot -- the task only detects touch gestures and calls
   feb_radio_kill_switch_toggle(); it does not itself touch any radio state. */
void feb_radio_kill_switch_start(void);

/* Implemented in main.c. Called from the touch debounce task (never from the NimBLE host
   task) on a single deliberate touch-and-release; flips the current Wi-Fi/BLE enabled state,
   performs the actual stop-or-start sequence for both radios, updates the OLED, and persists
   the new state via feb_radio_kill_switch_persist(). */
void feb_radio_kill_switch_toggle(void);

#endif

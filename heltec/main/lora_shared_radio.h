/* SX1276 (RadioLib-driven) shared LoRa listener -- pure-C interface; the implementation
   (lora_shared_radio.cpp) is C++ because RadioLib itself is a C++-only library (no C API).
   Renamed 2026-09-27 from meshcore_radio.h/.cpp (docs/PLAN.md's "MeshCore Scan Capability"
   design plan) when `meshtastic_scan` was added and needed to share this board's single
   physical SX1276 with `meshcore_scan` -- see this file's radio-sharing decision below and
   docs/SESSION_MEMORY.md's meshtastic_scan entry for the full writeup.

   Modeled on location.h/location.c's shape: a single init-once-at-boot entry point that
   starts a dedicated background task; the task's parsed output lands in meshcore_table.h/
   meshtastic_table.h, not returned through this header.

   ---- Radio-sharing decision (the one open design question this capability's task raised) ----
   MeshCore and Meshtastic cannot both continuously receive on this board's single SX1276 at
   the same time: RadioLib's SX1276 driver only ever has one active configuration. Two options
   were considered: (1) time-multiplex the one radio between both protocols on an interval, or
   (2) a runtime mode-select command letting the Flipper/user pick one protocol at a time.
   **Time-multiplexing was chosen.** The deciding fact (confirmed via this session's own
   research, not assumed): MeshCore's fixed EU-868 preset here (869.525 MHz, 250 kHz BW, SF11,
   CR4/5) and Meshtastic's EU_868 "LongFast" default preset use the *exact same* RF modem
   parameters -- the only per-protocol radio setting that differs is the SX1276's one-byte
   sync-word register (MeshCore: RadioLib's own default, 0x12; Meshtastic: 0x2B, per
   meshtastic/firmware's own RadioInterface config). Switching between the two "listen modes"
   is therefore a single `setSyncWord()` SPI register write (which the driver already
   implements as "go to standby, write one register") plus a `startReceive()` call -- not a
   full re-tune of frequency/bandwidth/spreading factor/coding rate. This makes
   time-multiplexing here much cheaper than the option list's framing assumed, and avoids
   needing a new wire-protocol mode-select command (option 2) for a Phase 1 capability pair
   that's otherwise poll-only/config-free. The real, accepted cost: whichever protocol isn't
   the currently active listen mode simply cannot receive at all during the other's window --
   a broadcast that arrives during the "wrong" window is missed outright, not merely delayed.
   See LORA_SHARED_RADIO_DWELL_MS's own comment (lora_shared_radio.cpp) for the interval
   chosen and why it's an engineering guess, not a measured bound -- same "flag the gap,
   don't guess a false precision" posture as this board's other unvalidated radio-coexistence
   numbers (docs/BACKLOG.md). */
#ifndef FEB_LORA_SHARED_RADIO_H
#define FEB_LORA_SHARED_RADIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the SX1276 on this board's already-wired, previously-unclaimed LoRa SPI pins
   (docs/hardware/heltec-wifi-lora-32-v2/README.md's "SX1276/SX1278 LoRa SPI pin mapping"),
   starts RadioLib's interrupt-driven continuous receive, and starts a dedicated FreeRTOS task
   that time-multiplexes the radio between MeshCore's and Meshtastic's listen configurations
   (see this header's radio-sharing decision above), handing each received frame to whichever
   protocol's parser matches the currently active listen mode, and upserting a successful
   decode into meshcore_table.h or meshtastic_table.h respectively. Call once at boot,
   unconditionally, regardless of BLE connection state -- the same "always-on background
   driver" shape as location_init(). A radio/task-init failure is logged and leaves both
   tables permanently empty; both `meshcore_scan`'s and `meshtastic_scan`'s `status` queries
   still answer (empty result) rather than failing outright -- this is optional peripheral
   hardware, not required for the board to boot. */
void lora_shared_radio_init(void);

#ifdef __cplusplus
}
#endif

#endif /* FEB_LORA_SHARED_RADIO_H */

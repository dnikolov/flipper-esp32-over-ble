/* Persists the wardriving capability's on/off state and last-used Wi-Fi/BLE capture
   settings across reboots (docs/BACKLOG.md "Per-board wardriving autostart setting").
   Backed by NVS (a single blob, not the raw-flash log wardriving_log.h owns -- this is
   small, infrequently-written control state, not capture data). ESP32-local only, not
   part of the wire protocol. */
#ifndef FEB_WARDRIVING_PERSIST_H
#define FEB_WARDRIVING_PERSIST_H

#include <stdbool.h>
#include <stdint.h>

/* Bumped whenever this struct's layout or field semantics change -- a same-size blob left
   over from an older layout must not be reinterpreted as the current one (see G13's NVS
   pairing-blob analog; this is the same class of bug applied to a second blob). */
#define FEB_WARDRIVING_PERSIST_VERSION 1u

typedef struct {
    uint32_t version;
    bool enabled; /* wardriving was running when this was last saved */
    bool want_wifi;
    bool want_ble;
    bool want_ble_passive;
    uint32_t wifi_interval_ms;
    uint32_t ble_window_ms;
    uint32_t ble_interval_ms;
} feb_wardriving_persisted_state_t;

/* Loads the last-saved state. On first boot (nothing ever saved), a corrupt/wrong-size
   blob, a version mismatch, or any field failing wardriving_validate.h's bounds (so a
   corrupted-but-plausible-length blob can never resolve to a starvation-prone interval
   combination -- see wardriving_validate.h's duty-cycle history), fills *out with
   enabled=false plus the same wifi+ble/default-interval settings a `start` command with
   sources=["wifi","ble"] and omitted intervals would resolve to, and returns false --
   never fails loudly. */
bool wardriving_persist_load(feb_wardriving_persisted_state_t *out);

/* Saves `state`. Logs and returns on failure, same style as main.c's
   persist_pairing_secret() -- never fatal. */
void wardriving_persist_save(const feb_wardriving_persisted_state_t *state);

#endif /* FEB_WARDRIVING_PERSIST_H */

/* Deduplication filter for wardriving capture: keeps 128 recent observations per
   address (WiFi BSSID or BLE address) and only logs a new observation if it's a
   new address, RSSI improved ≥6dB, or location moved ≥30m. Evicted entries from
   the table are flushed to flash before being discarded. Resets on stop/start
   (wardriving_dedup_reset()). Not part of the protocol contract (ESP32-local
   filtering before logging). */
#ifndef FEB_WARDRIVING_DEDUP_H
#define FEB_WARDRIVING_DEDUP_H

#include <stdbool.h>
#include <stdint.h>

#include "cbor_wardriving.h"

void wardriving_dedup_reset(void);

/* Filter a wardriving record: may log it to flash (if it passes dedup criteria),
   may flush an evicted entry to flash first. Returns true if the record was
   logged (or an evicted entry was flushed); false only for a real append failure
   (does not return false merely because dedup filtered a record). */
bool wardriving_dedup_and_maybe_append(const feb_wardriving_record_t *record);

#endif /* FEB_WARDRIVING_DEDUP_H */

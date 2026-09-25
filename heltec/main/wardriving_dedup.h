/* Deduplication filter for wardriving capture: keeps recent-observation state per
   address (WiFi BSSID or BLE address) and only logs a new observation if it's a
   new address, RSSI improved ≥6dB, or location moved ≥30m. Not part of the
   protocol contract (ESP32-local filtering before logging).

   Separate tables for WiFi and BLE (256 / 512 slots respectively) — no longer one
   128-slot table shared by both, keyed only on the 6 raw address bytes with no
   type discriminator, which let a WiFi and a BLE address alias to the same slot
   (former docs/BACKLOG.md G22). Sizes are picked against a real field capture that
   saw 517 distinct addresses in one session (172 WiFi, 345 BLE;
   docs/grok-4.6-findings-2026-09-11.md), not the prior "~64 active devices per
   type" guess, which assumed a small working set that depended on the table being
   wiped often (see the G35 scope note below — no longer true). Each table probes
   linearly on an FNV-1a hash of the address, so a same-type hash collision no
   longer silently evicts a different address's tracked state; eviction only
   happens when a table is genuinely full (every slot holds a distinct address),
   which the chosen sizes make a rare capacity-exhaustion case, not the routine
   hash-collision case it was before.

   Scope matches the flash-backed "wardrive" log's own lifetime (wardriving_log.c),
   not a single start/stop cycle: these tables are NOT reset when the `wardriving`
   command's start/stop action runs (former docs/BACKLOG.md G35 -- resetting there
   made every still-in-range address look brand-new again on a mid-session restart,
   causing real duplicate flash writes into the persistent log). Both tables start
   empty at boot via ordinary static zero-initialization, not an explicit reset
   call; they are not rebuilt from the existing on-flash log at boot either, so a
   reboot mid-capture can cause one extra duplicate append per still-in-range
   address (never on every subsequent stop/start) -- accepted because
   wardriving_log_init() itself resumes rather than wipes the flash log on boot,
   so this matches that same persistent-lifetime scope rather than being reset
   more often than the store it gates. wardriving_dedup_reset() is currently
   uncalled by any firmware code path; it is kept as the natural hook for a future
   explicit "clear wardriving log" command, should one be added. */
#ifndef FEB_WARDRIVING_DEDUP_H
#define FEB_WARDRIVING_DEDUP_H

#include <stdbool.h>
#include <stdint.h>

#include "cbor_wardriving.h"

void wardriving_dedup_reset(void);

/* Filter a wardriving record: logs it to flash only if it passes dedup criteria
   (new address, RSSI improved ≥6dB, or location moved ≥30m). On the rare
   genuine-table-exhaustion path (see table-sizing note above), the evicted
   address's tracked RSSI/location state is dropped, not flushed — this table
   only ever held that compact tracking state, never a full record to flush.
   Returns false only for a real flash-append failure, never merely because
   dedup filtered a record. */
bool wardriving_dedup_and_maybe_append(const feb_wardriving_record_t *record);

#endif /* FEB_WARDRIVING_DEDUP_H */

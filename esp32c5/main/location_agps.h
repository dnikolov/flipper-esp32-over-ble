#ifndef FEB_LOCATION_AGPS_H
#define FEB_LOCATION_AGPS_H

/* C5-only exclusive access to the GPS UART for the `agps` feed (docs/AGPS.md). location.c's
   NMEA task is the UART's only reader; it takes this lock around each uart_read_bytes() call
   (<= 200 ms), so a feed that holds the lock owns the UART exclusively and the NMEA task simply
   waits (its fix state goes stale for the few seconds the feed takes). Kept out of location.h
   so that header stays identical across boards (tools/check_shared_headers.py). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void location_agps_init(void);
/* The NMEA task's per-read lock; the task resyncs its line buffer when the epoch changed. */
void location_agps_task_lock(void);
void location_agps_task_unlock(void);
uint32_t location_agps_epoch(void);

/* True once the UART driver is installed and configured. */
bool location_agps_available(void);
/* Takes exclusive UART access (waits up to timeout_ms), discards pending RX bytes. */
bool location_agps_begin(uint32_t timeout_ms);
void location_agps_end(void);
/* Only between location_agps_begin() and location_agps_end(). */
void location_agps_write(const uint8_t *data, size_t len);
size_t location_agps_read(uint8_t *buf, size_t cap, uint32_t timeout_ms);

#endif

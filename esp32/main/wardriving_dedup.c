#include <string.h>

#include "wardriving_dedup.h"
#include "wardriving_log.h"

#define FEB_WARDRIVING_DEDUP_TABLE_SIZE 128u

/* Per-address observation state: tracks the most recent values seen so we can
   decide whether a new observation warrants a log entry. */
typedef struct {
    uint8_t address[6];  /* WiFi BSSID or BLE address */
    bool in_use;
    uint64_t last_rssi_offset;
    uint64_t last_lat_e7_offset;
    uint64_t last_lon_e7_offset;
} dedup_entry_t;

static dedup_entry_t dedup_table[FEB_WARDRIVING_DEDUP_TABLE_SIZE];

/* Hash address to a table slot (simple XOR-fold of the 6 bytes). */
static size_t hash_address(const uint8_t address[6])
{
    return ((address[0] ^ address[1] ^ address[2] ^ address[3] ^ address[4] ^ address[5])
            % FEB_WARDRIVING_DEDUP_TABLE_SIZE);
}

void wardriving_dedup_reset(void)
{
    memset(dedup_table, 0, sizeof(dedup_table));
}

/* Returns true if record passed dedup criteria and was logged (or an evicted
   entry was flushed); false only for an append failure. */
static bool should_log_record(const uint8_t address[6], uint64_t rssi_offset,
                               uint64_t lat_e7, uint64_t lon_e7)
{
    size_t slot = hash_address(address);
    dedup_entry_t *entry = &dedup_table[slot];

    /* New address (slot unused) */
    if (!entry->in_use) {
        return true;
    }

    /* Address collision: different address in this slot */
    if (memcmp(entry->address, address, 6) != 0) {
        return true;
    }

    /* Same address: check dedup criteria */
    int rssi_delta = (int)rssi_offset - (int)entry->last_rssi_offset;
    if (rssi_delta >= 6) {
        /* RSSI improved by ≥6dB */
        return true;
    }

    /* Location moved: check if either lat or lon changed by ≥30m.
       At the equator, 1 degree ≈ 111 km, so 1e7 units ≈ 1.11 cm, i.e. 1 unit ≈ 1.11 cm.
       30 m = 3000 cm; 3000 cm / 1.11 cm per unit ≈ 2700 units. */
    uint64_t lat_delta = (lat_e7 > entry->last_lat_e7_offset) ?
                         (lat_e7 - entry->last_lat_e7_offset) :
                         (entry->last_lat_e7_offset - lat_e7);
    uint64_t lon_delta = (lon_e7 > entry->last_lon_e7_offset) ?
                         (lon_e7 - entry->last_lon_e7_offset) :
                         (entry->last_lon_e7_offset - lon_e7);

    if (lat_delta >= 2700ULL || lon_delta >= 2700ULL) {
        return true;
    }

    /* No criteria met; skip logging */
    return false;
}

bool wardriving_dedup_and_maybe_append(const feb_wardriving_record_t *record)
{
    size_t slot = hash_address((record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI)
                                ? record->payload.wifi.bssid
                                : record->payload.ble.address);
    dedup_entry_t *entry = &dedup_table[slot];
    const uint8_t *address = (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI)
                             ? record->payload.wifi.bssid
                             : record->payload.ble.address;

    /* If this slot had a different address and was in use, we evict it.
       The 128-slot fixed table means hash collisions evict the old entry
       without flushing (we only track address/RSSI/location, not the full
       SSID/auth/name fields needed to reconstruct a complete record).
       With ~64 active devices per type, collisions should be rare. */

    /* Check dedup criteria */
    if (!should_log_record(address, record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI
                                     ? record->payload.wifi.rssi_offset
                                     : record->payload.ble.rssi_offset,
                           record->lat_e7_offset, record->lon_e7_offset)) {
        /* Filtered by dedup; don't log */
        return true;  /* Not a failure, just filtered */
    }

    /* Log to flash */
    if (!wardriving_log_append(record)) {
        return false;  /* Real append failure */
    }

    /* Update table with this observation */
    memcpy(entry->address, address, 6);
    entry->in_use = true;
    entry->last_rssi_offset = (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI)
                              ? record->payload.wifi.rssi_offset
                              : record->payload.ble.rssi_offset;
    entry->last_lat_e7_offset = record->lat_e7_offset;
    entry->last_lon_e7_offset = record->lon_e7_offset;

    return true;
}

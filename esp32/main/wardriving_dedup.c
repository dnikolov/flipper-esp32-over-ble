#include <string.h>

#include "wardriving_dedup.h"
#include "wardriving_log.h"

#define FEB_WARDRIVING_DEDUP_WIFI_TABLE_SIZE 256u
#define FEB_WARDRIVING_DEDUP_BLE_TABLE_SIZE 512u

/* Per-address observation state: tracks the most recent values seen so we can
   decide whether a new observation warrants a log entry. */
typedef struct {
    uint8_t address[6];  /* WiFi BSSID or BLE address */
    bool in_use;
    uint64_t last_rssi_offset;
    uint64_t last_lat_e7_offset;
    uint64_t last_lon_e7_offset;
} dedup_entry_t;

static dedup_entry_t wifi_table[FEB_WARDRIVING_DEDUP_WIFI_TABLE_SIZE];
static dedup_entry_t ble_table[FEB_WARDRIVING_DEDUP_BLE_TABLE_SIZE];

/* FNV-1a over the 6 address bytes, mod the table size (a power of two, so this
   is a plain AND under the hood). Mixes better than a byte-order-independent
   XOR-fold and, combined with linear probing below, keeps same-type collisions
   from silently clobbering an unrelated address's tracked state. */
static size_t hash_address(const uint8_t address[6], size_t table_size)
{
    uint32_t hash = 2166136261u;
    int i;

    for (i = 0; i < 6; i++) {
        hash ^= address[i];
        hash *= 16777619u;
    }
    return (size_t)(hash % table_size);
}

void wardriving_dedup_reset(void)
{
    memset(wifi_table, 0, sizeof(wifi_table));
    memset(ble_table, 0, sizeof(ble_table));
}

/* Linear-probes to the slot already tracking `address`, or the first free slot
   along the probe sequence. Only falls back to evicting the initial hash slot
   if every slot in the table is in use by a distinct address (the table is
   genuinely at capacity, not merely a hash collision). */
static dedup_entry_t *find_slot(dedup_entry_t *table, size_t table_size,
                                 const uint8_t address[6], bool *out_existing)
{
    size_t start = hash_address(address, table_size);
    size_t i;

    for (i = 0; i < table_size; i++) {
        dedup_entry_t *entry = &table[(start + i) % table_size];

        if (!entry->in_use) {
            *out_existing = false;
            return entry;
        }
        if (memcmp(entry->address, address, 6) == 0) {
            *out_existing = true;
            return entry;
        }
    }

    *out_existing = false;
    return &table[start];
}

static dedup_entry_t *table_for_kind(feb_wardriving_payload_kind_t kind, size_t *out_size)
{
    if (kind == FEB_WARDRIVING_PAYLOAD_WIFI) {
        *out_size = FEB_WARDRIVING_DEDUP_WIFI_TABLE_SIZE;
        return wifi_table;
    }
    *out_size = FEB_WARDRIVING_DEDUP_BLE_TABLE_SIZE;
    return ble_table;
}

/* Returns true if the observation passed dedup criteria and warrants logging. */
static bool should_log_record(const dedup_entry_t *entry, bool existing,
                               uint64_t rssi_offset, uint64_t lat_e7, uint64_t lon_e7)
{
    int rssi_delta;
    uint64_t lat_delta;
    uint64_t lon_delta;

    if (!existing) {
        return true;
    }

    rssi_delta = (int)rssi_offset - (int)entry->last_rssi_offset;
    if (rssi_delta >= 6) {
        /* RSSI improved by >=6dB */
        return true;
    }

    /* Location moved: check if either lat or lon changed by >=30m.
       At the equator, 1 degree ~= 111 km, so 1e7 units ~= 1.11 cm, i.e. 1 unit
       ~= 1.11 cm. 30 m = 3000 cm; 3000 cm / 1.11 cm per unit ~= 2700 units. */
    lat_delta = (lat_e7 > entry->last_lat_e7_offset) ?
                (lat_e7 - entry->last_lat_e7_offset) :
                (entry->last_lat_e7_offset - lat_e7);
    lon_delta = (lon_e7 > entry->last_lon_e7_offset) ?
                (lon_e7 - entry->last_lon_e7_offset) :
                (entry->last_lon_e7_offset - lon_e7);

    return (lat_delta >= 2700ULL || lon_delta >= 2700ULL);
}

bool wardriving_dedup_and_maybe_append(const feb_wardriving_record_t *record)
{
    size_t table_size;
    dedup_entry_t *table = table_for_kind(record->payload_kind, &table_size);
    const uint8_t *address = (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI)
                             ? record->payload.wifi.bssid
                             : record->payload.ble.address;
    uint64_t rssi_offset = (record->payload_kind == FEB_WARDRIVING_PAYLOAD_WIFI)
                           ? record->payload.wifi.rssi_offset
                           : record->payload.ble.rssi_offset;
    bool existing;
    dedup_entry_t *entry = find_slot(table, table_size, address, &existing);

    if (!should_log_record(entry, existing, rssi_offset, record->lat_e7_offset,
                            record->lon_e7_offset)) {
        return true;  /* Filtered by dedup; not a failure */
    }

    if (!wardriving_log_append(record)) {
        return false;  /* Real append failure */
    }

    memcpy(entry->address, address, 6);
    entry->in_use = true;
    entry->last_rssi_offset = rssi_offset;
    entry->last_lat_e7_offset = record->lat_e7_offset;
    entry->last_lon_e7_offset = record->lon_e7_offset;

    return true;
}

#include "meshcore_table.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE meshcore_table_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* Guarded by meshcore_table_spinlock; static/file-scope per this project's stack-budget
   convention (docs/BACKLOG.md's cost-efficiency section, this codebase's repeated
   BleEventWorker/task-stack-overflow bug class) rather than stack-resident anywhere. */
static meshcore_table_entry_t meshcore_table_entries[MESHCORE_TABLE_MAX_ENTRIES];
static size_t meshcore_table_count;

void meshcore_table_upsert(const meshcore_advert_t *advert, int32_t rssi_dbm, uint64_t last_seen_ms)
{
    size_t i;
    size_t target;
    bool found = false;

    if (advert == NULL) {
        return;
    }

    portENTER_CRITICAL(&meshcore_table_spinlock);

    target = 0;
    for (i = 0; i < meshcore_table_count; i++) {
        if (memcmp(meshcore_table_entries[i].node_id_hex, advert->node_id_hex,
                   sizeof(advert->node_id_hex)) == 0) {
            target = i;
            found = true;
            break;
        }
    }
    if (!found) {
        if (meshcore_table_count < MESHCORE_TABLE_MAX_ENTRIES) {
            target = meshcore_table_count++;
        } else {
            /* Table full: evict the single least-recently-seen entry (see meshcore_table.h's
               sizing note) rather than refuse the new sighting. */
            size_t oldest = 0;

            for (i = 1; i < MESHCORE_TABLE_MAX_ENTRIES; i++) {
                if (meshcore_table_entries[i].last_seen_ms < meshcore_table_entries[oldest].last_seen_ms) {
                    oldest = i;
                }
            }
            target = oldest;
        }
    }

    memset(&meshcore_table_entries[target], 0, sizeof(meshcore_table_entries[target]));
    memcpy(meshcore_table_entries[target].node_id_hex, advert->node_id_hex,
           sizeof(advert->node_id_hex));
    meshcore_table_entries[target].has_name = advert->has_name;
    if (advert->has_name) {
        memcpy(meshcore_table_entries[target].name, advert->name, sizeof(advert->name));
    }
    meshcore_table_entries[target].role = advert->role;
    meshcore_table_entries[target].rssi_dbm = rssi_dbm;
    meshcore_table_entries[target].last_seen_ms = (uint32_t)last_seen_ms; /* see
                                                                              meshcore_table.h's
                                                                              comment on this
                                                                              field's width */
    meshcore_table_entries[target].has_location = advert->has_location;
    meshcore_table_entries[target].lat_e7 = advert->lat_e7;
    meshcore_table_entries[target].lon_e7 = advert->lon_e7;

    portEXIT_CRITICAL(&meshcore_table_spinlock);
}

size_t meshcore_table_snapshot(meshcore_table_entry_t *out, size_t max_entries, uint64_t *out_total_known)
{
    size_t copied = 0;
    size_t total;
    /* Small enough (<= MESHCORE_TABLE_MAX_ENTRIES = 64 bool flags) to keep on this
       function's own stack even while holding the spinlock -- no dynamic allocation, no
       peer-controlled sizing. */
    bool used[MESHCORE_TABLE_MAX_ENTRIES] = {0};

    if (out == NULL) {
        max_entries = 0;
    }

    portENTER_CRITICAL(&meshcore_table_spinlock);

    total = meshcore_table_count;
    while (copied < max_entries && copied < total) {
        size_t best = SIZE_MAX;
        size_t i;

        for (i = 0; i < total; i++) {
            if (used[i]) {
                continue;
            }
            if (best == SIZE_MAX ||
                meshcore_table_entries[i].last_seen_ms > meshcore_table_entries[best].last_seen_ms) {
                best = i;
            }
        }
        used[best] = true;
        out[copied] = meshcore_table_entries[best];
        copied++;
    }

    portEXIT_CRITICAL(&meshcore_table_spinlock);

    if (out_total_known != NULL) {
        *out_total_known = (uint64_t)total;
    }
    return copied;
}

#include "meshtastic_table.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE meshtastic_table_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* Guarded by meshtastic_table_spinlock; static/file-scope per this project's stack-budget
   convention, same rationale as meshcore_table.c's own static arrays. */
static meshtastic_table_entry_t meshtastic_table_entries[MESHTASTIC_TABLE_MAX_ENTRIES];
static size_t meshtastic_table_count;

void meshtastic_table_upsert(const meshtastic_advert_t *advert, int32_t rssi_dbm, uint64_t last_seen_ms)
{
    size_t i;
    size_t target;
    bool found = false;

    if (advert == NULL) {
        return;
    }

    portENTER_CRITICAL(&meshtastic_table_spinlock);

    target = 0;
    for (i = 0; i < meshtastic_table_count; i++) {
        if (memcmp(meshtastic_table_entries[i].node_id_hex, advert->node_id_hex,
                   sizeof(advert->node_id_hex)) == 0) {
            target = i;
            found = true;
            break;
        }
    }
    if (!found) {
        if (meshtastic_table_count < MESHTASTIC_TABLE_MAX_ENTRIES) {
            target = meshtastic_table_count++;
        } else {
            size_t oldest = 0;

            for (i = 1; i < MESHTASTIC_TABLE_MAX_ENTRIES; i++) {
                if (meshtastic_table_entries[i].last_seen_ms < meshtastic_table_entries[oldest].last_seen_ms) {
                    oldest = i;
                }
            }
            target = oldest;
        }
    }

    memset(&meshtastic_table_entries[target], 0, sizeof(meshtastic_table_entries[target]));
    memcpy(meshtastic_table_entries[target].node_id_hex, advert->node_id_hex,
           sizeof(advert->node_id_hex));
    meshtastic_table_entries[target].has_name = advert->has_name;
    if (advert->has_name) {
        memcpy(meshtastic_table_entries[target].name, advert->name, sizeof(advert->name));
    }
    meshtastic_table_entries[target].rssi_dbm = rssi_dbm;
    meshtastic_table_entries[target].last_seen_ms = (uint32_t)last_seen_ms;

    portEXIT_CRITICAL(&meshtastic_table_spinlock);
}

size_t meshtastic_table_snapshot(meshtastic_table_entry_t *out, size_t max_entries, uint64_t *out_total_known)
{
    size_t copied = 0;
    size_t total;
    bool used[MESHTASTIC_TABLE_MAX_ENTRIES] = {0};

    if (out == NULL) {
        max_entries = 0;
    }

    portENTER_CRITICAL(&meshtastic_table_spinlock);

    total = meshtastic_table_count;
    while (copied < max_entries && copied < total) {
        size_t best = SIZE_MAX;
        size_t i;

        for (i = 0; i < total; i++) {
            if (used[i]) {
                continue;
            }
            if (best == SIZE_MAX ||
                meshtastic_table_entries[i].last_seen_ms > meshtastic_table_entries[best].last_seen_ms) {
                best = i;
            }
        }
        used[best] = true;
        out[copied] = meshtastic_table_entries[best];
        copied++;
    }

    portEXIT_CRITICAL(&meshtastic_table_spinlock);

    if (out_total_known != NULL) {
        *out_total_known = (uint64_t)total;
    }
    return copied;
}

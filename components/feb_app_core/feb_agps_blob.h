#ifndef FEB_AGPS_BLOB_H
#define FEB_AGPS_BLOB_H

/* Pure chunk-reassembly state machine for the `agps` window blob (docs/PROTOCOL.md "`agps`
   command and status payloads"). Strictly sequential: any error resets the whole state. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feb_agps_casic.h"

#define FEB_AGPS_CHUNK_MAX 256u
#define FEB_AGPS_CHUNK_TIMEOUT_MS 5000u

typedef enum {
    FEB_AGPS_BLOB_MORE = 0,       /* chunk accepted, more expected */
    FEB_AGPS_BLOB_COMPLETE,       /* final chunk accepted, blob is complete */
    FEB_AGPS_BLOB_ERR_TOTAL,      /* total is 0 or above FEB_AGPS_BLOB_MAX */
    FEB_AGPS_BLOB_ERR_LEN,        /* chunk is empty or above FEB_AGPS_CHUNK_MAX */
    FEB_AGPS_BLOB_ERR_ORDER,      /* off != next expected offset (gap, duplicate, overlap, bad start) */
    FEB_AGPS_BLOB_ERR_OVERRUN,    /* off + len > total */
    FEB_AGPS_BLOB_ERR_MISMATCH,   /* total changed mid-transfer */
    FEB_AGPS_BLOB_ERR_COMPLETE,   /* chunk after completion (state is NOT reset: caller owns it) */
    FEB_AGPS_BLOB_ERR_TIMEOUT,    /* previous chunk was more than FEB_AGPS_CHUNK_TIMEOUT_MS ago */
} feb_agps_blob_status_t;

typedef struct {
    uint8_t data[FEB_AGPS_BLOB_MAX];
    uint32_t total;
    uint32_t received;
    uint32_t last_chunk_ms;
    bool active;
    bool complete;
} feb_agps_blob_t;

void feb_agps_blob_reset(feb_agps_blob_t *blob);

/* Every error except FEB_AGPS_BLOB_ERR_COMPLETE leaves the blob reset. */
feb_agps_blob_status_t feb_agps_blob_chunk(feb_agps_blob_t *blob, uint32_t total, uint32_t off,
                                           const uint8_t *data, size_t len, uint32_t now_ms);

/* Drops a half-received blob whose last chunk is too old. Returns true if it did. */
bool feb_agps_blob_expire(feb_agps_blob_t *blob, uint32_t now_ms);

#endif

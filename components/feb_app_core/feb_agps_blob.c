#include "feb_agps_blob.h"

#include <string.h>

void feb_agps_blob_reset(feb_agps_blob_t *blob)
{
    blob->total = 0;
    blob->received = 0;
    blob->last_chunk_ms = 0;
    blob->active = false;
    blob->complete = false;
}

bool feb_agps_blob_expire(feb_agps_blob_t *blob, uint32_t now_ms)
{
    if (blob->active && (uint32_t)(now_ms - blob->last_chunk_ms) > FEB_AGPS_CHUNK_TIMEOUT_MS) {
        feb_agps_blob_reset(blob);
        return true;
    }
    return false;
}

feb_agps_blob_status_t feb_agps_blob_chunk(feb_agps_blob_t *blob, uint32_t total, uint32_t off,
                                           const uint8_t *data, size_t len, uint32_t now_ms)
{
    if (blob->complete) {
        return FEB_AGPS_BLOB_ERR_COMPLETE;
    }
    if (data == NULL || len == 0 || len > FEB_AGPS_CHUNK_MAX) {
        feb_agps_blob_reset(blob);
        return FEB_AGPS_BLOB_ERR_LEN;
    }
    if (total == 0 || total > FEB_AGPS_BLOB_MAX) {
        feb_agps_blob_reset(blob);
        return FEB_AGPS_BLOB_ERR_TOTAL;
    }
    if (blob->active && (uint32_t)(now_ms - blob->last_chunk_ms) > FEB_AGPS_CHUNK_TIMEOUT_MS) {
        feb_agps_blob_reset(blob);
        return FEB_AGPS_BLOB_ERR_TIMEOUT;
    }
    if (!blob->active) {
        if (off != 0) {
            feb_agps_blob_reset(blob);
            return FEB_AGPS_BLOB_ERR_ORDER;
        }
        blob->total = total;
        blob->received = 0;
    } else {
        if (total != blob->total) {
            feb_agps_blob_reset(blob);
            return FEB_AGPS_BLOB_ERR_MISMATCH;
        }
        if (off != blob->received) {
            feb_agps_blob_reset(blob);
            return FEB_AGPS_BLOB_ERR_ORDER;
        }
    }
    if ((uint64_t)off + len > total) {
        feb_agps_blob_reset(blob);
        return FEB_AGPS_BLOB_ERR_OVERRUN;
    }
    memcpy(blob->data + off, data, len);
    blob->received += (uint32_t)len;
    blob->last_chunk_ms = now_ms;
    if (blob->received == blob->total) {
        blob->active = false;
        blob->complete = true;
        return FEB_AGPS_BLOB_COMPLETE;
    }
    blob->active = true;
    return FEB_AGPS_BLOB_MORE;
}

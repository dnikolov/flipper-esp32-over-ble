#include "framing.h"

#include <string.h>

size_t feb_fragment_capacity(uint16_t att_mtu)
{
    uint16_t overhead = FEB_ATT_WRITE_OVERHEAD + FEB_FRAG_HEADER_SIZE;

    if (att_mtu <= overhead) {
        return 0;
    }
    return (size_t)(att_mtu - overhead);
}

void feb_reassembly_reset(feb_reassembly_t *r)
{
    if (r == NULL) {
        return;
    }
    r->record_len = 0;
    r->message_id = 0;
    r->fragment_count = 0;
    r->next_expected_index = 0;
    r->fragment_payload_capacity = 0;
    r->started_at_ms = 0;
    r->in_progress = 0;
}

uint8_t feb_fragment_record(
    const uint8_t *record,
    size_t record_len,
    size_t capacity,
    uint8_t message_id,
    feb_emit_fn emit,
    void *ctx)
{
    /* static, not stack-local: some callers on either firmware invoke this from a
       call chain running on a small dedicated stack (e.g. the Flipper's 1280-byte
       BleEventWorker thread), where a 772-byte automatic array is a real stack-
       overflow risk. Safe as static because fragmentation is synchronous and
       single-in-flight on both firmwares (no reentrant/concurrent callers). */
    static uint8_t frag_buf[FEB_FRAG_HEADER_SIZE + FEB_MAX_RECORD_SIZE];
    size_t count;
    size_t offset;
    size_t i;

    if (record == NULL || record_len == 0 || record_len > FEB_MAX_RECORD_SIZE ||
        capacity == 0 || emit == NULL) {
        return 0;
    }

    count = (record_len + capacity - 1) / capacity;
    if (count > FEB_MAX_FRAGMENTS) {
        return 0;
    }

    offset = 0;
    for (i = 0; i < count; i++) {
        size_t chunk = record_len - offset;

        if (chunk > capacity) {
            chunk = capacity;
        }
        frag_buf[0] = 0;
        frag_buf[1] = message_id;
        frag_buf[2] = (uint8_t)i;
        frag_buf[3] = (uint8_t)count;
        memcpy(frag_buf + FEB_FRAG_HEADER_SIZE, record + offset, chunk);
        emit(frag_buf, FEB_FRAG_HEADER_SIZE + chunk, ctx);
        offset += chunk;
    }
    return (uint8_t)count;
}

feb_frame_status_t feb_reassembly_feed(
    feb_reassembly_t *r,
    const uint8_t *fragment,
    size_t len,
    uint32_t now_ms,
    const uint8_t **out_record,
    size_t *out_len)
{
    uint8_t flags;
    uint8_t message_id;
    uint8_t fragment_index;
    uint8_t fragment_count;
    size_t payload_len;
    int start_new;

    if (r == NULL || fragment == NULL || out_record == NULL || out_len == NULL) {
        return FEB_FRAME_INVALID_ARGUMENT;
    }
    if (len < FEB_FRAG_HEADER_SIZE) {
        feb_reassembly_reset(r);
        return FEB_FRAME_INVALID_HEADER;
    }

    flags = fragment[0];
    message_id = fragment[1];
    fragment_index = fragment[2];
    fragment_count = fragment[3];
    payload_len = len - FEB_FRAG_HEADER_SIZE;

    if (flags != 0 || fragment_count == 0 || fragment_index >= fragment_count) {
        feb_reassembly_reset(r);
        return FEB_FRAME_INVALID_HEADER;
    }

    start_new = (!r->in_progress) || (message_id != r->message_id);

    if (start_new) {
        if (fragment_index != 0) {
            feb_reassembly_reset(r);
            return FEB_FRAME_OUT_OF_ORDER;
        }
        /* Reject up front if the declared fragment_count can never fit within
           FEB_MAX_RECORD_SIZE, instead of waiting for the accumulation to overflow.
           feb_fragment_record() only ever shortens the *last* fragment, so every
           fragment before it is exactly this fragment's payload_len (the capacity
           established by fragment 0); the smallest possible total the sender could
           legitimately produce is (fragment_count - 1) * payload_len + 1. Bounding on
           that minimum (rather than fragment_count * payload_len, the maximum) avoids
           spuriously rejecting a legitimate near-768-byte record whose capacity does
           not evenly divide its length. */
        if (payload_len > FEB_MAX_RECORD_SIZE ||
            (uint64_t)(fragment_count - 1) * (uint64_t)payload_len >= (uint64_t)FEB_MAX_RECORD_SIZE) {
            feb_reassembly_reset(r);
            return FEB_FRAME_OVERSIZED;
        }
        r->message_id = message_id;
        r->fragment_count = fragment_count;
        r->fragment_payload_capacity = payload_len;
        r->next_expected_index = 0;
        r->record_len = 0;
        r->started_at_ms = now_ms;
        r->in_progress = 1;
    }

    if (fragment_count != r->fragment_count) {
        feb_reassembly_reset(r);
        return FEB_FRAME_INCONSISTENT_COUNT;
    }
    if (fragment_index < r->next_expected_index) {
        feb_reassembly_reset(r);
        return FEB_FRAME_DUPLICATE_FRAGMENT;
    }
    if (fragment_index != r->next_expected_index) {
        feb_reassembly_reset(r);
        return FEB_FRAME_OUT_OF_ORDER;
    }
    if (payload_len > r->fragment_payload_capacity) {
        feb_reassembly_reset(r);
        return FEB_FRAME_OVERSIZED;
    }
    if (r->record_len + payload_len > FEB_MAX_RECORD_SIZE) {
        feb_reassembly_reset(r);
        return FEB_FRAME_OVERSIZED;
    }

    memcpy(r->buffer + r->record_len, fragment + FEB_FRAG_HEADER_SIZE, payload_len);
    r->record_len += payload_len;
    r->next_expected_index++;

    if (r->next_expected_index == r->fragment_count) {
        size_t completed_len = r->record_len;

        /* feb_reassembly_reset() only clears reassembly bookkeeping, not r->buffer's
           contents, so out_record stays valid until the caller's next mutating call
           (per this function's header contract) even though we reset here rather than
           leaving in_progress set. Resetting now (instead of leaving stale message_id/
           fragment_count behind) prevents a same-message_id false continuation once
           message_id wraps at 256 in a long-running session. */
        feb_reassembly_reset(r);
        *out_record = r->buffer;
        *out_len = completed_len;
        return FEB_FRAME_MESSAGE_COMPLETE;
    }
    return FEB_FRAME_OK;
}

feb_frame_status_t feb_reassembly_check_timeout(feb_reassembly_t *r, uint32_t now_ms)
{
    if (r == NULL) {
        return FEB_FRAME_INVALID_ARGUMENT;
    }
    if (!r->in_progress) {
        return FEB_FRAME_OK;
    }
    if ((uint32_t)(now_ms - r->started_at_ms) > FEB_REASSEMBLY_TIMEOUT_MS) {
        feb_reassembly_reset(r);
        return FEB_FRAME_TIMEOUT;
    }
    return FEB_FRAME_OK;
}

#include "framing.h"

#include <string.h>

size_t feb_fragment_capacity(uint16_t att_mtu) {
    uint32_t overhead = (uint32_t)FEB_ATT_WRITE_OVERHEAD + (uint32_t)FEB_FRAG_HEADER_SIZE;
    if ((uint32_t)att_mtu <= overhead) {
        return 0;
    }
    return (size_t)((uint32_t)att_mtu - overhead);
}

void feb_reassembly_reset(feb_reassembly_t* r) {
    if(r == NULL) {
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
    const uint8_t* record,
    size_t record_len,
    size_t capacity,
    uint8_t message_id,
    feb_emit_fn emit,
    void* ctx) {
    if(record == NULL || record_len == 0 || record_len > FEB_MAX_RECORD_SIZE) {
        return 0;
    }
    if(capacity == 0 || emit == NULL) {
        return 0;
    }
    size_t fragment_count = (record_len + capacity - 1) / capacity;
    if(fragment_count > FEB_MAX_FRAGMENTS) {
        return 0;
    }

    /* static, not stack-local: some callers on either firmware invoke this from a call
       chain running on a small dedicated stack (e.g. the Flipper's 1280-byte
       BleEventWorker thread), where a 772-byte automatic array is a real stack-overflow
       risk. Safe as static because fragmentation is synchronous and single-in-flight on
       both firmwares (no reentrant/concurrent callers). */
    static uint8_t frag_buf[FEB_FRAG_HEADER_SIZE + FEB_MAX_RECORD_SIZE];
    size_t offset = 0;
    for(size_t index = 0; index < fragment_count; index++) {
        size_t chunk_len = record_len - offset;
        if(chunk_len > capacity) {
            chunk_len = capacity;
        }
        frag_buf[0] = 0;
        frag_buf[1] = message_id;
        frag_buf[2] = (uint8_t)index;
        frag_buf[3] = (uint8_t)fragment_count;
        memcpy(frag_buf + FEB_FRAG_HEADER_SIZE, record + offset, chunk_len);
        emit(frag_buf, FEB_FRAG_HEADER_SIZE + chunk_len, ctx);
        offset += chunk_len;
    }
    return (uint8_t)fragment_count;
}

/* Starts a new in-progress message from its first fragment (fragment_index == 0). Caller
   guarantees header->fragment_count >= 1 and header->fragment_index < header->fragment_count. */
static feb_frame_status_t start_reassembly(
    feb_reassembly_t* r,
    const feb_frag_header_t* header,
    const uint8_t* payload,
    size_t payload_len,
    uint32_t now_ms) {
    /* Reject up front, without buffering all fragments first, if the declared
       fragment_count can never fit within FEB_MAX_RECORD_SIZE (PROTOCOL.md#fragmentation:
       "reject ... without allocating based on peer-controlled lengths"). feb_fragment_record()
       only ever shortens the *last* fragment, so every fragment before it is exactly this
       fragment's payload_len (the capacity established by fragment 0); the smallest total the
       sender could legitimately produce is (fragment_count - 1) * payload_len + 1. Bounding on
       that minimum, rather than fragment_count * payload_len (the maximum), avoids spuriously
       rejecting a legitimate near-768-byte record whose capacity does not evenly divide its
       length (e.g. a 768-byte record at ATT MTU 247). */
    uint64_t min_possible_total = (uint64_t)(header->fragment_count - 1) * (uint64_t)payload_len;
    if(payload_len > FEB_MAX_RECORD_SIZE || min_possible_total >= (uint64_t)FEB_MAX_RECORD_SIZE) {
        feb_reassembly_reset(r);
        return FEB_FRAME_OVERSIZED;
    }
    memcpy(r->buffer, payload, payload_len);
    r->record_len = payload_len;
    r->message_id = header->message_id;
    r->fragment_count = header->fragment_count;
    r->fragment_payload_capacity = payload_len;
    r->next_expected_index = 1;
    r->started_at_ms = now_ms;
    r->in_progress = 1;
    return FEB_FRAME_OK;
}

feb_frame_status_t feb_reassembly_feed(
    feb_reassembly_t* r,
    const uint8_t* fragment,
    size_t len,
    uint32_t now_ms,
    const uint8_t** out_record,
    size_t* out_len) {
    if(r == NULL || fragment == NULL || out_record == NULL || out_len == NULL) {
        return FEB_FRAME_INVALID_ARGUMENT;
    }
    if(len < FEB_FRAG_HEADER_SIZE) {
        feb_reassembly_reset(r);
        return FEB_FRAME_INVALID_HEADER;
    }

    feb_frag_header_t header;
    header.flags = fragment[0];
    header.message_id = fragment[1];
    header.fragment_index = fragment[2];
    header.fragment_count = fragment[3];
    if(header.fragment_count == 0 || header.fragment_index >= header.fragment_count) {
        feb_reassembly_reset(r);
        return FEB_FRAME_INVALID_HEADER;
    }

    const uint8_t* payload = fragment + FEB_FRAG_HEADER_SIZE;
    size_t payload_len = len - FEB_FRAG_HEADER_SIZE;

    if(!r->in_progress || header.message_id != r->message_id) {
        /* First fragment of a new message_id: either nothing was in progress, or this
           message_id differs from the one in progress (starts fresh, discarding it). */
        if(header.fragment_index != 0) {
            feb_reassembly_reset(r);
            return FEB_FRAME_OUT_OF_ORDER;
        }
        feb_frame_status_t status = start_reassembly(r, &header, payload, payload_len, now_ms);
        if(status != FEB_FRAME_OK) {
            return status;
        }
    } else {
        if(header.fragment_index < r->next_expected_index) {
            feb_reassembly_reset(r);
            return FEB_FRAME_DUPLICATE_FRAGMENT;
        }
        if(header.fragment_index > r->next_expected_index) {
            feb_reassembly_reset(r);
            return FEB_FRAME_OUT_OF_ORDER;
        }
        if(header.fragment_count != r->fragment_count) {
            feb_reassembly_reset(r);
            return FEB_FRAME_INCONSISTENT_COUNT;
        }
        if(payload_len > r->fragment_payload_capacity) {
            feb_reassembly_reset(r);
            return FEB_FRAME_OVERSIZED;
        }
        if(r->record_len + payload_len > FEB_MAX_RECORD_SIZE) {
            feb_reassembly_reset(r);
            return FEB_FRAME_OVERSIZED;
        }
        memcpy(r->buffer + r->record_len, payload, payload_len);
        r->record_len += payload_len;
        r->next_expected_index = (uint8_t)(header.fragment_index + 1);
    }

    if(r->next_expected_index == r->fragment_count) {
        /* feb_reassembly_reset() only clears bookkeeping fields, not r->buffer's contents,
           so out_record stays valid until the caller's next mutating call (per this
           function's header contract) even though we reset the tracking state now instead
           of leaving in_progress set. Resetting immediately (instead of leaving a stale
           message_id/fragment_count behind) prevents a same-message_id false continuation
           once message_id wraps at 256 in a long-running session. */
        size_t completed_len = r->record_len;
        *out_record = r->buffer;
        *out_len = completed_len;
        feb_reassembly_reset(r);
        return FEB_FRAME_MESSAGE_COMPLETE;
    }
    return FEB_FRAME_OK;
}

feb_frame_status_t feb_reassembly_check_timeout(feb_reassembly_t* r, uint32_t now_ms) {
    if(r == NULL) {
        return FEB_FRAME_INVALID_ARGUMENT;
    }
    if(!r->in_progress) {
        return FEB_FRAME_OK;
    }
    if((uint32_t)(now_ms - r->started_at_ms) > FEB_REASSEMBLY_TIMEOUT_MS) {
        feb_reassembly_reset(r);
        return FEB_FRAME_TIMEOUT;
    }
    return FEB_FRAME_OK;
}

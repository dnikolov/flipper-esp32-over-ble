/* Shared contract, mirrored byte-for-byte in flipper/framing.h. Changes here must be
   mirrored there and in docs/PROTOCOL.md#fragmentation, or the two firmwares diverge. */
#ifndef FEB_FRAMING_H
#define FEB_FRAMING_H

#include <stdint.h>
#include <stddef.h>

#define FEB_FRAG_HEADER_SIZE 4u
#define FEB_ATT_WRITE_OVERHEAD 3u /* ATT opcode (1) + attribute handle (2) */
#define FEB_MAX_RECORD_SIZE 768u  /* full on-wire record, either shape, pre-CBOR-decode */
#define FEB_MAX_FRAGMENTS 255u
#define FEB_REASSEMBLY_TIMEOUT_MS 2000u

typedef enum {
    FEB_FRAME_OK = 0,
    FEB_FRAME_MESSAGE_COMPLETE, /* feed() consumed the final fragment; out_record is valid */
    FEB_FRAME_DUPLICATE_FRAGMENT,
    FEB_FRAME_INCONSISTENT_COUNT,
    FEB_FRAME_OVERSIZED,
    FEB_FRAME_OUT_OF_ORDER,
    FEB_FRAME_TIMEOUT,
    FEB_FRAME_INVALID_HEADER,
    FEB_FRAME_INVALID_ARGUMENT,
} feb_frame_status_t;

typedef struct {
    uint8_t flags;
    uint8_t message_id;
    uint8_t fragment_index;
    uint8_t fragment_count;
} feb_frag_header_t;

/* One reassembly buffer per direction, per docs/PROTOCOL.md#fragmentation. */
typedef struct {
    uint8_t buffer[FEB_MAX_RECORD_SIZE];
    size_t record_len;         /* bytes reassembled so far */
    uint8_t message_id;        /* message_id of the in-progress message */
    uint8_t fragment_count;    /* fragment_count declared by the first fragment */
    uint8_t next_expected_index;
    size_t fragment_payload_capacity; /* payload bytes per fragment for this message */
    uint32_t started_at_ms;
    int in_progress;
} feb_reassembly_t;

/* fragment payload capacity = negotiated ATT MTU - FEB_ATT_WRITE_OVERHEAD - FEB_FRAG_HEADER_SIZE.
   Returns 0 if att_mtu is too small to carry a header plus at least one payload byte. */
size_t feb_fragment_capacity(uint16_t att_mtu);

void feb_reassembly_reset(feb_reassembly_t *r);

/* Splits `record` (record_len bytes, 1..FEB_MAX_RECORD_SIZE) into fragments of at most
   `capacity` payload bytes each (capacity from feb_fragment_capacity, > 0), each prefixed
   with the 4-byte header (flags = 0). Fragments are emitted in order via `emit`, one at a
   time, into a file-scope static buffer sized FEB_FRAG_HEADER_SIZE + FEB_MAX_RECORD_SIZE
   (not a caller-owned buffer, no dynamic allocation) — safe because fragmentation is
   synchronous and single-in-flight on both firmwares (see framing.c). Returns the
   fragment_count on success, or 0 if record_len is 0, exceeds FEB_MAX_RECORD_SIZE, capacity
   is 0, or the record would need more than FEB_MAX_FRAGMENTS fragments. */
typedef void (*feb_emit_fn)(const uint8_t *fragment, size_t fragment_len, void *ctx);
uint8_t feb_fragment_record(
    const uint8_t *record,
    size_t record_len,
    size_t capacity,
    uint8_t message_id,
    feb_emit_fn emit,
    void *ctx);

/* Feeds one received fragment (raw bytes as received off the wire, header + payload) into
   the per-direction reassembly state `r`.
   - The first fragment of a new message_id (r not in_progress, or fragment_index == 0 for
     a different message_id than the one in progress) starts a new reassembly; its
     fragment_count and payload length set the capacity/count expected for the rest of the
     message.
   - FEB_FRAME_MESSAGE_COMPLETE is returned when the final fragment completes the message;
     *out_record / *out_len then point into r->buffer. The caller must consume the record
     before the next feed() call, which is free to reset `r` for a new message.
   - Any rejection (FEB_FRAME_DUPLICATE_FRAGMENT, FEB_FRAME_INCONSISTENT_COUNT,
     FEB_FRAME_OVERSIZED, FEB_FRAME_OUT_OF_ORDER, FEB_FRAME_INVALID_HEADER) resets `r` to
     not-in-progress (drop the in-progress message and continue — see
     docs/PLAN.md step 3 "Malformed fragment/message handling"). The BLE connection itself
     is not affected by this layer; that decision belongs to the caller.
   - FEB_FRAME_OK is returned for a valid non-final fragment (reassembly continues). */
feb_frame_status_t feb_reassembly_feed(
    feb_reassembly_t *r,
    const uint8_t *fragment,
    size_t len,
    uint32_t now_ms,
    const uint8_t **out_record,
    size_t *out_len);

/* Call periodically (independent of feed()) with the current tick. Returns
   FEB_FRAME_TIMEOUT and resets `r` if a message has been in progress for more than
   FEB_REASSEMBLY_TIMEOUT_MS as of now_ms; otherwise FEB_FRAME_OK (including when nothing
   is in progress). */
feb_frame_status_t feb_reassembly_check_timeout(feb_reassembly_t *r, uint32_t now_ms);

#endif /* FEB_FRAMING_H */

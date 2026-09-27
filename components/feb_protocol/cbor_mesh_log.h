/* Split out of cbor_codec.h (docs/OPTIMIZATION.md item 1 pattern) -- shared contract, to be
   mirrored byte-for-byte in flipper/cbor_mesh_log.h once the Flipper side implements the
   `mesh_log` capability (Heltec-only, docs/WARDRIVING_PUBLISH.md "Mesh node publishing"
   section, design frozen 2026-09-27). Changes here must be mirrored there and in
   docs/PROTOCOL.md, or the two firmwares diverge. Included transitively via cbor_codec.h;
   nothing outside the codec split should need to include this directly.

   ---- `mesh_log`-specific `<mesh-log-record>` element and `status.result` map
   (docs/PROTOCOL.md "`mesh_log` command and status payloads") ----

   `mesh_log` has no `command` shape at all -- unlike every other capability in this codebase,
   the Flipper never sends a `command` with `capability = "mesh_log"`; the ESP32 only ever
   pushes `status` records unsolicited, exactly once per authenticated session establishment,
   mirroring `wardriving`'s own unsolicited-backlog-drain convention (reserved sentinel
   `request_id = 0`, peek/mark-drained batching against a dedicated on-flash circular log --
   see `heltec/main/mesh_log.h`). This is a deliberate, narrower scope than `wardriving`'s own
   drain (which also re-kicks after every live wifi/ble scan cycle while connected): mesh_log's
   capture happens on a different FreeRTOS task (the LoRa RX task, `lora_shared_radio.cpp`) than
   the NimBLE host task that owns the BLE session/TX state, so pushing live (not just at
   (re)connect) would need a new cross-task signal into the BLE state machine -- accepted as an
   explicit scope cut for this pass (see docs/BACKLOG.md); a node captured while a session is
   already open and fully drained waits for the next reconnect to reach the Flipper, not
   ideal but acceptable given mesh nodes are sparse and sessions reconnect periodically anyway.

   `<mesh-log-record>` field order: node_id, network, lat_e7_offset, lon_e7_offset -- all four
   always present (unlike `<meshcore-node>`'s optional lat/lon pair): `mesh_log` only ever
   records a sighting that already carries a position (`has_location == true` at capture time,
   see `heltec/main/mesh_log.c`), so there is no positionless case to represent on the wire.
   - `node_id`: 1..FEB_MESH_LOG_NODE_ID_MAX_LEN (16) hex characters. Unlike `meshcore_scan`'s
     own `<meshcore-node>.node_id` (always exactly 16), this field's length varies by
     `network` (MeshCore: 16 hex chars; Meshtastic: 8) -- this codec bounds it (non-empty, at
     most 16) rather than enforcing an exact length, leaving any per-network exactness check to
     the caller, matching `role`/`auth`/`phy`'s caller-owned-text treatment elsewhere in this
     split.
   - `network`: caller-owned text, `"meshcore"` or `"meshtastic"` -- this codec does not
     restrict its value, matching `source`/`role`'s own caller-owned-text treatment. Bounded at
     FEB_MESH_LOG_NETWORK_MAX_LEN (10, the length of the longer of the two defined values).
   - `lat_e7_offset`/`lon_e7_offset`: same `(int32_t)(lat_or_lon * 1e7) + <hemisphere-max-
     magnitude-offset>` encoding as every other lat/lon pair in this protocol (`wardriving`,
     `gps`, `meshcore_scan`) -- applied by the caller (`heltec/main/mesh_log.c`), not this
     codec.

   `status.result` = `{ "records": [ <mesh-log-record>, ... ], "backlog_remaining": <uint> }`
   -- field names and semantics both copied verbatim from `wardriving`'s own
   `status(state="data").result` shape (not `meshcore_scan`'s fixed-cap-plus-total_known_nodes
   shape): like `wardriving`, `mesh_log` has a real accumulate-then-drain backlog with no
   natural per-reply truncation point, and repeats the same "keep sending, chained via
   `backlog_remaining`, until caught up" mechanism -- but unlike `wardriving`'s dynamic
   pack-as-many-as-fit batching, `records` here never holds more than
   FEB_MESH_LOG_MAX_RECORDS_PER_BATCH (1) entries (see that constant's own comment for why).
   `backlog_remaining` is the count of buffered records still undrained after this reply, `0`
   once caught up -- identical meaning to `wardriving`'s own field.

   **Global state-string uniqueness (a wire-shape fact this design didn't originally spell
   out, confirmed 2026-09-27 by reading the Flipper's actual routing code,
   `flipper/flipper_esp32_over_ble.c`):** the Flipper routes an inbound `status` record to the
   correct capability handler by peeking only its `state` text (there is no `capability` field
   anywhere on `status`) -- so every capability's `status.state` value(s) must be unique across
   the *whole* protocol, not merely within that one capability. `wardriving` already occupies
   `"data"`; `mesh_log` therefore uses a distinct state, `"mesh_data"`, for its (always
   request_id=0, always carrying `result`) status records -- see docs/PROTOCOL.md and
   docs/WARDRIVING_PUBLISH.md's "corrected" note for the full context. */
#ifndef FEB_CBOR_MESH_LOG_H
#define FEB_CBOR_MESH_LOG_H

#include "cbor_codec.h"

#define FEB_MESH_LOG_NODE_ID_MAX_LEN 16u
#define FEB_MESH_LOG_NETWORK_MAX_LEN 10u /* len("meshtastic"), the longer of the two values */
/* Exactly 1, not `wardriving`'s dynamic pack-until-it-doesn't-fit batching, nor even a small
   fixed batch like `meshcore_scan`'s -- this board's classic-ESP32 DRAM budget was already
   down to ~120 bytes of `.dram0.bss` headroom before this capability existed (measured during
   `meshtastic_scan`'s own implementation pass, docs/SESSION_MEMORY.md's 2026-09-27 entry;
   see docs/BACKLOG.md BL23). A real per-record batch (even just 2-4 entries) needs the
   caller-supplied scratch space to hold every batched record's decoded strings
   *simultaneously* (the same reasoning wardriving_log_peek_pending()'s own scratch_buf
   parameter documents), which this board's remaining headroom cannot absorb -- so
   `heltec/main/mesh_log.c` sends the buffered backlog one record per `status(state=
   "mesh_data")` reply instead, chained via `backlog_remaining`/repeated pushes exactly like
   `wardriving` chains multiple batches, just at batch size 1. This is a wire-format-level
   constraint (not merely this firmware's own sending choice): a `mesh_log` status reply can
   never carry more than one record, on either firmware. Acceptable given mesh nodes are
   sparse (dozens, not hundreds/thousands) and this capability is not a hot/latency-sensitive
   path. */
#define FEB_MESH_LOG_MAX_RECORDS_PER_BATCH 1u

typedef struct {
    const char *node_id; /* 1..FEB_MESH_LOG_NODE_ID_MAX_LEN bytes */
    size_t node_id_len;
    const char *network; /* "meshcore" | "meshtastic" */
    size_t network_len;
    uint64_t lat_e7_offset;
    uint64_t lon_e7_offset;
} feb_mesh_log_record_t;

size_t feb_cbor_encode_mesh_log_record(uint8_t *out, size_t out_cap, const feb_mesh_log_record_t *record);
size_t feb_cbor_decode_mesh_log_record(const uint8_t *in, size_t in_len, feb_mesh_log_record_t *record, feb_cbor_status_t *status);

typedef struct {
    feb_mesh_log_record_t records[FEB_MESH_LOG_MAX_RECORDS_PER_BATCH];
    size_t record_count;
    uint64_t backlog_remaining;
} feb_mesh_log_status_result_payload_t;

size_t feb_cbor_encode_mesh_log_status_result_payload(uint8_t *out, size_t out_cap, const feb_mesh_log_status_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_mesh_log_status_result_payload(const uint8_t *in, size_t in_len, feb_mesh_log_status_result_payload_t *payload);

#endif /* FEB_CBOR_MESH_LOG_H */

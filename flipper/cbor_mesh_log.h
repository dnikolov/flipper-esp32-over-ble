/* Shared contract, mirrored byte-for-byte in components/feb_protocol/cbor_mesh_log.h (added
   there first, 2026-09-27, Heltec-only mesh node publishing, docs/WARDRIVING_PUBLISH.md "Mesh
   node publishing" section). Changes here must be mirrored there and in docs/PROTOCOL.md, or
   the two firmwares diverge. Included by the umbrella cbor_codec.h, which must be included
   first (directly or transitively) so that feb_cbor_status_t is already visible; not meant to
   be included standalone. Part of the cross-firmware shared-header contract check
   (tools/check_shared_headers.py's HEADER_PAIRS) -- that script only diffs macros/prototypes,
   not struct bodies, so the field list below was checked by hand against
   components/feb_protocol/cbor_mesh_log.h, not just by a clean script run.

   ---- `mesh_log`-specific `<mesh-log-record>` element and `status.result` map
   (docs/PROTOCOL.md "`mesh_log` command and status payloads") ----

   `mesh_log` has no `command` shape at all -- unlike every other capability in this codebase,
   the Flipper never sends a `command` with `capability = "mesh_log"`; the ESP32/Heltec only
   ever pushes `status` records unsolicited, exactly once per authenticated session
   establishment, mirroring `wardriving`'s own unsolicited-backlog-drain convention (reserved
   sentinel `request_id = 0`, peek/mark-drained batching against a dedicated on-flash circular
   log -- see `heltec/main/mesh_log.h`). This capability's capture happens on the Heltec's LoRa
   RX task, not the NimBLE host task that owns the BLE session/TX state, so it only pushes at
   (re)connect, not live mid-session (accepted scope cut, see docs/BACKLOG.md) -- a node
   captured while a session is already open and fully drained waits for the next reconnect to
   reach the Flipper.

   `<mesh-log-record>` field order: node_id, network, lat_e7_offset, lon_e7_offset -- all four
   always present (unlike `<meshcore-node>`'s optional lat/lon pair): `mesh_log` only ever
   records a sighting that already carries a position, so there is no positionless case to
   represent on the wire.
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
     `gps`, `meshcore_scan`) -- applied by the sender, not this codec.

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

   **Global state-string uniqueness:** the Flipper routes an inbound `status` record to the
   correct capability handler by peeking only its `state` text (there is no `capability` field
   anywhere on `status`) -- so every capability's `status.state` value(s) must be unique across
   the *whole* protocol, not merely within that one capability. `wardriving` already occupies
   `"data"`, `meshcore_scan` occupies `"ok"`, `gps` occupies `"no_signal"/"acquiring"/"fix"` --
   `mesh_log` therefore uses a distinct state, `"mesh_data"`, for its (always request_id=0,
   always carrying `result`) status records -- see docs/PROTOCOL.md and
   docs/WARDRIVING_PUBLISH.md. */
#ifndef FEB_CBOR_MESH_LOG_H
#define FEB_CBOR_MESH_LOG_H

#define FEB_MESH_LOG_NODE_ID_MAX_LEN 16u
#define FEB_MESH_LOG_NETWORK_MAX_LEN 10u /* len("meshtastic"), the longer of the two values */
/* Exactly 1, not `wardriving`'s dynamic pack-until-it-doesn't-fit batching, nor even a small
   fixed batch like `meshcore_scan`'s -- the Heltec's classic-ESP32 DRAM budget was already
   down to ~120 bytes of `.dram0.bss` headroom before this capability existed (see
   docs/BACKLOG.md BL23). This is a wire-format-level constraint (not merely the Heltec
   firmware's own sending choice): a `mesh_log` status reply can never carry more than one
   record, on either firmware. Acceptable given mesh nodes are sparse (dozens, not
   hundreds/thousands) and this capability is not a hot/latency-sensitive path. */
#define FEB_MESH_LOG_MAX_RECORDS_PER_BATCH 1u

typedef struct {
    const char* node_id; /* 1..FEB_MESH_LOG_NODE_ID_MAX_LEN bytes */
    size_t node_id_len;
    const char* network; /* "meshcore" | "meshtastic" */
    size_t network_len;
    uint64_t lat_e7_offset;
    uint64_t lon_e7_offset;
} feb_mesh_log_record_t;

size_t feb_cbor_encode_mesh_log_record(uint8_t* out, size_t out_cap, const feb_mesh_log_record_t* record);
size_t feb_cbor_decode_mesh_log_record(const uint8_t* in, size_t in_len, feb_mesh_log_record_t* record, feb_cbor_status_t* status);

typedef struct {
    feb_mesh_log_record_t records[FEB_MESH_LOG_MAX_RECORDS_PER_BATCH];
    size_t record_count;
    uint64_t backlog_remaining;
} feb_mesh_log_status_result_payload_t;

size_t feb_cbor_encode_mesh_log_status_result_payload(uint8_t* out, size_t out_cap, const feb_mesh_log_status_result_payload_t* payload);
feb_cbor_status_t feb_cbor_decode_mesh_log_status_result_payload(const uint8_t* in, size_t in_len, feb_mesh_log_status_result_payload_t* payload);

#endif /* FEB_CBOR_MESH_LOG_H */

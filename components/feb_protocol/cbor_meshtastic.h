/* Split out of cbor_codec.h (docs/OPTIMIZATION.md item 1 pattern) -- shared contract, to be
   mirrored byte-for-byte in flipper/cbor_meshtastic.h once the Flipper side implements the
   `meshtastic_scan` capability (Heltec-only, docs/PLAN.md Phase 4's "Meshtastic Scan
   Capability" design, Phase 1: detection + display, mirroring meshcore_scan's own Phase 1
   tier and cbor_meshcore.h's own codec shape as closely as Meshtastic's actual on-air data
   allows). Changes here must be mirrored there and in docs/PROTOCOL.md, or the two firmwares
   diverge. Included transitively via cbor_codec.h; nothing outside the codec split should
   need to include this directly.

   ---- `meshtastic_scan`-specific `<meshtastic-node>` element and `status.result` map
   (docs/PROTOCOL.md "`meshtastic_scan` command and status payloads") ----

   `meshtastic_scan` is poll-only, same shape as `meshcore_scan`: a single `status` action, no
   start/stop, no busy/exclusivity concept. `command`'s `arguments` is always an empty map.

   `<meshtastic-node>` field order: node_id, name (optional), rssi_offset, last_seen_ms.
   Deliberately smaller than `<meshcore-node>` -- no `role`, no `lat_e7_offset`/
   `lon_e7_offset` -- because Meshtastic's raw packet header (always sent unencrypted, unlike
   MeshCore's signed-but-plaintext ADVERT) carries neither; getting either would require
   decoding a POSITION_APP payload (out of Phase 1 scope, see meshtastic_proto.h/main.c).
   - `node_id`: exactly FEB_MESHTASTIC_NODE_ID_LEN (8) hex characters -- Meshtastic's own
     32-bit NodeNum, lowercase-hex, matching the app's own on-screen "!<8 hex>" convention
     minus the "!" (same plain-hex-field treatment cbor_meshcore.h's node_id gets). Always
     present -- derived from the packet's cleartext header, never requires decryption.
   - `name`: optional, `has_name` flag, same omitted-not-empty convention as
     `<meshcore-node>.name`/`<device-result>.name`. Capped at FEB_MESHTASTIC_NAME_MAX_LEN (24)
     -- meshtastic_proto.c truncates a longer parsed name before it reaches this codec. Only
     ever present for a node heard on Meshtastic's default "LongFast" channel whose payload
     this board could actually decrypt+parse (see meshtastic_proto.c) -- a node on a private
     channel this board has no key for is still reported (node_id/rssi/last_seen), just always
     without a name.
   - `rssi_offset`: `rssi_dbm + 128`, identical convention to every other RSSI-bearing field in
     this protocol.
   - `last_seen_ms`: milliseconds since ESP32 boot, same boot-relative convention as
     `<meshcore-node>.last_seen_ms` and for the same reason (no RTC on this board).

   **Sizing note**, same deliberately-conservative-single-shot-reply methodology as
   cbor_meshcore.h (no partial/complete streaming lifecycle): FEB_MESHTASTIC_MAX_NODES_PER_RESULT
   must provably fit inside FEB_CBOR_MAX_PAYLOAD (512 bytes) at every field's own simultaneous
   worst-case length (8-char node_id, 24-char name, rssi_offset==255, last_seen_ms at its
   longest CBOR encoding) -- confirmed by a host-native test
   (tests/esp32/test_framing_cbor.c) that encodes exactly this many maximally-sized nodes and
   asserts the result stays within FEB_CBOR_MAX_PAYLOAD, same as cbor_meshcore.h's own
   precedent.

   `status.result` = `{ "nodes": [ <meshtastic-node>, ... ], "total_known_nodes": <uint> }`,
   field order and `total_known_nodes` semantics identical to cbor_meshcore.h's own (see that
   header for the full rationale -- not repeated here). `status.state` is always `"ok"`;
   `result` is therefore always present, same as `meshcore_scan`. */
#ifndef FEB_CBOR_MESHTASTIC_H
#define FEB_CBOR_MESHTASTIC_H

#include "cbor_codec.h"

#define FEB_MESHTASTIC_NODE_ID_LEN 8u
/* 8, not the 24 cbor_meshcore.h's own FEB_MESHCORE_NAME_MAX_LEN uses -- this board's
   classic-ESP32 DRAM budget forced a smaller cap once meshtastic_table.h's node table was
   added alongside meshcore_table.h's existing one; see that header's sizing note for the
   measured numbers. Also matches this capability's own choice to decode Meshtastic's compact
   `short_name` field rather than the free-form `long_name` (meshtastic_proto.c) -- 8
   characters is generous headroom for a field that's conventionally <=4 characters. */
#define FEB_MESHTASTIC_NAME_MAX_LEN 8u
/* Computed and test-confirmed (see this header's top comment) against FEB_CBOR_MAX_PAYLOAD's
   512-byte cap at every field's simultaneous worst-case length -- NOT derived from the node
   table's own capacity (meshtastic_table.h); a table holding more than this many nodes is
   reported truncated via `total_known_nodes`. Smaller than cbor_meshcore.h's own
   FEB_MESHCORE_MAX_NODES_PER_RESULT (3) purely for this board's DRAM-budget reason (main.c's
   per-command scratch scales with this constant), not a wire-size constraint -- a larger
   value would still fit well under 512 bytes, given this capability's smaller per-node
   encoding than meshcore's. Deliberately kept below meshtastic_table.h's own
   MESHTASTIC_TABLE_MAX_ENTRIES so a fuller table can still show up as
   `total_known_nodes > nodes.length`, not just as an untruncated echo of the whole table. */
#define FEB_MESHTASTIC_MAX_NODES_PER_RESULT 2u

typedef struct {
    const char *node_id; /* exactly FEB_MESHTASTIC_NODE_ID_LEN bytes */
    size_t node_id_len;
    const char *name; /* NULL if has_name is 0; 0..FEB_MESHTASTIC_NAME_MAX_LEN bytes otherwise */
    size_t name_len;
    int has_name;
    uint64_t rssi_offset; /* rssi_dbm + 128; encoder/decoder reject a value > 255 */
    uint64_t last_seen_ms;
} feb_meshtastic_node_t;

size_t feb_cbor_encode_meshtastic_node(uint8_t *out, size_t out_cap, const feb_meshtastic_node_t *node);
size_t feb_cbor_decode_meshtastic_node(const uint8_t *in, size_t in_len, feb_meshtastic_node_t *node, feb_cbor_status_t *status);

typedef struct {
    feb_meshtastic_node_t nodes[FEB_MESHTASTIC_MAX_NODES_PER_RESULT];
    size_t node_count;
    uint64_t total_known_nodes;
} feb_meshtastic_status_result_payload_t;

size_t feb_cbor_encode_meshtastic_status_result_payload(uint8_t *out, size_t out_cap, const feb_meshtastic_status_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_meshtastic_status_result_payload(const uint8_t *in, size_t in_len, feb_meshtastic_status_result_payload_t *payload);

#endif /* FEB_CBOR_MESHTASTIC_H */

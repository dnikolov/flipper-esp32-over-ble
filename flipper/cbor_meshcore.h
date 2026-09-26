/* Shared contract, mirrored byte-for-byte in components/feb_protocol/cbor_meshcore.h (added
   there first, 2026-09-26, Heltec-only MeshCore Scan Phase 1 "detection + display"). Changes
   here must be mirrored there and in docs/PROTOCOL.md, or the two firmwares diverge. Included
   by the umbrella cbor_codec.h, which must be included first (directly or transitively) so
   that feb_cbor_status_t is already visible; not meant to be included standalone. Part of the
   cross-firmware shared-header contract check (tools/check_shared_headers.py's
   HEADER_PAIRS) -- that script only diffs macros/prototypes, not struct bodies, so the field
   list below was checked by hand against components/feb_protocol/cbor_meshcore.h, not just by
   a clean script run.

   ---- `meshcore_scan`-specific `<meshcore-node>` element and `status.result` map
   (docs/PROTOCOL.md "`meshcore_scan` command and status payloads") ----

   `meshcore_scan` is poll-only -- a single `status` action, no start/stop, no busy/exclusivity
   concept (the LoRa radio's continuous receive task runs unconditionally from boot,
   independent of the Wi-Fi/BLE radio the other capabilities contend over). `command`'s
   `arguments` is always an empty map, matching `gps`/`wifi_scan`/`ble_scan`'s established
   convention exactly -- no `action` field, since this capability has exactly one operation.

   `<meshcore-node>` field order: node_id, name (optional), role, rssi_offset, last_seen_ms,
   lat_e7_offset (optional, paired with lon_e7_offset), lon_e7_offset (optional, paired with
   lat_e7_offset).
   - `node_id`: exactly FEB_MESHCORE_NODE_ID_LEN (16) hex characters -- the first 16 hex chars
     of the ADVERT's 32-byte Ed25519 pubkey. Fixed-length text, same exact-length-on-decode
     treatment as `bssid`/`address` get for their exact-length *byte* fields, applied here to a
     *text* field instead.
   - `name`: optional, `has_name` flag mirrors `ble_scan`'s `<device-result>.name` convention
     exactly -- omitted from the wire entirely (not an empty string) when absent. Capped at
     FEB_MESHCORE_NAME_MAX_LEN (24).
   - `role`: caller-owned text ("chat"/"repeater"/"room_server"/"sensor"/"unknown") -- this
     codec does not restrict its value, matching `phy`/`auth`/`addr_type`'s caller-owned-text
     treatment elsewhere.
   - `rssi_offset`: `rssi_dbm + 128`, identical convention to `wifi_scan`/`ble_scan`/
     `wardriving` -- but sourced from the LoRa radio's own per-frame RSSI reading, not anything
     carried in the MeshCore packet itself.
   - `last_seen_ms`: milliseconds since the board's own boot -- this board has no RTC, so there
     is no wall-clock alternative.
   - `lat_e7_offset`/`lon_e7_offset`: present only when the ADVERT's appdata carried a
     position -- `has_location` (struct-only, not itself a wire field) mirrors `wardriving`'s
     `ble_window_ms`/`ble_interval_ms` pairing-enforcement pattern: seeing exactly one of the
     two keys on decode is FEB_CBOR_ERR_MISSING_FIELD, not merely "the other one defaults to
     absent". Same offset-by-max-magnitude encoding as `wardriving`/`gps`'s own lat/lon fields.

   **Sizing note (deliberately conservative, unlike wifi_scan/ble_scan's dynamic
   pack-until-it-doesn't-fit batching):** this capability has no partial/complete streaming
   lifecycle -- a single `status` reply is the whole design -- so
   FEB_MESHCORE_MAX_NODES_PER_RESULT must be a number that provably fits inside
   FEB_CBOR_MAX_PAYLOAD (512 bytes) simultaneously at every field's own worst-case declared
   length. `total_known_nodes` exists specifically so a table that exceeds this bound is still
   honestly reported as truncated rather than silently under-representing what the radio has
   actually heard.

   `status.result` = `{ "nodes": [ <meshcore-node>, ... ], "total_known_nodes": <uint> }`.
   `total_known_nodes` is the node table's current total occupied-entry count (>=
   `nodes.length`) -- mirrors `wardriving`'s `backlog_remaining` field exactly. Field order:
   nodes, total_known_nodes. `status.state` is always `"ok"` for `meshcore_scan` -- there is no
   multi-state lifecycle like `gps`'s no_signal/acquiring/fix or `wifi_scan`/`ble_scan`'s
   partial/complete; `result` is therefore always present (`has_result` always 1), unlike every
   other capability's status payload in this codebase. */
#ifndef FEB_CBOR_MESHCORE_H
#define FEB_CBOR_MESHCORE_H

#define FEB_MESHCORE_NODE_ID_LEN 16u
#define FEB_MESHCORE_NAME_MAX_LEN 24u
/* Computed and test-confirmed (see this header's top comment) against FEB_CBOR_MAX_PAYLOAD's
   512-byte cap at every field's simultaneous worst-case length -- NOT derived from the
   64-entry node table's own capacity, which is much larger; a table holding more than this
   many nodes is reported truncated via `total_known_nodes`. */
#define FEB_MESHCORE_MAX_NODES_PER_RESULT 3u

typedef struct {
    const char *node_id; /* exactly FEB_MESHCORE_NODE_ID_LEN bytes */
    size_t node_id_len;
    const char *name; /* NULL if has_name is 0; 0..FEB_MESHCORE_NAME_MAX_LEN bytes otherwise */
    size_t name_len;
    int has_name;
    const char *role; /* "chat" | "repeater" | "room_server" | "sensor" | "unknown" */
    size_t role_len;
    uint64_t rssi_offset; /* rssi_dbm + 128; encoder/decoder reject a value > 255 */
    uint64_t last_seen_ms;
    uint64_t lat_e7_offset; /* meaningful only if has_location */
    uint64_t lon_e7_offset; /* meaningful only if has_location */
    int has_location; /* lat_e7_offset/lon_e7_offset always present or absent together */
} feb_meshcore_node_t;

size_t feb_cbor_encode_meshcore_node(uint8_t *out, size_t out_cap, const feb_meshcore_node_t *node);
size_t feb_cbor_decode_meshcore_node(const uint8_t *in, size_t in_len, feb_meshcore_node_t *node, feb_cbor_status_t *status);

typedef struct {
    feb_meshcore_node_t nodes[FEB_MESHCORE_MAX_NODES_PER_RESULT];
    size_t node_count;
    uint64_t total_known_nodes;
} feb_meshcore_status_result_payload_t;

size_t feb_cbor_encode_meshcore_status_result_payload(uint8_t *out, size_t out_cap, const feb_meshcore_status_result_payload_t *payload);
feb_cbor_status_t feb_cbor_decode_meshcore_status_result_payload(const uint8_t *in, size_t in_len, feb_meshcore_status_result_payload_t *payload);

#endif /* FEB_CBOR_MESHCORE_H */

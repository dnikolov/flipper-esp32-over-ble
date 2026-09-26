/* Split out of cbor_codec.h (docs/OPTIMIZATION.md item 1 pattern) -- shared contract, to be
   mirrored byte-for-byte in flipper/cbor_meshcore.h once the Flipper side implements the
   `meshcore_scan` capability (Heltec-only, docs/PLAN.md Phase 4's "MeshCore Scan Capability"
   design, Phase 1: detection + display). Changes here must be mirrored there and in
   docs/PROTOCOL.md, or the two firmwares diverge. Included transitively via cbor_codec.h;
   nothing outside the codec split should need to include this directly.

   ---- `meshcore_scan`-specific `<meshcore-node>` element and `status.result` map
   (docs/PROTOCOL.md "`meshcore_scan` command and status payloads") ----

   `meshcore_scan` is poll-only -- a single `status` action, no start/stop, no busy/exclusivity
   concept (the LoRa radio's continuous receive task runs unconditionally from boot,
   independent of the Wi-Fi/BLE radio the other capabilities contend over -- see
   meshcore_radio.h). `command`'s `arguments` is always an empty map, matching
   `gps`/`wifi_scan`/`ble_scan`'s established convention exactly (this codebase has no
   existing capability that wire-encodes an explicit "action" string for a single-operation
   capability -- that only exists for `wardriving`, which genuinely has two operations
   (start/stop) to distinguish; `meshcore_scan` has one, so it needs no such field).

   `<meshcore-node>` field order: node_id, name (optional), role, rssi_offset, last_seen_ms,
   lat_e7_offset (optional, paired with lon_e7_offset), lon_e7_offset (optional, paired with
   lat_e7_offset).
   - `node_id`: exactly FEB_MESHCORE_NODE_ID_LEN (16) hex characters -- the first 16 hex chars
     of the ADVERT's 32-byte Ed25519 pubkey, matching the community wdgwars feeder tools'
     convention (see the design plan). Fixed-length text, same exact-length-on-decode
     treatment as `bssid`/`address` (cbor_wifi_scan.h/cbor_ble_scan.h) get for their
     exact-length *byte* fields, applied here to a *text* field instead.
   - `name`: optional, `has_name` flag mirrors `ble_scan`'s `<device-result>.name` convention
     exactly (cbor_ble_scan.h) -- omitted from the wire entirely (not an empty string) when
     the ADVERT's appdata carried no name (flags byte bit 0x80 clear). Capped at
     FEB_MESHCORE_NAME_MAX_LEN (24) -- meshcore_proto.c truncates a longer parsed name before
     it ever reaches this codec; this is a deliberately conservative bound (see
     FEB_MESHCORE_MAX_NODES_PER_RESULT below for why every field here is kept short) rather
     than the ADVERT payload's own much larger structural ceiling.
   - `role`: caller-owned text ("chat"/"repeater"/"room_server"/"sensor"/"unknown" per
     meshcore_proto.c's flags-byte-nibble mapping) -- this codec does not restrict its value,
     matching `phy`/`auth`/`addr_type`'s caller-owned-text treatment elsewhere in this split.
   - `rssi_offset`: `rssi_dbm + 128`, identical convention to `wifi_scan`/`ble_scan`/
     `wardriving` -- but sourced from the LoRa radio's own per-frame RSSI reading (RadioLib),
     not anything carried in the MeshCore packet itself.
   - `last_seen_ms`: milliseconds since ESP32 boot (`esp_timer_get_time() / 1000`), same
     boot-relative convention as `wardriving`'s `timestamp_ms` record field -- this board has
     no RTC, so there is no wall-clock alternative (MeshCore ADVERTs do carry their own
     timestamp field, but it is the *sender's* clock, not verified/synchronized against this
     board's, so it is deliberately not surfaced here to avoid implying a trust property this
     capability's "no signature verification" scope cut does not support).
   - `lat_e7_offset`/`lon_e7_offset`: present only when the ADVERT's appdata carried a
     position (flags byte bit 0x10 set) -- `has_location` (struct-only, not itself a wire
     field) mirrors `wardriving`'s `ble_window_ms`/`ble_interval_ms` pairing-enforcement
     pattern exactly (cbor_wardriving.h/.c): seeing exactly one of the two keys on decode is
     FEB_CBOR_ERR_MISSING_FIELD, not merely "the other one defaults to absent". Same
     offset-by-max-magnitude encoding as `wardriving`/`gps`'s own lat/lon fields
     (`(int32_t)(lat * 1e7) + 900000000`, `(int32_t)(lon * 1e7) + 1800000000`) -- applied by
     the caller (main.c), not this codec, matching that same precedent exactly (this codec
     does not itself validate the offset's magnitude, mirroring cbor_gps.c's lat_e7_offset/
     lon_e7_offset treatment).

   **Sizing note (deliberately conservative, unlike wifi_scan/ble_scan's dynamic
   pack-until-it-doesn't-fit batching in main.c):** this capability has no partial/complete
   streaming lifecycle (a single `status` reply is the whole design, per the design plan) --
   so FEB_MESHCORE_MAX_NODES_PER_RESULT must be a number that provably fits inside
   FEB_CBOR_MAX_PAYLOAD (512 bytes) *simultaneously at every field's own worst-case declared
   length* (16-char node_id, 24-char name, 11-char "room_server" role, rssi_offset==255,
   last_seen_ms needing its longest CBOR encoding, both lat/lon present at their max offset),
   not merely "typical" sizes -- there is no fallback batching path to fall back on if a
   real-world table blows this budget. `total_known_nodes` (see below) exists specifically so
   a table that exceeds this bound is still honestly reported as truncated rather than
   silently under-representing what the radio has actually heard. This bound and the
   worst-case-fits-in-512-bytes claim are both confirmed by a host-native test
   (tests/esp32/test_framing_cbor.c) that encodes exactly this many maximally-sized nodes and
   asserts the result stays within FEB_CBOR_MAX_PAYLOAD -- treat the constant below as
   load-bearing on that test passing, not just on the doc comment's arithmetic.

   `status.result` = `{ "nodes": [ <meshcore-node>, ... ], "total_known_nodes": <uint> }`.
   `total_known_nodes` is the node table's current total occupied-entry count (>=
   `nodes.length`) -- mirrors `wardriving`'s `backlog_remaining` field exactly: an independent
   piece of information not recoverable from the array's own length, letting the Flipper
   detect a truncated listing (`total_known_nodes > nodes.length`) rather than silently
   showing an incomplete node list with no indication anything was omitted. Field order:
   nodes, total_known_nodes (matches `wardriving`'s records/backlog_remaining order exactly).
   `status.state` is always `"ok"` for `meshcore_scan` -- there is no multi-state lifecycle
   like `gps`'s no_signal/acquiring/fix or `wifi_scan`/`ble_scan`'s partial/complete;
   `result` is therefore always present (`has_result` always 1), unlike every other
   capability's status payload in this codebase. */
#ifndef FEB_CBOR_MESHCORE_H
#define FEB_CBOR_MESHCORE_H

#include "cbor_codec.h"

#define FEB_MESHCORE_NODE_ID_LEN 16u
#define FEB_MESHCORE_NAME_MAX_LEN 24u
/* Computed and test-confirmed (see this header's top comment) against
   FEB_CBOR_MAX_PAYLOAD's 512-byte cap at every field's simultaneous worst-case length --
   NOT derived from the 64-entry node table's own capacity (meshcore_table.h), which is much
   larger; a table holding more than this many nodes is reported truncated via
   `total_known_nodes`. */
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

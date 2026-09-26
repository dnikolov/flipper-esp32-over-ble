/* MeshCore (a third-party open LoRa mesh-network protocol, unrelated to this project's own
   BLE protocol) packet parser -- board-local, no dependency on components/feb_protocol/ or
   ESP-IDF (pure C, host-testable; see tests/heltec/test_meshcore_proto.c). Confirmed against
   docs.meshcore.io's packet_format.md/payloads.md (this session's own web research, not
   assumed from the design plan alone): header byte layout, route-type-dependent transport
   codes, path-length byte, and the ADVERT payload's pubkey/timestamp/signature/appdata
   layout.

   Scope (docs/PLAN.md's "MeshCore Scan Capability" design, Phase 1): only payload type
   ADVERT (0x04) is decoded -- every other payload type (req/response/txt_msg/ack/etc.) is
   deliberately left unparsed and this parser returns false for it. The ADVERT's 64-byte
   Ed25519 signature is read past (skipped) but NEVER verified -- this capability does
   presence detection over passively-observed broadcast data, not mesh participation, and
   explicitly does not attempt to authenticate what it reports; see docs/CAPABILITIES.md's
   `meshcore_scan` entry for this scope cut spelled out for the wire-protocol audience too. */
#ifndef FEB_MESHCORE_PROTO_H
#define FEB_MESHCORE_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hex-character count (not bytes) of the node_id derived from the first 8 bytes of the
   ADVERT's 32-byte Ed25519 pubkey -- matches the community wdgwars feeder tools' convention
   (docs/PLAN.md's design plan). */
#define MESHCORE_NODE_ID_HEX_LEN 16u

/* Parser-local cap on the ADVERT appdata's optional name field. Deliberately independent of
   components/feb_protocol/cbor_meshcore.h's own FEB_MESHCORE_NAME_MAX_LEN -- this file has
   no dependency on that component (see top comment), so it cannot reuse that constant
   directly -- but set to the same value (24) on purpose: a real hardware-DRAM-budget check
   this session (see meshcore_table.h's sizing note) ruled out carrying any extra margin
   above the wire codec's own bound through this board's whole meshcore_scan stack, so this
   is a single source of truth for "the longest name this capability will ever keep," not a
   generous-margin guess. main.c still re-clamps to FEB_MESHCORE_NAME_MAX_LEN defensively
   when building a feb_meshcore_node_t, since the two constants are independently defined and
   nothing enforces they stay equal if either changes in the future. */
#define MESHCORE_NAME_MAX_LEN 24u

typedef enum {
    MESHCORE_ROLE_UNKNOWN = 0,
    MESHCORE_ROLE_CHAT = 1,
    MESHCORE_ROLE_REPEATER = 2,
    MESHCORE_ROLE_ROOM_SERVER = 3,
    MESHCORE_ROLE_SENSOR = 4,
} meshcore_role_t;

typedef struct {
    char node_id_hex[MESHCORE_NODE_ID_HEX_LEN + 1u]; /* NUL-terminated lowercase hex */
    meshcore_role_t role;
    bool has_name;
    char name[MESHCORE_NAME_MAX_LEN + 1u]; /* NUL-terminated; meaningful only if has_name */
    bool has_location;
    int32_t lat_e7; /* meaningful only if has_location; already converted from the wire's
                        decimal-degrees*1e6 to this project's lat_e7/lon_e7 (*1e7)
                        convention (gps/wardriving) -- see meshcore_proto.c */
    int32_t lon_e7;
} meshcore_advert_t;

/* Parses one raw LoRa PHY payload (as returned by RadioLib's readData(), the whole frame
   MeshCore transmitted -- header, optional transport codes, path, and payload back-to-back)
   into *out. Returns true and populates *out only for a structurally valid frame whose
   payload type is ADVERT; returns false (leaving *out zeroed) for any other payload type, a
   truncated/malformed frame, or a path/payload length that doesn't fit within frame_len --
   never reads past frame[0..frame_len). Never verifies the ADVERT's signature (see top
   comment). */
bool meshcore_proto_parse(const uint8_t *frame, size_t frame_len, meshcore_advert_t *out);

/* "chat" | "repeater" | "room_server" | "sensor" | "unknown" (any role nibble value the
   ADVERT's appdata flags byte didn't define, including 0x00) -- matches
   components/feb_protocol/cbor_meshcore.h's `role` field's caller-owned-text convention. */
const char *meshcore_role_to_string(meshcore_role_t role);

#endif /* FEB_MESHCORE_PROTO_H */

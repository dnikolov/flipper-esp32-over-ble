#include "meshcore_proto.h"

#include <string.h>

/* Route type (header bits 0-1) -- docs.meshcore.io/packet_format's ROUTE_TYPE_* enum.
   TRANSPORT_FLOOD/TRANSPORT_DIRECT carry 4 bytes of transport codes right after the header
   (region_code/sub_region_code, both little-endian uint16); this parser skips them
   unconditionally since it never participates in regional flood suppression. */
#define MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD 0x00u
#define MESHCORE_ROUTE_TYPE_TRANSPORT_DIRECT 0x03u
#define MESHCORE_TRANSPORT_CODE_LEN 4u

/* Payload type (header bits 2-5) -- only ADVERT is decoded (see meshcore_proto.h). */
#define MESHCORE_PAYLOAD_TYPE_ADVERT 0x04u

/* Path-length byte: bits 0-5 = hop count (0-63), bits 6-7 = hash-size code
   (hash_size = code + 1 byte; 0b11 is documented "reserved" but this parser still honors
   the code+1 formula rather than special-casing it, since it only needs a byte count to
   skip, not to interpret the path's contents). MAX_PATH_SIZE/MAX_PACKET_PAYLOAD are
   docs.meshcore.io's own documented bounds. */
#define MESHCORE_PATH_HOP_COUNT_MASK 0x3Fu
#define MESHCORE_PATH_HASH_SIZE_SHIFT 6u
#define MESHCORE_MAX_PATH_SIZE 64u
#define MESHCORE_MAX_PACKET_PAYLOAD 184u

/* ADVERT payload's fixed-length prefix (docs.meshcore.io/payloads.md). */
#define MESHCORE_ADVERT_PUBKEY_LEN 32u
#define MESHCORE_ADVERT_TIMESTAMP_LEN 4u
#define MESHCORE_ADVERT_SIGNATURE_LEN 64u
#define MESHCORE_ADVERT_FIXED_LEN \
    (MESHCORE_ADVERT_PUBKEY_LEN + MESHCORE_ADVERT_TIMESTAMP_LEN + MESHCORE_ADVERT_SIGNATURE_LEN)

/* Appdata flags byte -- low nibble is the role (values 1-4 are defined; 0x00 and any
   undefined nibble value map to MESHCORE_ROLE_UNKNOWN), high nibble is a set of independent
   presence bits for the optional fields that follow, in this fixed order. */
#define MESHCORE_APPDATA_FLAG_ROLE_MASK 0x0Fu
#define MESHCORE_APPDATA_FLAG_LOCATION 0x10u
#define MESHCORE_APPDATA_FLAG_FEATURE1 0x20u
#define MESHCORE_APPDATA_FLAG_FEATURE2 0x40u
#define MESHCORE_APPDATA_FLAG_NAME 0x80u
#define MESHCORE_APPDATA_LOCATION_FIELD_LEN 4u /* each of latitude/longitude */
#define MESHCORE_APPDATA_FEATURE_FIELD_LEN 2u  /* each of feature1/feature2 */

/* MeshCore's ADVERT location fields are decimal_degrees * 1,000,000 (docs.meshcore.io/
   payloads.md); this project's own lat_e7/lon_e7 convention (gps/wardriving,
   components/feb_protocol/cbor_gps.h & cbor_wardriving.h) is decimal_degrees * 10,000,000 --
   one order of magnitude finer. Converting once here (rather than carrying the *1e6 value
   any further into meshcore_table.c/main.c) keeps every other board-side file in this
   capability's stack speaking the same lat_e7/lon_e7 units the rest of the codebase already
   uses. */
#define MESHCORE_LOCATION_E6_TO_E7_SCALE 10

static int32_t read_i32_le(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                 ((uint32_t)p[3] << 24);
    return (int32_t)u;
}

static void pubkey_prefix_to_hex(const uint8_t *pubkey, char *out_hex)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < MESHCORE_NODE_ID_HEX_LEN / 2u; i++) {
        out_hex[i * 2u] = digits[(pubkey[i] >> 4) & 0x0Fu];
        out_hex[i * 2u + 1u] = digits[pubkey[i] & 0x0Fu];
    }
    out_hex[MESHCORE_NODE_ID_HEX_LEN] = '\0';
}

const char *meshcore_role_to_string(meshcore_role_t role)
{
    switch (role) {
    case MESHCORE_ROLE_CHAT: return "chat";
    case MESHCORE_ROLE_REPEATER: return "repeater";
    case MESHCORE_ROLE_ROOM_SERVER: return "room_server";
    case MESHCORE_ROLE_SENSOR: return "sensor";
    case MESHCORE_ROLE_UNKNOWN:
    default: return "unknown";
    }
}

bool meshcore_proto_parse(const uint8_t *frame, size_t frame_len, meshcore_advert_t *out)
{
    size_t pos;
    uint8_t header;
    uint8_t route_type;
    uint8_t payload_type;
    uint8_t path_len_byte;
    uint8_t hop_count;
    uint8_t hash_size;
    size_t path_bytes;
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *appdata;
    size_t appdata_len;
    size_t appdata_pos;
    uint8_t flags;

    if (frame == NULL || out == NULL || frame_len < 1u) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    pos = 0;
    header = frame[pos++];
    route_type = header & 0x03u;
    payload_type = (header >> 2) & 0x0Fu;
    /* payload version (header bits 6-7) is not currently used by this parser. */

    if (route_type == MESHCORE_ROUTE_TYPE_TRANSPORT_FLOOD ||
        route_type == MESHCORE_ROUTE_TYPE_TRANSPORT_DIRECT) {
        if (frame_len < pos + MESHCORE_TRANSPORT_CODE_LEN) {
            return false;
        }
        pos += MESHCORE_TRANSPORT_CODE_LEN;
    }

    if (frame_len < pos + 1u) {
        return false;
    }
    path_len_byte = frame[pos++];
    hop_count = path_len_byte & MESHCORE_PATH_HOP_COUNT_MASK;
    hash_size = (uint8_t)(((path_len_byte >> MESHCORE_PATH_HASH_SIZE_SHIFT) & 0x03u) + 1u);
    path_bytes = (size_t)hop_count * (size_t)hash_size;
    if (path_bytes > MESHCORE_MAX_PATH_SIZE || frame_len < pos + path_bytes) {
        return false;
    }
    pos += path_bytes;

    if (payload_type != MESHCORE_PAYLOAD_TYPE_ADVERT) {
        return false; /* deliberately unparsed -- see meshcore_proto.h's scope note */
    }

    payload = frame + pos;
    payload_len = frame_len - pos;
    if (payload_len > MESHCORE_MAX_PACKET_PAYLOAD || payload_len < MESHCORE_ADVERT_FIXED_LEN + 1u) {
        /* Too big for MeshCore's own documented max, or too small to even hold the
           mandatory appdata flags byte ("Flags | 1 byte | Always present" per
           docs.meshcore.io/payloads.md) after pubkey+timestamp+signature. */
        return false;
    }

    pubkey_prefix_to_hex(payload, out->node_id_hex);
    /* timestamp (payload+32..+36, little-endian) and the 64-byte signature are both read
       past but not surfaced/verified -- see meshcore_proto.h's top comment and
       components/feb_protocol/cbor_meshcore.h's comment on why last_seen_ms uses this
       board's own boot-relative clock instead of the sender's unauthenticated one. */

    appdata = payload + MESHCORE_ADVERT_FIXED_LEN;
    appdata_len = payload_len - MESHCORE_ADVERT_FIXED_LEN;
    appdata_pos = 0;

    flags = appdata[appdata_pos++];
    switch (flags & MESHCORE_APPDATA_FLAG_ROLE_MASK) {
    case MESHCORE_ROLE_CHAT: out->role = MESHCORE_ROLE_CHAT; break;
    case MESHCORE_ROLE_REPEATER: out->role = MESHCORE_ROLE_REPEATER; break;
    case MESHCORE_ROLE_ROOM_SERVER: out->role = MESHCORE_ROLE_ROOM_SERVER; break;
    case MESHCORE_ROLE_SENSOR: out->role = MESHCORE_ROLE_SENSOR; break;
    default: out->role = MESHCORE_ROLE_UNKNOWN; break;
    }

    if (flags & MESHCORE_APPDATA_FLAG_LOCATION) {
        int32_t lat_e6;
        int32_t lon_e6;
        int64_t lat_e7_wide;
        int64_t lon_e7_wide;

        if (appdata_pos + (2u * MESHCORE_APPDATA_LOCATION_FIELD_LEN) > appdata_len) {
            return false;
        }
        lat_e6 = read_i32_le(appdata + appdata_pos);
        appdata_pos += MESHCORE_APPDATA_LOCATION_FIELD_LEN;
        lon_e6 = read_i32_le(appdata + appdata_pos);
        appdata_pos += MESHCORE_APPDATA_LOCATION_FIELD_LEN;

        /* Widen to int64 before scaling -- lat_e6/lon_e6 come straight off the wire from an
           ADVERT whose signature is never verified (scope cut, see meshcore_proto.h), so a
           malicious or corrupted sender could supply a value large enough that *10 overflows
           plain int32 arithmetic (undefined behavior in C). Bounds-check the widened result
           against the physically valid decimal-degree range (+-90 deg / +-180 deg, i.e.
           +-900,000,000 / +-1,800,000,000 in this project's lat_e7/lon_e7 units) and drop the
           location entirely (has_location stays false) rather than store or propagate an
           implausible value -- this also protects cbor_meshcore.h's single-shot wire-payload
           sizing guarantee, which assumes lat_e7_offset/lon_e7_offset stay within their
           documented offset ranges. */
        lat_e7_wide = (int64_t)lat_e6 * MESHCORE_LOCATION_E6_TO_E7_SCALE;
        lon_e7_wide = (int64_t)lon_e6 * MESHCORE_LOCATION_E6_TO_E7_SCALE;
        if (lat_e7_wide >= -900000000LL && lat_e7_wide <= 900000000LL &&
            lon_e7_wide >= -1800000000LL && lon_e7_wide <= 1800000000LL) {
            out->has_location = true;
            out->lat_e7 = (int32_t)lat_e7_wide;
            out->lon_e7 = (int32_t)lon_e7_wide;
        }
    }

    if (flags & MESHCORE_APPDATA_FLAG_FEATURE1) {
        if (appdata_pos + MESHCORE_APPDATA_FEATURE_FIELD_LEN > appdata_len) {
            return false;
        }
        appdata_pos += MESHCORE_APPDATA_FEATURE_FIELD_LEN; /* reserved -- consumed, ignored */
    }

    if (flags & MESHCORE_APPDATA_FLAG_FEATURE2) {
        if (appdata_pos + MESHCORE_APPDATA_FEATURE_FIELD_LEN > appdata_len) {
            return false;
        }
        appdata_pos += MESHCORE_APPDATA_FEATURE_FIELD_LEN; /* reserved -- consumed, ignored */
    }

    if (flags & MESHCORE_APPDATA_FLAG_NAME) {
        size_t name_len = (appdata_pos <= appdata_len) ? (appdata_len - appdata_pos) : 0u;

        if (name_len > MESHCORE_NAME_MAX_LEN) {
            name_len = MESHCORE_NAME_MAX_LEN; /* truncate; see meshcore_proto.h's comment */
        }
        memcpy(out->name, appdata + appdata_pos, name_len);
        out->name[name_len] = '\0';
        out->has_name = true;
    }

    return true;
}

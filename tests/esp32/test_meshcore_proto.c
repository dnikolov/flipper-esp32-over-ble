/* Host-native test driver for heltec/main/meshcore_proto.c -- the pure, zero-ESP-IDF-
   dependency MeshCore packet parser underneath meshcore_radio.cpp's real SX1276/RadioLib
   driver (docs/PLAN.md's "MeshCore Scan Capability -- Heltec Board (Phase 1)" design plan).

   No real MeshCore hardware/node was available this session to capture genuine over-the-air
   packets from (see that design plan's own note) -- every frame below is hand-constructed
   from docs.meshcore.io's packet_format.md/payloads.md field layout (confirmed via this
   session's own web research, cited in meshcore_proto.c's header comments), not captured
   from a real device. Treat this as "the parser does what the spec says it should" evidence,
   not as proof it correctly decodes whatever a real MeshCore firmware actually transmits on
   air -- that remains genuinely untested. */
#include <stdio.h>
#include <string.h>

#include "meshcore_proto.h"

static int g_failures = 0;

static void check(int condition, const char *name)
{
    if (condition) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        g_failures++;
    }
}

static size_t append_bytes(uint8_t *buf, size_t pos, const uint8_t *data, size_t len)
{
    memcpy(buf + pos, data, len);
    return pos + len;
}

static size_t append_u8(uint8_t *buf, size_t pos, uint8_t v)
{
    buf[pos] = v;
    return pos + 1;
}

static size_t append_le32(uint8_t *buf, size_t pos, uint32_t v)
{
    buf[pos + 0] = (uint8_t)(v & 0xFFu);
    buf[pos + 1] = (uint8_t)((v >> 8) & 0xFFu);
    buf[pos + 2] = (uint8_t)((v >> 16) & 0xFFu);
    buf[pos + 3] = (uint8_t)((v >> 24) & 0xFFu);
    return pos + 4;
}

/* route_type: 0x00=TRANSPORT_FLOOD, 0x01=FLOOD, 0x02=DIRECT, 0x03=TRANSPORT_DIRECT.
   payload_type: 0x04=ADVERT (this parser's only decoded type). transport is NULL unless
   route_type carries transport codes (the caller must pass 4 bytes when it does, matching
   the header's own route-type-dependent layout -- this helper does not infer or validate
   that itself, since a malformed-on-purpose test vector needs to be able to violate it). */
static size_t build_frame(uint8_t *out, uint8_t route_type, uint8_t payload_type, uint8_t version,
                           const uint8_t *transport, uint8_t hop_count, uint8_t hash_size_code,
                           const uint8_t *path, size_t path_len,
                           const uint8_t *payload, size_t payload_len)
{
    size_t pos = 0;
    uint8_t header = (uint8_t)(((version & 0x03u) << 6) | ((payload_type & 0x0Fu) << 2) | (route_type & 0x03u));

    pos = append_u8(out, pos, header);
    if (transport != NULL) {
        pos = append_bytes(out, pos, transport, 4);
    }
    pos = append_u8(out, pos, (uint8_t)(((hash_size_code & 0x03u) << 6) | (hop_count & 0x3Fu)));
    if (path_len > 0) {
        pos = append_bytes(out, pos, path, path_len);
    }
    if (payload_len > 0) {
        pos = append_bytes(out, pos, payload, payload_len);
    }
    return pos;
}

static size_t build_advert_payload(uint8_t *out, const uint8_t pubkey[32], uint32_t timestamp,
                                     const uint8_t signature[64], const uint8_t *appdata, size_t appdata_len)
{
    size_t pos = 0;

    pos = append_bytes(out, pos, pubkey, 32);
    pos = append_le32(out, pos, timestamp);
    pos = append_bytes(out, pos, signature, 64);
    if (appdata_len > 0) {
        pos = append_bytes(out, pos, appdata, appdata_len);
    }
    return pos;
}

static void fill_pattern(uint8_t *buf, size_t len, uint8_t start)
{
    size_t i;

    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)(start + i);
    }
}

int main(void)
{
    uint8_t pubkey[32];
    uint8_t signature[64];
    uint8_t appdata[256];
    uint8_t payload[256];
    uint8_t frame[512];
    size_t appdata_len;
    size_t payload_len;
    size_t frame_len;
    meshcore_advert_t advert;

    fill_pattern(pubkey, sizeof(pubkey), 0xA0);
    fill_pattern(signature, sizeof(signature), 0x50);

    /* --- Test 1: a simple ADVERT, no location, a short name, no transport codes,
       no path hops (route_type FLOOD). --- */
    {
        appdata_len = 0;
        appdata[appdata_len++] = 0x81u; /* role=chat (0x01) | name present (0x80) */
        appdata_len = append_bytes(appdata, appdata_len, (const uint8_t *)"TestNode", 8);

        payload_len = build_advert_payload(payload, pubkey, 1700000000u, signature, appdata, appdata_len);
        frame_len = build_frame(frame, 0x01u /* FLOOD */, 0x04u /* ADVERT */, 0,
                                 NULL, 0, 0, NULL, 0, payload, payload_len);

        check(meshcore_proto_parse(frame, frame_len, &advert), "advert 1 (chat, named, no location): parses");
        check(strcmp(advert.node_id_hex, "a0a1a2a3a4a5a6a7") == 0,
              "advert 1: node_id_hex is the first 8 pubkey bytes, hex-encoded");
        check(advert.role == MESHCORE_ROLE_CHAT, "advert 1: role decodes as chat");
        check(strcmp(meshcore_role_to_string(advert.role), "chat") == 0, "advert 1: role_to_string is \"chat\"");
        check(advert.has_name && strcmp(advert.name, "TestNode") == 0, "advert 1: name decodes as \"TestNode\"");
        check(!advert.has_location, "advert 1: has_location is false (flag bit clear)");
    }

    /* --- Test 2: repeater role with a location, no name, no features. --- */
    {
        int32_t lat_e6 = 42360100;  /* 42.3601 deg */
        int32_t lon_e6 = -71058900; /* -71.0589 deg */

        appdata_len = 0;
        appdata[appdata_len++] = 0x12u; /* role=repeater (0x02) | location present (0x10) */
        appdata_len = append_le32(appdata, appdata_len, (uint32_t)lat_e6);
        appdata_len = append_le32(appdata, appdata_len, (uint32_t)lon_e6);

        payload_len = build_advert_payload(payload, pubkey, 1700000001u, signature, appdata, appdata_len);
        frame_len = build_frame(frame, 0x02u /* DIRECT */, 0x04u, 0, NULL, 0, 0, NULL, 0, payload, payload_len);

        check(meshcore_proto_parse(frame, frame_len, &advert), "advert 2 (repeater, location, no name): parses");
        check(advert.role == MESHCORE_ROLE_REPEATER, "advert 2: role decodes as repeater");
        check(!advert.has_name, "advert 2: has_name is false (flag bit clear, optional-field omission)");
        check(advert.has_location, "advert 2: has_location is true");
        check(advert.lat_e7 == 423601000, "advert 2: lat_e7 == lat_e6 * 10 (this project's lat_e7 convention)");
        check(advert.lon_e7 == -710589000, "advert 2: lon_e7 == lon_e6 * 10");
    }

    /* --- Test 3: room server, transport codes present (TRANSPORT_FLOOD), a 2-hop path
       (hash_size 1 byte/hop), appdata carrying only the mandatory flags byte. --- */
    {
        static const uint8_t transport[4] = {0x01, 0x00, 0x02, 0x00};
        static const uint8_t path[2] = {0xAA, 0xBB};

        appdata_len = 0;
        appdata[appdata_len++] = 0x03u; /* role=room_server, no optional fields */

        payload_len = build_advert_payload(payload, pubkey, 1700000002u, signature, appdata, appdata_len);
        frame_len = build_frame(frame, 0x00u /* TRANSPORT_FLOOD */, 0x04u, 0,
                                 transport, 2 /* hop_count */, 0 /* hash_size_code -> 1 byte/hop */,
                                 path, sizeof(path), payload, payload_len);

        check(meshcore_proto_parse(frame, frame_len, &advert),
              "advert 3 (room_server, transport codes + 2-hop path): parses");
        check(advert.role == MESHCORE_ROLE_ROOM_SERVER, "advert 3: role decodes as room_server");
        check(!advert.has_name && !advert.has_location, "advert 3: no optional appdata fields present");
    }

    /* --- Test 4: non-ADVERT payload type must be rejected regardless of content. --- */
    {
        appdata_len = 0;
        appdata[appdata_len++] = 0x81u;
        appdata_len = append_bytes(appdata, appdata_len, (const uint8_t *)"TestNode", 8);
        payload_len = build_advert_payload(payload, pubkey, 1700000003u, signature, appdata, appdata_len);
        frame_len = build_frame(frame, 0x01u, 0x02u /* PAYLOAD_TYPE_TXT_MSG, not ADVERT */, 0,
                                 NULL, 0, 0, NULL, 0, payload, payload_len);

        check(!meshcore_proto_parse(frame, frame_len, &advert),
              "non-ADVERT payload type (txt_msg): rejected (deliberately unparsed)");
    }

    /* --- Test 5: truncated frames must be rejected, never read past frame_len. --- */
    {
        uint8_t one_byte[1] = {0x01u}; /* FLOOD route, ADVERT would need more header context
                                           anyway; this is just the header byte alone, no
                                           path-length byte at all */

        check(!meshcore_proto_parse(one_byte, 1, &advert), "1-byte frame (no path-length byte): rejected");
        check(!meshcore_proto_parse(NULL, 0, &advert), "NULL frame: rejected");

        /* A structurally-fine header+path-length, but the payload is one byte short of the
           mandatory pubkey+timestamp+signature+flags minimum (101 bytes). */
        {
            uint8_t short_payload[100]; /* one byte short of MESHCORE_ADVERT_FIXED_LEN + 1 */

            memset(short_payload, 0, sizeof(short_payload));
            frame_len = build_frame(frame, 0x01u, 0x04u, 0, NULL, 0, 0, NULL, 0,
                                     short_payload, sizeof(short_payload));
            check(!meshcore_proto_parse(frame, frame_len, &advert),
                  "payload one byte short of pubkey+timestamp+signature+flags: rejected");
        }
    }

    /* --- Test 6: a name longer than MESHCORE_NAME_MAX_LEN is truncated, not overrun. --- */
    {
        char long_name[MESHCORE_NAME_MAX_LEN + 10u];
        size_t i;

        for (i = 0; i < sizeof(long_name); i++) {
            long_name[i] = (char)('A' + (i % 26));
        }

        appdata_len = 0;
        appdata[appdata_len++] = 0x84u; /* role=sensor | name present */
        appdata_len = append_bytes(appdata, appdata_len, (const uint8_t *)long_name, sizeof(long_name));

        payload_len = build_advert_payload(payload, pubkey, 1700000004u, signature, appdata, appdata_len);
        frame_len = build_frame(frame, 0x01u, 0x04u, 0, NULL, 0, 0, NULL, 0, payload, payload_len);

        check(meshcore_proto_parse(frame, frame_len, &advert), "advert 6 (oversized name): parses");
        check(advert.has_name && strlen(advert.name) == MESHCORE_NAME_MAX_LEN,
              "advert 6: name truncated to exactly MESHCORE_NAME_MAX_LEN bytes, NUL-terminated");
    }

    /* --- Test 7: every appdata optional field present at once (location + both reserved
       feature words + name), in the spec's fixed order. --- */
    {
        appdata_len = 0;
        appdata[appdata_len++] = 0xF4u; /* sensor | location | feature1 | feature2 | name */
        appdata_len = append_le32(appdata, appdata_len, (uint32_t)(int32_t)10000000); /* lat 1.0 deg */
        appdata_len = append_le32(appdata, appdata_len, (uint32_t)(int32_t)20000000); /* lon 2.0 deg */
        appdata_len = append_u8(appdata, appdata_len, 0xAAu); /* feature1 (reserved) */
        appdata_len = append_u8(appdata, appdata_len, 0xBBu);
        appdata_len = append_u8(appdata, appdata_len, 0xCCu); /* feature2 (reserved) */
        appdata_len = append_u8(appdata, appdata_len, 0xDDu);
        appdata_len = append_bytes(appdata, appdata_len, (const uint8_t *)"Kitchen Sink", 12);

        payload_len = build_advert_payload(payload, pubkey, 1700000005u, signature, appdata, appdata_len);
        frame_len = build_frame(frame, 0x01u, 0x04u, 0, NULL, 0, 0, NULL, 0, payload, payload_len);

        check(meshcore_proto_parse(frame, frame_len, &advert), "advert 7 (all optional fields present): parses");
        check(advert.role == MESHCORE_ROLE_SENSOR, "advert 7: role decodes as sensor");
        check(advert.has_location && advert.lat_e7 == 100000000 && advert.lon_e7 == 200000000,
              "advert 7: location decodes correctly after skipping nothing before it");
        check(advert.has_name && strcmp(advert.name, "Kitchen Sink") == 0,
              "advert 7: name decodes correctly after both reserved feature words are skipped");
    }

    /* --- Test 8: an implausible (out-of-range) location must be dropped (has_location
       stays false) without rejecting the whole ADVERT -- see meshcore_proto.c's comment on
       why (unverified signature; also protects the wire codec's sizing guarantee). --- */
    {
        appdata_len = 0;
        appdata[appdata_len++] = 0x11u; /* chat | location present */
        appdata_len = append_le32(appdata, appdata_len, (uint32_t)(int32_t)999000000); /* way past +-90 deg */
        appdata_len = append_le32(appdata, appdata_len, (uint32_t)(int32_t)20000000);

        payload_len = build_advert_payload(payload, pubkey, 1700000006u, signature, appdata, appdata_len);
        frame_len = build_frame(frame, 0x01u, 0x04u, 0, NULL, 0, 0, NULL, 0, payload, payload_len);

        check(meshcore_proto_parse(frame, frame_len, &advert),
              "advert 8 (implausible lat, still a structurally valid ADVERT): parses");
        check(!advert.has_location, "advert 8: has_location is false (implausible value dropped, not propagated)");
    }

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

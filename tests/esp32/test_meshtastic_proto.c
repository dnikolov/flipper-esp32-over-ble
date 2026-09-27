/* Host-native test driver for heltec/main/meshtastic_proto.c -- the pure-C(+mbedtls-AES)
   Meshtastic packet parser underneath lora_shared_radio.cpp's real SX1276/RadioLib driver
   (docs/PLAN.md's "Meshtastic Scan Capability -- Heltec Board (Phase 1)" design plan).

   No real Meshtastic hardware/node was available this session to capture genuine over-the-air
   packets from (mirrors meshcore_proto's own caveat, tests/esp32/test_meshcore_proto.c) --
   every frame below is hand-constructed from this project's own research into meshtastic/
   firmware's/meshtastic/protobufs' public source (cited in meshtastic_proto.c's header
   comments), including this test performing its own from-scratch AES-128-CTR encryption of
   synthetic protobuf bytes to build a "decryptable" test vector -- not captured from a real
   device. Treat this as "the parser does what our research says the protocol should look
   like", not as proof it correctly decodes whatever a real Meshtastic firmware actually
   transmits on air -- that remains genuinely untested (see meshtastic_proto.h's top comment
   for the fuller risk framing, especially for the name-decode path). */
#include <stdio.h>
#include <string.h>

#include <mbedtls/aes.h>

#include "meshtastic_proto.h"

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

/* Duplicated from meshtastic_proto.c on purpose (test independence, same pattern this
   project's other host tests use for known RFC/spec constants) -- meshtastic/firmware's
   src/mesh/Channels.h defaultpsk[16], AES-128 key for the public default channel at PSK
   index 1 ("unmodified"). */
static const uint8_t TEST_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

static void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static size_t build_header(uint8_t *out, uint32_t to, uint32_t from, uint32_t packet_id, uint8_t channel)
{
    write_u32_le(out + 0, to);
    write_u32_le(out + 4, from);
    write_u32_le(out + 8, packet_id);
    out[12] = 0; /* flags, unused by this parser */
    out[13] = channel;
    out[14] = 0; /* next_hop, unused */
    out[15] = 0; /* relay_node, unused */
    return 16;
}

/* meshtastic/firmware's CryptoEngine::initNonce(): packetId (8 bytes, 32-bit value
   zero-extended, little-endian) || fromNode (4 bytes, little-endian) || 4 zero bytes. Mirrors
   meshtastic_proto.c's own construction exactly -- this test intentionally duplicates it
   rather than reaching into that file's internals, so a bug shared by both would not be
   caught by this test (an accepted limitation, same as any test that re-derives its own
   expected value from the same spec the code under test was written against). */
static void aes128_ctr_encrypt(uint32_t from_node, uint32_t packet_id,
                                const uint8_t *in, size_t len, uint8_t *out)
{
    mbedtls_aes_context aes;
    uint8_t nonce[16];
    uint8_t stream_block[16];
    size_t nc_off = 0;

    memset(nonce, 0, sizeof(nonce));
    write_u32_le(nonce + 0, packet_id);
    write_u32_le(nonce + 8, from_node);

    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, TEST_DEFAULT_PSK, 128);
    mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce, stream_block, in, out);
    mbedtls_aes_free(&aes);
}

static size_t append_varint(uint8_t *buf, size_t pos, uint64_t v)
{
    while (v >= 0x80u) {
        buf[pos++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    buf[pos++] = (uint8_t)v;
    return pos;
}

static size_t append_tag(uint8_t *buf, size_t pos, uint32_t field, int wire_type)
{
    return append_varint(buf, pos, ((uint64_t)field << 3) | (uint32_t)wire_type);
}

static size_t append_length_delimited(uint8_t *buf, size_t pos, const uint8_t *data, size_t len)
{
    pos = append_varint(buf, pos, len);
    memcpy(buf + pos, data, len);
    return pos + len;
}

/* Builds a plaintext Data{portnum, payload=User{short_name}} protobuf message (only the
   fields this parser actually reads -- mirrors meshtastic_proto.c's own "parse only what's
   needed" scope). */
static size_t build_nodeinfo_data(uint8_t *out, uint32_t portnum, const char *short_name)
{
    uint8_t user_buf[64];
    size_t user_len = 0;
    size_t pos = 0;

    if (short_name != NULL) {
        user_len = append_tag(user_buf, user_len, 3 /* short_name */, 2);
        user_len = append_length_delimited(user_buf, user_len, (const uint8_t *)short_name, strlen(short_name));
    }

    pos = append_tag(out, pos, 1 /* portnum */, 0);
    pos = append_varint(out, pos, portnum);
    pos = append_tag(out, pos, 2 /* payload */, 2);
    pos = append_length_delimited(out, pos, user_buf, user_len);
    return pos;
}

int main(void)
{
    uint8_t frame[300];
    uint8_t plaintext[128];
    size_t plaintext_len;
    size_t frame_len;
    meshtastic_advert_t advert;
    uint8_t default_channel;

    default_channel = meshtastic_default_channel_hash();
    /* Cross-check against the publicly documented value (see meshtastic_proto.h's own
       comment) -- this is a regression check on this file's own research/arithmetic, not
       load-bearing for the parser itself (which only ever calls the function, never a
       hardcoded literal). */
    check(default_channel == 0x08u,
          "meshtastic_default_channel_hash(): matches the publicly documented LongFast/default-PSK value (0x08)");

    /* --- Test 1: a frame on a non-default channel -- presence (node_id) is still reported,
       no decrypt is even attempted, so no name. --- */
    {
        frame_len = build_header(frame, 0xFFFFFFFFu, 0x12345678u, 0xAABBCCDDu,
                                  (uint8_t)(default_channel ^ 0xFFu));
        /* Arbitrary ciphertext-shaped bytes -- content doesn't matter since this channel byte
           never triggers a decrypt attempt. */
        memset(frame + frame_len, 0x55, 20);
        frame_len += 20;

        check(meshtastic_proto_parse(frame, frame_len, &advert),
              "non-default channel: parses (structural presence only)");
        check(strcmp(advert.node_id_hex, "12345678") == 0,
              "non-default channel: node_id_hex derived from the header's `from` field");
        check(!advert.has_name, "non-default channel: has_name is false (decrypt never attempted)");
    }

    /* --- Test 2: a frame on the default channel whose payload decrypts to a NODEINFO_APP
       Data/User pair carrying a short_name -- has_name should be true. --- */
    {
        uint32_t from_node = 0x433d2b1cu;
        uint32_t packet_id = 0x00000001u;

        plaintext_len = build_nodeinfo_data(plaintext, 4 /* NODEINFO_APP */, "Bob");
        frame_len = build_header(frame, 0xFFFFFFFFu, from_node, packet_id, default_channel);
        aes128_ctr_encrypt(from_node, packet_id, plaintext, plaintext_len, frame + frame_len);
        frame_len += plaintext_len;

        check(meshtastic_proto_parse(frame, frame_len, &advert),
              "default channel, NODEINFO_APP: parses");
        check(strcmp(advert.node_id_hex, "433d2b1c") == 0,
              "default channel, NODEINFO_APP: node_id_hex derived from the header's `from` field");
        check(advert.has_name && strcmp(advert.name, "Bob") == 0,
              "default channel, NODEINFO_APP: short_name decrypts+decodes as \"Bob\"");
    }

    /* --- Test 3: a frame on the default channel whose payload decrypts to a non-NODEINFO_APP
       portnum -- presence reported, no name. --- */
    {
        uint32_t from_node = 0xaaaaaaaau;
        uint32_t packet_id = 0x00000002u;

        plaintext_len = build_nodeinfo_data(plaintext, 1 /* TEXT_MESSAGE_APP, not NODEINFO_APP */, "Bob");
        frame_len = build_header(frame, 0xFFFFFFFFu, from_node, packet_id, default_channel);
        aes128_ctr_encrypt(from_node, packet_id, plaintext, plaintext_len, frame + frame_len);
        frame_len += plaintext_len;

        check(meshtastic_proto_parse(frame, frame_len, &advert),
              "default channel, non-NODEINFO_APP portnum: parses");
        check(!advert.has_name, "default channel, non-NODEINFO_APP portnum: has_name is false");
    }

    /* --- Test 4: a frame on the default channel with no User.short_name field at all
       (NODEINFO_APP but an empty/id-only User) -- presence reported, no name. --- */
    {
        uint32_t from_node = 0xbbbbbbbbu;
        uint32_t packet_id = 0x00000003u;

        plaintext_len = build_nodeinfo_data(plaintext, 4 /* NODEINFO_APP */, NULL);
        frame_len = build_header(frame, 0xFFFFFFFFu, from_node, packet_id, default_channel);
        aes128_ctr_encrypt(from_node, packet_id, plaintext, plaintext_len, frame + frame_len);
        frame_len += plaintext_len;

        check(meshtastic_proto_parse(frame, frame_len, &advert),
              "default channel, NODEINFO_APP with no short_name: parses");
        check(!advert.has_name, "default channel, NODEINFO_APP with no short_name: has_name is false");
    }

    /* --- Test 5: a frame on the default channel whose ciphertext decrypts to garbage (wrong
       key/channel-hash collision or corrupted data) -- must never surface a garbage name. --- */
    {
        uint32_t from_node = 0xccccccccu;

        frame_len = build_header(frame, 0xFFFFFFFFu, from_node, 0x12345678u, default_channel);
        memset(frame + frame_len, 0xEE, 40); /* not valid AES-CTR ciphertext of any protobuf */
        frame_len += 40;

        check(meshtastic_proto_parse(frame, frame_len, &advert),
              "default channel, garbage ciphertext: still parses (structural presence)");
        check(!advert.has_name, "default channel, garbage ciphertext: has_name is false, never garbage");
    }

    /* --- Test 6: a name longer than MESHTASTIC_NAME_MAX_LEN is truncated, not overrun, and
       the truncated (still fully printable-ASCII) result is accepted. --- */
    {
        uint32_t from_node = 0xddddddddu;
        uint32_t packet_id = 0x00000004u;
        char long_name[MESHTASTIC_NAME_MAX_LEN + 10u];
        size_t i;

        for (i = 0; i < sizeof(long_name) - 1u; i++) {
            long_name[i] = (char)('A' + (i % 26));
        }
        long_name[sizeof(long_name) - 1u] = '\0';

        plaintext_len = build_nodeinfo_data(plaintext, 4, long_name);
        frame_len = build_header(frame, 0xFFFFFFFFu, from_node, packet_id, default_channel);
        aes128_ctr_encrypt(from_node, packet_id, plaintext, plaintext_len, frame + frame_len);
        frame_len += plaintext_len;

        check(meshtastic_proto_parse(frame, frame_len, &advert),
              "default channel, oversized short_name: parses");
        check(advert.has_name && strlen(advert.name) == MESHTASTIC_NAME_MAX_LEN,
              "default channel, oversized short_name: truncated to exactly MESHTASTIC_NAME_MAX_LEN bytes");
    }

    /* --- Test 7: truncated frames (shorter than the mandatory 16-byte header) must be
       rejected, never read past frame_len. --- */
    {
        uint8_t short_frame[15];

        memset(short_frame, 0, sizeof(short_frame));
        check(!meshtastic_proto_parse(short_frame, sizeof(short_frame), &advert),
              "15-byte frame (shorter than the mandatory header): rejected");
        check(!meshtastic_proto_parse(NULL, 0, &advert), "NULL frame: rejected");

        frame_len = build_header(frame, 0, 0, 0, 0); /* exactly 16 bytes, no payload at all */
        check(meshtastic_proto_parse(frame, frame_len, &advert),
              "exactly-16-byte frame (header only, no payload): parses (presence only)");
        check(!advert.has_name, "exactly-16-byte frame: has_name is false (nothing to decrypt)");
    }

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}

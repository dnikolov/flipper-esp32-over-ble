/* See meshtastic_proto.h for scope/citations/risk framing. This file's header-decode path
   (read_header, node_id formatting) is derived from meshtastic/firmware's RadioInterface.h
   `PacketHeader` struct (to/from/id: 4-byte NodeNum/PacketId each; flags/channel/next_hop/
   relay_node: 1 byte each -- 16 bytes total, "exactly match[ing] the wire layout when sent
   over the radio link" per that header's own comment) and meshtastic.org/docs/overview/
   encryption/'s statement that this header is always sent unencrypted specifically so relays
   without a channel's key can still forward what they can't read.

   The name-decode path (decrypt_default_channel_payload/protobuf field walk) additionally
   relies on:
   - meshtastic/firmware's src/mesh/Channels.h `defaultpsk[16]` (AES-128 key for the
     public default channel, PSK index 1 = "unmodified") and its generateHash() channel-hash
     algorithm (xorHash(name) ^ xorHash(psk)).
   - meshtastic/firmware's src/mesh/CryptoEngine.cpp `initNonce()`: a 16-byte AES-CTR
     nonce/counter laid out as packetId (8 bytes, the 32-bit on-air id zero-extended, native/
     little-endian) followed by fromNode (4 bytes, little-endian) followed by 4 zero bytes
     (no "extraNonce" for ordinary channel traffic).
   - meshtastic/protobufs' meshtastic/mesh.proto: `Data { PortNum portnum = 1; bytes payload
     = 2; ... }` and `User { string id = 1; string long_name = 2; string short_name = 3;
     ... }`, and meshtastic/portnums.proto's `NODEINFO_APP = 4`. This file decodes
     `short_name`, not `long_name` -- see MESHTASTIC_USER_FIELD_SHORT_NAME's own comment for
     why.
   None of this is secret -- Meshtastic explicitly designs the default channel to be publicly
   listenable/decryptable, matching this capability's scope (see meshtastic_proto.h). */
#include "meshtastic_proto.h"

#include <string.h>

#include <mbedtls/aes.h>

#define MESHTASTIC_HEADER_LEN 16u
#define MESHTASTIC_PORTNUM_NODEINFO_APP 4u
#define MESHTASTIC_DATA_FIELD_PORTNUM 1u
#define MESHTASTIC_DATA_FIELD_PAYLOAD 2u
/* short_name (User message field 3), not long_name (field 2) -- Meshtastic's own convention
   is that short_name is a compact per-node tag (an emoji or short callsign, UI-enforced at a
   few characters in the official apps, though the wire field itself carries no hard length
   limit), a much better fit for MESHTASTIC_NAME_MAX_LEN's small budget on this board (see
   meshtastic_proto.h) than truncating the longer, free-form long_name field would be. */
#define MESHTASTIC_USER_FIELD_SHORT_NAME 3u

/* Meshtastic's own documented maximum LoRa frame size (16-byte header + up to ~237-240 bytes
   of encrypted payload, depending on firmware version/region); 256 total mirrors
   MESHCORE_RADIO_MAX_FRAME_LEN's own margin-above-spec convention (meshcore_radio.cpp). */
#define MESHTASTIC_MAX_CIPHERTEXT_LEN (256u - MESHTASTIC_HEADER_LEN)

static const char MESHTASTIC_DEFAULT_CHANNEL_NAME[] = "LongFast";
/* meshtastic/firmware's src/mesh/Channels.h: "16 bytes of random PSK for our _public_ default
   channel that all devices power up on (AES128)". PSK index 1 (a channel whose configured PSK
   is the single byte 0x01) uses this array completely unmodified. */
static const uint8_t MESHTASTIC_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void node_id_to_hex(uint32_t node_num, char *out_hex)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = (int)MESHTASTIC_NODE_ID_HEX_LEN - 1; i >= 0; i--) {
        out_hex[i] = digits[node_num & 0x0Fu];
        node_num >>= 4;
    }
    out_hex[MESHTASTIC_NODE_ID_HEX_LEN] = '\0';
}

static uint8_t xor_hash(const uint8_t *data, size_t len)
{
    uint8_t result = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        result ^= data[i];
    }
    return result;
}

uint8_t meshtastic_default_channel_hash(void)
{
    uint8_t name_hash = xor_hash((const uint8_t *)MESHTASTIC_DEFAULT_CHANNEL_NAME,
                                  strlen(MESHTASTIC_DEFAULT_CHANNEL_NAME));
    uint8_t psk_hash = xor_hash(MESHTASTIC_DEFAULT_PSK, sizeof(MESHTASTIC_DEFAULT_PSK));

    return (uint8_t)(name_hash ^ psk_hash);
}

/* Local zeroize, deliberately not components/feb_protocol's feb_secure_zero() -- this file
   has no dependency on that component (see meshtastic_proto.h's top comment, mirroring
   meshcore_proto.c's own zero-coupling stance). volatile pointer defeats a dead-store-
   elimination optimizer the same way feb_secure_zero() does. */
static void meshtastic_secure_zero(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    size_t i;

    for (i = 0; i < len; i++) {
        p[i] = 0;
    }
}

/* Minimal protobuf wire-format walk: returns the FIRST top-level occurrence of field
   want_field in buf[0..len), regardless of wire type, via *out_wire_type/out_varint/out_data/
   out_len (whichever apply to the wire type actually found). Returns false if want_field
   never appears, or the buffer is malformed/truncated -- this only ever needs to walk small
   (<= MESHTASTIC_MAX_CIPHERTEXT_LEN), fully-buffered, bounds-checked-up-front messages, so
   returning false-and-bailing on any irregularity (including wire types 3/4, the deprecated
   group start/end markers no current Meshtastic message uses) is an acceptable, deliberately
   narrow "parse only what's needed" posture -- see meshtastic_proto.h/meshcore_proto.h's
   shared philosophy note. */
static bool protobuf_read_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t result = 0;
    unsigned shift = 0;
    size_t p = *pos;

    while (p < len) {
        uint8_t b = buf[p++];

        result |= (uint64_t)(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            *pos = p;
            *out = result;
            return true;
        }
        shift += 7u;
        if (shift >= 64u) {
            return false; /* malformed: varint too long */
        }
    }
    return false; /* truncated */
}

static bool protobuf_find_field(const uint8_t *buf, size_t len, uint32_t want_field,
                                 const uint8_t **out_data, size_t *out_len, uint64_t *out_varint)
{
    size_t pos = 0;

    while (pos < len) {
        uint64_t tag;
        uint32_t field_number;
        uint32_t wire_type;

        if (!protobuf_read_varint(buf, len, &pos, &tag)) {
            return false;
        }
        field_number = (uint32_t)(tag >> 3);
        wire_type = (uint32_t)(tag & 0x7u);

        switch (wire_type) {
        case 0: { /* varint */
            uint64_t value;

            if (!protobuf_read_varint(buf, len, &pos, &value)) {
                return false;
            }
            if (field_number == want_field) {
                if (out_varint != NULL) {
                    *out_varint = value;
                }
                return true;
            }
            break;
        }
        case 1: /* 64-bit fixed */
            if (pos + 8u > len) {
                return false;
            }
            if (field_number == want_field) {
                if (out_data != NULL) *out_data = buf + pos;
                if (out_len != NULL) *out_len = 8u;
                return true;
            }
            pos += 8u;
            break;
        case 2: { /* length-delimited */
            uint64_t sublen;

            if (!protobuf_read_varint(buf, len, &pos, &sublen)) {
                return false;
            }
            if (sublen > len - pos) {
                return false;
            }
            if (field_number == want_field) {
                if (out_data != NULL) *out_data = buf + pos;
                if (out_len != NULL) *out_len = (size_t)sublen;
                return true;
            }
            pos += (size_t)sublen;
            break;
        }
        case 5: /* 32-bit fixed */
            if (pos + 4u > len) {
                return false;
            }
            if (field_number == want_field) {
                if (out_data != NULL) *out_data = buf + pos;
                if (out_len != NULL) *out_len = 4u;
                return true;
            }
            pos += 4u;
            break;
        default:
            return false; /* wire type 3/4 (deprecated groups) -- not used by any message this
                              parser reads; bail out defensively rather than guess a length */
        }
    }
    return false; /* want_field not present */
}

static bool all_printable_ascii(const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (data[i] < 0x20u || data[i] > 0x7Eu) {
            return false;
        }
    }
    return true;
}

/* Attempts to decrypt+parse a default-("LongFast")-channel payload down to a NODEINFO_APP
   long_name. Returns false (leaving *out_name untouched) for anything short of a fully
   structurally-valid, printable-ASCII result -- see meshtastic_proto.h's top comment on why
   this path is held to a stricter bar than meshcore_proto.c's plaintext ADVERT decode. */
static bool decrypt_default_channel_name(const uint8_t *ciphertext, size_t ciphertext_len,
                                          uint32_t from_node, uint32_t packet_id,
                                          char *out_name, size_t out_name_cap)
{
    uint8_t nonce[16];
    uint8_t stream_block[16];
    uint8_t plaintext[MESHTASTIC_MAX_CIPHERTEXT_LEN];
    size_t nc_off = 0;
    mbedtls_aes_context aes;
    int ret;
    uint64_t portnum = 0;
    const uint8_t *inner_payload = NULL;
    size_t inner_payload_len = 0;
    const uint8_t *name_data = NULL;
    size_t name_len = 0;
    bool ok;

    if (ciphertext_len == 0u || ciphertext_len > sizeof(plaintext)) {
        return false;
    }

    memset(nonce, 0, sizeof(nonce));
    nonce[0] = (uint8_t)(packet_id & 0xFFu);
    nonce[1] = (uint8_t)((packet_id >> 8) & 0xFFu);
    nonce[2] = (uint8_t)((packet_id >> 16) & 0xFFu);
    nonce[3] = (uint8_t)((packet_id >> 24) & 0xFFu);
    /* bytes 4-7 stay zero: the wire's packetId is only 32 bits; CryptoEngine::initNonce()
       zero-extends it into a 64-bit value before copying 8 bytes */
    nonce[8] = (uint8_t)(from_node & 0xFFu);
    nonce[9] = (uint8_t)((from_node >> 8) & 0xFFu);
    nonce[10] = (uint8_t)((from_node >> 16) & 0xFFu);
    nonce[11] = (uint8_t)((from_node >> 24) & 0xFFu);
    /* bytes 12-15 stay zero: no extraNonce for ordinary (non-PKI) channel traffic */

    mbedtls_aes_init(&aes);
    ret = mbedtls_aes_setkey_enc(&aes, MESHTASTIC_DEFAULT_PSK, 128);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        meshtastic_secure_zero(nonce, sizeof(nonce));
        return false;
    }

    ret = mbedtls_aes_crypt_ctr(&aes, ciphertext_len, &nc_off, nonce, stream_block,
                                 ciphertext, plaintext);
    mbedtls_aes_free(&aes);
    meshtastic_secure_zero(nonce, sizeof(nonce));
    meshtastic_secure_zero(stream_block, sizeof(stream_block));
    if (ret != 0) {
        meshtastic_secure_zero(plaintext, sizeof(plaintext));
        return false;
    }

    ok = protobuf_find_field(plaintext, ciphertext_len, MESHTASTIC_DATA_FIELD_PORTNUM,
                              NULL, NULL, &portnum);
    if (!ok || portnum != MESHTASTIC_PORTNUM_NODEINFO_APP) {
        meshtastic_secure_zero(plaintext, sizeof(plaintext));
        return false;
    }

    ok = protobuf_find_field(plaintext, ciphertext_len, MESHTASTIC_DATA_FIELD_PAYLOAD,
                              &inner_payload, &inner_payload_len, NULL);
    if (!ok) {
        meshtastic_secure_zero(plaintext, sizeof(plaintext));
        return false;
    }

    ok = protobuf_find_field(inner_payload, inner_payload_len, MESHTASTIC_USER_FIELD_SHORT_NAME,
                              &name_data, &name_len, NULL);
    if (!ok || name_len == 0u) {
        meshtastic_secure_zero(plaintext, sizeof(plaintext));
        return false;
    }
    if (name_len > out_name_cap - 1u) {
        name_len = out_name_cap - 1u; /* truncate, same convention as meshcore_proto.c */
    }
    if (!all_printable_ascii(name_data, name_len)) {
        /* A garbage/implausible name means either the wrong key was applied (channel-hash
           collision with a private channel we can't actually decrypt) or a bug in the
           reverse-engineered layout above -- report no name at all rather than a name that
           could be actively misleading. */
        meshtastic_secure_zero(plaintext, sizeof(plaintext));
        return false;
    }

    memcpy(out_name, name_data, name_len);
    out_name[name_len] = '\0';
    meshtastic_secure_zero(plaintext, sizeof(plaintext));
    return true;
}

bool meshtastic_proto_parse(const uint8_t *frame, size_t frame_len, meshtastic_advert_t *out)
{
    uint32_t from_node;
    uint32_t packet_id;
    uint8_t channel;
    const uint8_t *ciphertext;
    size_t ciphertext_len;

    if (frame == NULL || out == NULL || frame_len < MESHTASTIC_HEADER_LEN) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    from_node = read_u32_le(frame + 4);
    packet_id = read_u32_le(frame + 8);
    channel = frame[13];

    node_id_to_hex(from_node, out->node_id_hex);

    ciphertext = frame + MESHTASTIC_HEADER_LEN;
    ciphertext_len = frame_len - MESHTASTIC_HEADER_LEN;

    if (ciphertext_len > 0u && channel == meshtastic_default_channel_hash()) {
        out->has_name = decrypt_default_channel_name(ciphertext, ciphertext_len, from_node,
                                                      packet_id, out->name, sizeof(out->name));
    }

    return true;
}

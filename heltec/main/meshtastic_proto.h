/* Meshtastic (a third-party open LoRa mesh-network firmware/protocol, unrelated to this
   project's own BLE protocol and unrelated to MeshCore -- see meshcore_proto.h) packet
   parser -- board-local, no ESP-IDF/FreeRTOS dependency (pure C aside from mbedTLS's AES
   module, which is itself portable/host-buildable -- see this file's .c for why that one
   dependency was accepted here, unlike meshcore_proto.c's zero-dependency stance).
   Host-testable: tests/esp32/test_meshtastic_proto.c.

   Scope (docs/PLAN.md's "Meshtastic Scan Capability" design, Phase 1, mirroring
   meshcore_scan's own Phase 1 "detection + display" tier): passive presence detection only,
   default ("LongFast") primary channel only. Every field here was derived from Meshtastic's
   own public documentation and firmware/protobuf source (meshtastic.org's docs, the
   meshtastic/firmware and meshtastic/protobufs GitHub repos) during this session's research
   -- there is no MeshCore-style vendor "packet_format.md" for Meshtastic's raw-header layout,
   so the citations live in this file's function-level comments instead of one central spec
   doc. **None of this has been checked against a real over-the-air capture** (no Meshtastic
   hardware available) -- only against synthetic frames this project constructs itself
   (tests/esp32/test_meshtastic_proto.c), exactly like meshcore_proto.c's own caveat, but with
   materially higher risk here: Meshtastic's header-only presence detection (below) is
   low-risk (a fixed, publicly-documented raw byte layout), but the *optional* name field
   requires reversing an AES-128-CTR decryption + a hand-rolled protobuf field walk, which is
   a larger surface for a subtle mistake (wrong nonce byte order, wrong field number, wrong
   channel-hash math) to silently produce a wrong-but-plausible-looking name rather than a
   clean "field absent" -- see meshtastic_proto.c's decrypt path for the defensive checks
   (printable-ASCII validation) added specifically because of this. Treat the presence/
   node-id path as solid, the name-decode path as "our best reconstruction, unverified" until
   a real capture confirms it. */
#ifndef FEB_MESHTASTIC_PROTO_H
#define FEB_MESHTASTIC_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hex-character count (not bytes) of the node_id text: Meshtastic's own 32-bit NodeNum,
   rendered as 8 lowercase hex characters (matching the app's own on-screen convention of
   "!<8 hex chars>", minus the "!" -- this codebase's cbor_meshcore.h precedent for node_id
   already drops any such prefix character for a plain-hex wire field). */
#define MESHTASTIC_NODE_ID_HEX_LEN 8u

/* Parser-local cap on the decoded NodeInfo short_name (see meshtastic_proto.c's
   MESHTASTIC_USER_FIELD_SHORT_NAME comment for why this decodes short_name, not long_name).
   Independent of components/feb_protocol/cbor_meshtastic.h's own FEB_MESHTASTIC_NAME_MAX_LEN
   for the same reason meshcore_proto.h's own MESHCORE_NAME_MAX_LEN is independent of
   cbor_meshcore.h's -- this file has no dependency on that component -- but set to the same
   value (8) on purpose, for the same reasons (see meshcore_proto.h's comment on this).
   Deliberately much smaller than meshcore_proto.h's own 24: this board's classic-ESP32 DRAM
   budget could not fit meshtastic_table.h's node table at longer name lengths once both
   mesh-scan capabilities' tables coexisted -- see that header's own sizing note for the
   measured `idf.py build` numbers that drove this down. Meshtastic's short_name is already
   conventionally a compact (often <=4 character) tag, so 8 is generous headroom for it, unlike
   truncating the free-form long_name field would be. main.c re-clamps to
   FEB_MESHTASTIC_NAME_MAX_LEN defensively when building a feb_meshtastic_node_t. */
#define MESHTASTIC_NAME_MAX_LEN 8u

typedef struct {
    char node_id_hex[MESHTASTIC_NODE_ID_HEX_LEN + 1u]; /* NUL-terminated lowercase hex,
                                                           always present -- derived from the
                                                           packet's unencrypted header, never
                                                           requires decryption */
    bool has_name;
    char name[MESHTASTIC_NAME_MAX_LEN + 1u]; /* NUL-terminated; meaningful only if has_name.
                                                 Only ever populated for a packet on the
                                                 default "LongFast" channel whose payload
                                                 decrypts to a structurally valid NODEINFO_APP
                                                 Data/User protobuf pair with a printable-ASCII
                                                 long_name -- see meshtastic_proto.c. */
} meshtastic_advert_t;

/* Parses one raw LoRa PHY payload (as returned by RadioLib's readData()) into *out. The
   16-byte raw packet header (to/from/id/flags/channel/next_hop/relay_node -- always sent in
   cleartext by design, so relaying nodes that lack a channel's key can still forward packets;
   see meshtastic_proto.c's top comment for the citation) is structurally mandatory: returns
   false (leaving *out zeroed) for any frame shorter than that header, or a NULL frame/out.
   Any frame_len >= 16 is accepted as "a Meshtastic-shaped packet was heard" (node_id_hex is
   always populated from the header's `from` field in that case) -- this is intentionally
   lenient compared to meshcore_proto_parse()'s payload-type gate, because Meshtastic's header
   carries no payload-type/magic byte of its own to filter on; the SX1276's own sync-word
   register (see lora_shared_radio.cpp) is what already restricts reception to
   Meshtastic-shaped frames before this parser ever sees them.
   name/has_name are populated only best-effort (see this header's top comment and
   meshtastic_proto.c) -- their absence is never itself a parse failure. Never reads past
   frame[0..frame_len). */
bool meshtastic_proto_parse(const uint8_t *frame, size_t frame_len, meshtastic_advert_t *out);

/* Exposed for tests (and used internally by meshtastic_proto_parse()): the packet header's
   `channel` byte value that identifies Meshtastic's default "LongFast" channel at its default
   (single-byte, value 1 = "unmodified default PSK") pre-shared key. Computed at runtime from
   the channel name and default PSK constants this file already needs for decryption (rather
   than hand-derived and hardcoded), via the same "xorHash(name) ^ xorHash(psk)" algorithm
   Meshtastic's own firmware uses (meshtastic/firmware's Channels.cpp generateHash(), per this
   session's research) -- see meshtastic_proto.c. Publicly known to currently equal 0x08; this
   function's whole point is to derive that value from source constants rather than trust a
   hand-computed magic number, so treat 0x08 as a cross-check, not the source of truth. */
uint8_t meshtastic_default_channel_hash(void);

#endif /* FEB_MESHTASTIC_PROTO_H */

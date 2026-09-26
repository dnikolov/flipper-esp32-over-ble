/* Wired inter-board cluster link (Heltec coordinator <-> C6/C5 scan workers), frozen wire
   format in docs/CLUSTER.md's "Inter-board protocol" section (2026-09-26) -- implement
   exactly what's there, do not re-derive. This is NOT components/feb_protocol: that
   component is the BLE-facing, cryptographically-authenticated contract with the Flipper
   (docs/PROTOCOL.md); this one is a physically-wired, unencrypted, co-located link between
   boards the same owner already physically possesses (see docs/CLUSTER.md's "physical
   possession" threat-model note and docs/DECISIONS.md). Do not conflate the two, and do
   not add crypto here -- that would be scope not asked for by the frozen design.

   No ESP-IDF/FreeRTOS dependency in this file or cluster_link.c/crc16.c -- host-testable
   the same way components/feb_protocol's cbor_*.c files are (see
   tests/esp32/test_cluster_link.c).

   ---- Frame layout (docs/CLUSTER.md) ----
   byte 0-1: SOF        0xFE 0xED (resync marker, NOT covered by the CRC)
   byte 2:   msg_type   uint8
   byte 3-4: payload_len uint16, little-endian, max FEB_CLUSTER_MAX_PAYLOAD (512)
   byte 5..: payload    payload_len bytes, shape depends on msg_type
   last 2:   crc16      CCITT-FALSE (poly 0x1021, init 0xFFFF), over
                        [msg_type, payload_len, payload] -- not the SOF bytes

   docs/CLUSTER.md does not pin the CRC field's own transmission byte order. This
   implementation sends/expects it little-endian (crc_lo, crc_hi), for consistency with
   payload_len's explicitly-stated little-endian convention on the same frame. Flag this
   for an explicit line in docs/CLUSTER.md before the Heltec/C6 sides are wired to two
   independently-written decoders that could each guess differently.

   ---- Streaming decoder resync behavior (docs/CLUSTER.md) ----
   Byte-at-a-time state machine: scan for 0xFE 0xED, then msg_type, then payload_len
   (reject/resync immediately if it exceeds FEB_CLUSTER_MAX_PAYLOAD -- before consuming any
   payload bytes, since a corrupted length field is exactly the case this must not trust),
   then that many payload bytes, then the 2-byte CRC. On any validation failure (bad CRC or
   oversized length), the decoder drops back to FEB_CLUSTER_DECODE_STATE_SEEK_SOF0 and the
   *next* fed byte begins a fresh one-byte-at-a-time SOF scan -- it never skips a
   declared-but-untrusted frame length's worth of bytes. The byte that triggered the
   failure itself is consumed as part of the rejected frame, not re-examined as a
   candidate SOF byte; only bytes fed after it are considered for the next resync scan. */
#ifndef FEB_CLUSTER_LINK_H
#define FEB_CLUSTER_LINK_H

#include <stddef.h>
#include <stdint.h>

#define FEB_CLUSTER_SOF0 0xFEu
#define FEB_CLUSTER_SOF1 0xEDu

#define FEB_CLUSTER_MAX_PAYLOAD 512u
/* SOF(2) + msg_type(1) + payload_len(2) + payload(<=FEB_CLUSTER_MAX_PAYLOAD) + crc16(2) */
#define FEB_CLUSTER_MAX_FRAME_SIZE (2u + 1u + 2u + FEB_CLUSTER_MAX_PAYLOAD + 2u)

typedef enum {
    FEB_CLUSTER_MSG_WORKER_HELLO = 0x01,
    FEB_CLUSTER_MSG_SCAN_CONFIG_SET = 0x02,
    FEB_CLUSTER_MSG_SCAN_RESULT = 0x03,
    FEB_CLUSTER_MSG_SCAN_BATCH_DONE = 0x04,
} feb_cluster_msg_type_t;

/* ---- 0x01 WORKER_HELLO ---- worker -> coordinator, sent unconditionally every 1000ms. */
typedef enum {
    FEB_CLUSTER_BAND_24GHZ = 1,
    FEB_CLUSTER_BAND_5GHZ = 2,
} feb_cluster_band_t;

typedef struct {
    uint8_t band; /* feb_cluster_band_t */
} feb_cluster_worker_hello_t;

/* ---- 0x02 SCAN_CONFIG_SET ---- coordinator -> worker, sent on every mode change. */
typedef enum {
    FEB_CLUSTER_SCAN_MODE_IDLE = 0,
    FEB_CLUSTER_SCAN_MODE_CONTINUOUS = 1,
    FEB_CLUSTER_SCAN_MODE_MANUAL = 2,
} feb_cluster_scan_mode_t;

/* docs/CLUSTER.md: "mirrors today's single-board Normal/Aggressive/Speed-based swelling
   enum". That existing field (`wifi_swelling` in docs/PROTOCOL.md's `wardriving` command,
   components/feb_protocol/cbor_wardriving.h) is wire-encoded as caller-owned text
   ("normal"/"aggressive"/"speed_based") on the BLE-facing protocol, not a numeric enum --
   there is nothing to literally reuse. These numeric values are newly assigned here, in
   that same string-list order, so a future text<->number mapping (if this ever needs to
   cross into the BLE-facing protocol) stays unambiguous. */
typedef enum {
    FEB_CLUSTER_DWELL_NORMAL = 0,
    FEB_CLUSTER_DWELL_AGGRESSIVE = 1,
    FEB_CLUSTER_DWELL_SPEED_BASED = 2,
} feb_cluster_dwell_mode_t;

/* docs/CLUSTER.md: "mirrors today's wifi_band values, minus the 2.4ghz option". Same
   caveat as feb_cluster_dwell_mode_t: `wifi_band` is text ("2.4ghz"/"5ghz_fast"/
   "5ghz_full") on the BLE-facing protocol; these numeric values are newly assigned here
   in that string-list's relative order (minus 2.4ghz, per the doc). Only meaningful to
   the 5GHz (C5) worker. */
typedef enum {
    FEB_CLUSTER_BAND_FILTER_NA = 0,
    FEB_CLUSTER_BAND_FILTER_FAST_NON_DFS = 1,
    FEB_CLUSTER_BAND_FILTER_FULL = 2,
} feb_cluster_band_filter_t;

typedef struct {
    uint8_t mode;        /* feb_cluster_scan_mode_t */
    uint8_t dwell_mode;   /* feb_cluster_dwell_mode_t */
    uint8_t band_filter;  /* feb_cluster_band_filter_t */
} feb_cluster_scan_config_t;

/* ---- 0x03 SCAN_RESULT ---- worker -> coordinator, one per discovered AP. */
#define FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN 32u
#define FEB_CLUSTER_SCAN_RESULT_BSSID_LEN 6u

/* phy/auth reuse the exact concepts (and, for auth, the exact numeric values) documented
   in components/feb_protocol/cbor_wifi_scan.h/.c and docs/PROTOCOL.md's `<ap-result>`
   table -- per docs/CLUSTER.md: "no new encoding is invented for values that cross back
   into the Flipper-facing wire format later". Important nuance found while implementing:
   cbor_wifi_scan.h itself stores phy/auth as caller-owned *text* (the BLE wire encodes
   them as CBOR text strings, generated by each board's wifi_scan_phy_str()/
   wifi_scan_auth_str() in main.c) -- there is no pre-existing C enum in that header to
   literally reuse. What *is* pinned and reusable:
     - auth's string list in docs/PROTOCOL.md is stated to match, in order, the pinned
       ESP-IDF v5.5.2 wifi_auth_mode_t's own numeric values exactly (open=0 ...
       wpa_enterprise=16), so FEB_CLUSTER_AUTH_* below is that same numeric enum, plus a
       trailing UNKNOWN=17 for any value PROTOCOL.md's table folds into "unknown" (mirrors
       wifi_auth_mode_t's own WIFI_AUTH_MAX sentinel position).
     - phy has no analogous existing ESP-IDF enum type (it's derived from
       wifi_ap_record_t's phy_11b/g/n/ax bitflags in board main.c, highest-generation-wins);
       FEB_CLUSTER_PHY_* below assigns 0..3 in docs/PROTOCOL.md's listed string order
       ("11b","11g","11n","11ax"), which is newly assigned here, not read from an existing
       enum. Flag both of these for promotion into a genuinely shared header (e.g. a new
       small header under components/feb_protocol/) if or when the BLE-facing protocol
       ever needs to cross these same numeric IDs instead of text -- right now nothing
       outside this component defines them numerically, so there was nothing to import. */
typedef enum {
    FEB_CLUSTER_PHY_11B = 0,
    FEB_CLUSTER_PHY_11G = 1,
    FEB_CLUSTER_PHY_11N = 2,
    FEB_CLUSTER_PHY_11AX = 3,
} feb_cluster_phy_t;

typedef enum {
    FEB_CLUSTER_AUTH_OPEN = 0,
    FEB_CLUSTER_AUTH_WEP = 1,
    FEB_CLUSTER_AUTH_WPA_PSK = 2,
    FEB_CLUSTER_AUTH_WPA2_PSK = 3,
    FEB_CLUSTER_AUTH_WPA_WPA2_PSK = 4,
    FEB_CLUSTER_AUTH_WPA2_ENTERPRISE = 5,
    FEB_CLUSTER_AUTH_WPA3_PSK = 6,
    FEB_CLUSTER_AUTH_WPA2_WPA3_PSK = 7,
    FEB_CLUSTER_AUTH_WAPI_PSK = 8,
    FEB_CLUSTER_AUTH_OWE = 9,
    FEB_CLUSTER_AUTH_WPA3_ENT_192 = 10,
    FEB_CLUSTER_AUTH_WPA3_EXT_PSK = 11,
    FEB_CLUSTER_AUTH_WPA3_EXT_PSK_MIXED_MODE = 12,
    FEB_CLUSTER_AUTH_DPP = 13,
    FEB_CLUSTER_AUTH_WPA3_ENTERPRISE = 14,
    FEB_CLUSTER_AUTH_WPA2_WPA3_ENTERPRISE = 15,
    FEB_CLUSTER_AUTH_WPA_ENTERPRISE = 16,
    FEB_CLUSTER_AUTH_UNKNOWN = 17,
} feb_cluster_auth_t;

typedef struct {
    uint8_t ssid[FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN];
    uint8_t ssid_len; /* 0..FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN */
    uint8_t bssid[FEB_CLUSTER_SCAN_RESULT_BSSID_LEN];
    int8_t rssi;
    uint8_t channel;
    uint8_t phy;  /* feb_cluster_phy_t */
    uint8_t auth; /* feb_cluster_auth_t */
} feb_cluster_scan_result_t;

/* ---- 0x04 SCAN_BATCH_DONE ---- worker -> coordinator, mode=2 (manual) only. */
typedef struct {
    uint16_t count;
} feb_cluster_scan_batch_done_t;

/* ---- Encoding ----
   Each encode function writes one complete framed byte sequence (SOF..CRC) into `out`
   (capacity out_cap) and returns the number of bytes written, or 0 if `out`/`msg` is
   NULL, the message's payload would exceed FEB_CLUSTER_MAX_PAYLOAD (only reachable via
   scan_result's ssid_len > FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN, since every other
   message's payload size is fixed and small), or out_cap is smaller than the frame this
   message needs. A buffer sized FEB_CLUSTER_MAX_FRAME_SIZE always fits any valid message
   of any of the four types. */
size_t feb_cluster_encode_worker_hello(uint8_t *out, size_t out_cap,
                                        const feb_cluster_worker_hello_t *msg);
size_t feb_cluster_encode_scan_config_set(uint8_t *out, size_t out_cap,
                                           const feb_cluster_scan_config_t *msg);
size_t feb_cluster_encode_scan_result(uint8_t *out, size_t out_cap,
                                       const feb_cluster_scan_result_t *msg);
size_t feb_cluster_encode_scan_batch_done(uint8_t *out, size_t out_cap,
                                           const feb_cluster_scan_batch_done_t *msg);

/* ---- Streaming decoder ----
   feb_cluster_frame_t is the generic decoded-and-validated frame (msg_type + raw payload
   bytes); the message-specific feb_cluster_decode_*() helpers below unpack one of these
   into its typed struct. */
typedef struct {
    uint8_t msg_type;
    uint16_t payload_len;
    uint8_t payload[FEB_CLUSTER_MAX_PAYLOAD];
} feb_cluster_frame_t;

typedef enum {
    FEB_CLUSTER_DECODE_STATE_SEEK_SOF0 = 0,
    FEB_CLUSTER_DECODE_STATE_SEEK_SOF1,
    FEB_CLUSTER_DECODE_STATE_MSG_TYPE,
    FEB_CLUSTER_DECODE_STATE_LEN_LO,
    FEB_CLUSTER_DECODE_STATE_LEN_HI,
    FEB_CLUSTER_DECODE_STATE_PAYLOAD,
    FEB_CLUSTER_DECODE_STATE_CRC_LO,
    FEB_CLUSTER_DECODE_STATE_CRC_HI,
} feb_cluster_decode_state_t;

typedef struct {
    feb_cluster_decode_state_t state;
    uint8_t msg_type;
    uint16_t payload_len;   /* declared length, validated <= FEB_CLUSTER_MAX_PAYLOAD */
    uint16_t payload_pos;   /* payload bytes received so far in the current frame */
    uint16_t crc_running;   /* CRC over msg_type/payload_len/payload received so far */
    uint8_t crc_lo;         /* received CRC low byte, held while awaiting the high byte */
    uint8_t payload[FEB_CLUSTER_MAX_PAYLOAD];
} feb_cluster_decoder_t;

void feb_cluster_decoder_init(feb_cluster_decoder_t *dec);

typedef enum {
    FEB_CLUSTER_DECODE_NEED_MORE = 0, /* byte consumed, no complete frame yet */
    FEB_CLUSTER_DECODE_FRAME_READY,    /* byte consumed, *out_frame is valid */
    FEB_CLUSTER_DECODE_RESYNC,         /* byte consumed; an in-progress frame was dropped
                                          (bad CRC or oversized payload_len) and the
                                          decoder is scanning for the next SOF */
} feb_cluster_decode_result_t;

/* Feeds exactly one received byte into the decoder's state machine. Returns
   FEB_CLUSTER_DECODE_FRAME_READY (and fills *out_frame) when that byte completed a
   validated frame, FEB_CLUSTER_DECODE_RESYNC when that byte's frame failed validation
   (dec has already reset to scan for the next SOF; *out_frame is untouched), or
   FEB_CLUSTER_DECODE_NEED_MORE otherwise. `out_frame` may be NULL if the caller only
   cares about the result code (e.g. tests exercising resync behavior). */
feb_cluster_decode_result_t feb_cluster_decoder_feed_byte(feb_cluster_decoder_t *dec,
                                                           uint8_t byte,
                                                           feb_cluster_frame_t *out_frame);

/* Convenience wrapper over feb_cluster_decoder_feed_byte() for a chunk of `len` bytes.
   Every completed frame is appended to `frames` (capacity `max_frames`); frames beyond
   `max_frames` are silently dropped (decoding continues correctly, only the return to
   the caller is truncated) -- callers that must not lose a frame under a bursty read
   should size max_frames to the worst case for one read, or call
   feb_cluster_decoder_feed_byte() directly. Returns the number of frames written to
   `frames` (0..max_frames). */
size_t feb_cluster_decoder_feed(feb_cluster_decoder_t *dec, const uint8_t *data, size_t len,
                                 feb_cluster_frame_t *frames, size_t max_frames);

/* ---- Per-message decode helpers ----
   Each unpacks a feb_cluster_frame_t already produced by the decoder above into its typed
   struct, validating msg_type and payload_len match what that message type requires.
   Return 1 (and fill *out) on success, 0 otherwise (wrong msg_type, wrong/inconsistent
   payload_len, or a NULL argument). */
int feb_cluster_decode_worker_hello(const feb_cluster_frame_t *frame,
                                     feb_cluster_worker_hello_t *out);
int feb_cluster_decode_scan_config_set(const feb_cluster_frame_t *frame,
                                        feb_cluster_scan_config_t *out);
int feb_cluster_decode_scan_result(const feb_cluster_frame_t *frame,
                                    feb_cluster_scan_result_t *out);
int feb_cluster_decode_scan_batch_done(const feb_cluster_frame_t *frame,
                                        feb_cluster_scan_batch_done_t *out);

#endif /* FEB_CLUSTER_LINK_H */

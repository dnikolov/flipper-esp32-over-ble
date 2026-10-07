#ifndef FEB_AGPS_CASIC_H
#define FEB_AGPS_CASIC_H

/* Pure (no ESP-IDF) CASIC framing, AID-INI builder, ACK/NACK receive parser and the
   frame-by-frame feeder for the `agps` capability (docs/AGPS.md "Build contract",
   docs/PROTOCOL.md "`agps` command and status payloads"). Host-tested by
   tests/esp32/test_agps.c. Layouts come from the throwaway esp32c5_agps_spike (hardware-
   verified on the ATGM336H, all 34 messages ACKed). Float/double fields are copied with
   memcpy and rely on a little-endian host (RISC-V ESP32-C5 and the x86 test host both are). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEB_AGPS_BLOB_MAX 4096u
#define FEB_AGPS_MAX_FRAMES 64u
#define FEB_CASIC_OVERHEAD 10u /* BA CE + len16 + class + id + checksum32 */
#define FEB_CASIC_CLASS_ACK 0x05u
#define FEB_CASIC_CLASS_AID 0x0Bu
#define FEB_CASIC_ID_AID_INI 0x01u
#define FEB_CASIC_CLASS_MSG 0x08u
/* The module's UART TX is shared with ~960 B/s of NMEA at 9600 baud, so an ACK can queue behind
   several hundred ms of text (hardware 2026-10-07: 32/34 ACKs at 500 ms). */
#define FEB_CASIC_ACK_TIMEOUT_MS 1500u
/* Bound on the pre-send drain of already-received bytes (only matters while NMEA streams). */
#define FEB_CASIC_DRAIN_MAX_MS 50u
/* Whole-feed budget, checked before each frame. Worst case is therefore
   FEED_BUDGET + ACK_TIMEOUT + DRAIN = 20000 + 1500 + 50 ms plus the final write, ~21.6 s, inside
   the Flipper's 30 s complete-wait and the 30 s BLE idle timeout (3 consecutive timeouts alone
   would end it after 4.5 s; without the budget 2-timeouts-then-ACK patterns could reach ~34 s). */
#define FEB_AGPS_FEED_BUDGET_MS 20000u
#define FEB_AGPS_INI_PAYLOAD_LEN 56u
#define FEB_AGPS_INI_LEN (FEB_CASIC_OVERHEAD + FEB_AGPS_INI_PAYLOAD_LEN)
/* Claimed 1-sigma host time error: the Sync script sets the Flipper RTC from the PC just before. */
#define FEB_AGPS_TIME_ERR_S 5u
/* Claimed 1-sigma position error when a hint is sent. The spike validated 50 km; 100 km is
   unvalidated (a last-known fix can be far from here). */
#define FEB_AGPS_POS_ERR_M 100000.0
/* Abort a feed after this many frames in a row get no ACK/NACK (module not listening). */
#define FEB_AGPS_MAX_CONSECUTIVE_TIMEOUTS 3u

typedef enum {
    FEB_CASIC_OK = 0,
    FEB_CASIC_ERR_EMPTY,
    FEB_CASIC_ERR_TRUNCATED, /* partial header, or declared length runs past the blob */
    FEB_CASIC_ERR_SYNC,      /* frame does not start with BA CE */
    FEB_CASIC_ERR_LENGTH,    /* payload length not a multiple of 4 */
    FEB_CASIC_ERR_CHECKSUM,
    FEB_CASIC_ERR_CLASS,     /* only class 0x08 (MSG) frames may be fed */
    FEB_CASIC_ERR_TOO_MANY,
    FEB_CASIC_ERR_TOO_LARGE,
} feb_casic_status_t;

uint32_t feb_casic_checksum(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len);

/* Walks `blob` as back-to-back whole CASIC frames and requires it to end exactly on a frame
   boundary. Nothing may be sent to the GPS unless this returns FEB_CASIC_OK. */
feb_casic_status_t feb_casic_validate_frames(const uint8_t *blob, size_t len, uint32_t *frame_count);

typedef struct {
    bool valid;
    int32_t lat_e7;
    int32_t lon_e7;
    int32_t alt_dm;
} feb_agps_position_t;

/* AID-INI with flags 0x02 (time) or 0x03 (time + position). `pos` may be NULL. */
void feb_agps_build_ini(uint8_t out[FEB_AGPS_INI_LEN], uint64_t unix_ms, const feb_agps_position_t *pos);

typedef enum {
    FEB_CASIC_RX_NONE = 0,
    FEB_CASIC_RX_ACK,
    FEB_CASIC_RX_NACK,
} feb_casic_rx_event_t;

#define FEB_CASIC_RX_BUF 96u

/* Byte-wise receive parser: skips NMEA lines, reports checksum-valid ACK/NACK frames. */
typedef struct {
    uint8_t buf[FEB_CASIC_RX_BUF];
    uint16_t count;
    uint16_t need;
    uint8_t state;
} feb_casic_rx_t;

void feb_casic_rx_init(feb_casic_rx_t *rx);
/* True while a binary frame is partially received. */
bool feb_casic_rx_busy(const feb_casic_rx_t *rx);
feb_casic_rx_event_t feb_casic_rx_byte(feb_casic_rx_t *rx, uint8_t byte, uint8_t *acked_cls, uint8_t *acked_id);

typedef struct {
    void *ctx;
    void (*write)(void *ctx, const uint8_t *data, size_t len);
    /* Returns bytes read (0 on timeout). */
    size_t (*read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    uint32_t (*now_ms)(void *ctx);
    /* Optional, called once per sent frame: outcome 0 timeout / 1 ACK / 2 NACK. */
    void (*frame_log)(void *ctx, const uint8_t *frame, int outcome, uint32_t waited_ms);
} feb_agps_io_t;

/* The first four fields are the wire result; `late` and `elapsed_ms` are local diagnostics. */
typedef struct {
    uint16_t sent;
    uint16_t acked;
    uint16_t nacked;
    uint16_t timeout;
    uint16_t late;        /* ACK/NACK frames discarded by the pre-send drain */
    uint32_t elapsed_ms;
} feb_agps_result_t;

/* Sends `ini` then each frame of the (already validated) `blob`, strictly one whole frame at a
   time: drain already-received bytes, send, wait up to FEB_CASIC_ACK_TIMEOUT_MS for the matching
   ACK/NACK (by class/id) before the next. Stops when FEB_AGPS_FEED_BUDGET_MS has elapsed. Counts include the AID-INI frame. Stops early after FEB_AGPS_MAX_CONSECUTIVE_TIMEOUTS
   unanswered frames in a row. Never sends anything but the frames given. */
void feb_agps_feed(const feb_agps_io_t *io, const uint8_t *ini, const uint8_t *blob, size_t blob_len,
                   feb_agps_result_t *result);

#endif

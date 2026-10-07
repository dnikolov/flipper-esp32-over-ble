#include "feb_agps_casic.h"

#include <math.h>
#include <string.h>

#define GPS_EPOCH_UNIX_S 315964800LL
#define GPS_LEAP_S 18LL
#define SPEED_OF_LIGHT_MPS 299792458.0

enum { RX_IDLE = 0, RX_NMEA, RX_BIN };

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t feb_casic_checksum(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
    uint32_t sum = ((uint32_t)id << 24) + ((uint32_t)cls << 16) + len;
    uint16_t i;

    for (i = 0; i < len / 4u; i++) {
        sum += rd_u32(payload + 4u * i);
    }
    return sum;
}

feb_casic_status_t feb_casic_validate_frames(const uint8_t *blob, size_t len, uint32_t *frame_count)
{
    size_t pos = 0;
    uint32_t count = 0;

    if (frame_count != NULL) {
        *frame_count = 0;
    }
    if (blob == NULL || len == 0) {
        return FEB_CASIC_ERR_EMPTY;
    }
    if (len > FEB_AGPS_BLOB_MAX) {
        return FEB_CASIC_ERR_TOO_LARGE;
    }
    while (pos < len) {
        size_t remaining = len - pos;
        const uint8_t *f = blob + pos;
        uint16_t plen;

        if (remaining < FEB_CASIC_OVERHEAD) {
            return FEB_CASIC_ERR_TRUNCATED;
        }
        if (f[0] != 0xBAu || f[1] != 0xCEu) {
            return FEB_CASIC_ERR_SYNC;
        }
        plen = rd_u16(f + 2);
        if ((plen & 3u) != 0u) {
            return FEB_CASIC_ERR_LENGTH;
        }
        if ((size_t)plen + FEB_CASIC_OVERHEAD > remaining) {
            return FEB_CASIC_ERR_TRUNCATED;
        }
        if (f[4] != FEB_CASIC_CLASS_MSG) {
            return FEB_CASIC_ERR_CLASS;
        }
        if (rd_u32(f + 6 + plen) != feb_casic_checksum(f[4], f[5], f + 6, plen)) {
            return FEB_CASIC_ERR_CHECKSUM;
        }
        if (++count > FEB_AGPS_MAX_FRAMES) {
            return FEB_CASIC_ERR_TOO_MANY;
        }
        pos += (size_t)plen + FEB_CASIC_OVERHEAD;
    }
    if (frame_count != NULL) {
        *frame_count = count;
    }
    return FEB_CASIC_OK;
}

void feb_agps_build_ini(uint8_t out[FEB_AGPS_INI_LEN], uint64_t unix_ms, const feb_agps_position_t *pos)
{
    uint8_t *pl = out + 6;
    int64_t gps_ms = (int64_t)unix_ms - (GPS_EPOCH_UNIX_S - GPS_LEAP_S) * 1000LL;
    uint16_t week = (uint16_t)(gps_ms / 604800000LL);
    double tow = (double)(gps_ms % 604800000LL) / 1000.0;
    double ecef[3] = {0.0, 0.0, 0.0};
    float freq_bias = 0.0f;
    float pos_acc = (float)(FEB_AGPS_POS_ERR_M * FEB_AGPS_POS_ERR_M);
    double time_err_m = (double)FEB_AGPS_TIME_ERR_S * SPEED_OF_LIGHT_MPS;
    float time_acc = (float)(time_err_m * time_err_m);
    float freq_acc = 0.0f;
    uint8_t flags = 0x02u;

    memset(out, 0, FEB_AGPS_INI_LEN);
    out[0] = 0xBAu;
    out[1] = 0xCEu;
    out[2] = (uint8_t)FEB_AGPS_INI_PAYLOAD_LEN;
    out[3] = 0;
    out[4] = FEB_CASIC_CLASS_AID;
    out[5] = FEB_CASIC_ID_AID_INI;

    if (pos != NULL && pos->valid) {
        const double a = 6378137.0;
        const double f = 1.0 / 298.257223563;
        const double e2 = f * (2.0 - f);
        double lat = ((double)pos->lat_e7 * 1e-7) * 3.14159265358979323846 / 180.0;
        double lon = ((double)pos->lon_e7 * 1e-7) * 3.14159265358979323846 / 180.0;
        double alt = (double)pos->alt_dm / 10.0;
        double sin_lat = sin(lat);
        double n = a / sqrt(1.0 - e2 * sin_lat * sin_lat);

        ecef[0] = (n + alt) * cos(lat) * cos(lon);
        ecef[1] = (n + alt) * cos(lat) * sin(lon);
        ecef[2] = (n * (1.0 - e2) + alt) * sin_lat;
        flags |= 0x01u;
    }

    memcpy(pl + 0, &ecef[0], 8);
    memcpy(pl + 8, &ecef[1], 8);
    memcpy(pl + 16, &ecef[2], 8);
    memcpy(pl + 24, &tow, 8);
    memcpy(pl + 32, &freq_bias, 4);
    memcpy(pl + 36, &pos_acc, 4);
    memcpy(pl + 40, &time_acc, 4);
    memcpy(pl + 44, &freq_acc, 4);
    /* pl[48..51] reserved = 0 */
    pl[52] = (uint8_t)week;
    pl[53] = (uint8_t)(week >> 8);
    pl[54] = 0; /* timeSource 0 = GPS */
    pl[55] = flags;
    wr_u32(pl + FEB_AGPS_INI_PAYLOAD_LEN,
           feb_casic_checksum(FEB_CASIC_CLASS_AID, FEB_CASIC_ID_AID_INI, pl, FEB_AGPS_INI_PAYLOAD_LEN));
}

void feb_casic_rx_init(feb_casic_rx_t *rx)
{
    rx->count = 0;
    rx->need = 0;
    rx->state = RX_IDLE;
}

static feb_casic_rx_event_t rx_finish_frame(feb_casic_rx_t *rx, uint8_t *acked_cls, uint8_t *acked_id)
{
    uint16_t plen = rd_u16(rx->buf + 2);
    uint8_t cls = rx->buf[4];
    uint8_t id = rx->buf[5];

    if (rd_u32(rx->buf + 6 + plen) != feb_casic_checksum(cls, id, rx->buf + 6, plen)) {
        return FEB_CASIC_RX_NONE;
    }
    if (cls != FEB_CASIC_CLASS_ACK || plen != 4u || id > 1u) {
        return FEB_CASIC_RX_NONE;
    }
    *acked_cls = rx->buf[6];
    *acked_id = rx->buf[7];
    return id == 1u ? FEB_CASIC_RX_ACK : FEB_CASIC_RX_NACK;
}

feb_casic_rx_event_t feb_casic_rx_byte(feb_casic_rx_t *rx, uint8_t byte, uint8_t *acked_cls, uint8_t *acked_id)
{
    if (rx->state == RX_BIN) {
        rx->buf[rx->count++] = byte;
        if (rx->count == 2u && byte != 0xCEu) {
            rx->state = RX_IDLE;
            return FEB_CASIC_RX_NONE;
        }
        if (rx->count == 6u) {
            uint16_t plen = rd_u16(rx->buf + 2);

            if (plen > FEB_CASIC_RX_BUF - FEB_CASIC_OVERHEAD) {
                rx->state = RX_IDLE;
                return FEB_CASIC_RX_NONE;
            }
            rx->need = (uint16_t)(FEB_CASIC_OVERHEAD + plen);
        }
        if (rx->need != 0u && rx->count == rx->need) {
            rx->state = RX_IDLE;
            return rx_finish_frame(rx, acked_cls, acked_id);
        }
        return FEB_CASIC_RX_NONE;
    }
    if (byte == '$') {
        rx->state = RX_NMEA;
        rx->count = 0;
        return FEB_CASIC_RX_NONE;
    }
    if (rx->state == RX_NMEA) {
        if (byte == '\n' || ++rx->count > 120u) {
            rx->state = RX_IDLE;
        }
        return FEB_CASIC_RX_NONE;
    }
    if (byte == 0xBAu) {
        rx->buf[0] = byte;
        rx->count = 1;
        rx->need = 0;
        rx->state = RX_BIN;
    }
    return FEB_CASIC_RX_NONE;
}

bool feb_casic_rx_busy(const feb_casic_rx_t *rx) { return rx->state == RX_BIN; }

/* An ACK/NACK frame carries only the acked class/id (+2 reserved bytes), so back-to-back frames
   of one class/id (all the EPH frames are 08/07) cannot be told apart by their ACKs. The only
   sound scheme is lock-step: one frame in flight, wait for its ACK or the timeout, and throw away
   whatever is already received before the next send, so an ACK that arrived after its frame timed
   out is never credited to the following frame. An ACK that lands after the next send has
   started is indistinguishable and can only be made unlikely by a generous timeout. */
static void drain_late(const feb_agps_io_t *io, feb_casic_rx_t *rx, feb_agps_result_t *result)
{
    uint8_t rd[32];
    uint32_t start = io->now_ms(io->ctx);

    feb_casic_rx_init(rx);
    for (;;) {
        size_t n = io->read(io->ctx, rd, sizeof(rd), feb_casic_rx_busy(rx) ? 5u : 0u);
        size_t i;

        for (i = 0; i < n; i++) {
            uint8_t c = 0;
            uint8_t d = 0;

            if (feb_casic_rx_byte(rx, rd[i], &c, &d) != FEB_CASIC_RX_NONE) {
                result->late++;
            }
        }
        if (n == 0 && !feb_casic_rx_busy(rx)) {
            break;
        }
        if ((uint32_t)(io->now_ms(io->ctx) - start) >= FEB_CASIC_DRAIN_MAX_MS) {
            break;
        }
    }
    feb_casic_rx_init(rx);
}

/* Returns 0 timeout, 1 ACK, 2 NACK. */
static int send_one(const feb_agps_io_t *io, feb_casic_rx_t *rx, const uint8_t *frame, size_t frame_len,
                    feb_agps_result_t *result, uint32_t *waited_ms)
{
    uint8_t cls = frame[4];
    uint8_t id = frame[5];
    uint8_t rd[32];
    uint32_t start;
    int outcome = 0;

    drain_late(io, rx, result);
    io->write(io->ctx, frame, frame_len);
    start = io->now_ms(io->ctx);
    while (outcome == 0 && (uint32_t)(io->now_ms(io->ctx) - start) < FEB_CASIC_ACK_TIMEOUT_MS) {
        size_t n = io->read(io->ctx, rd, sizeof(rd), 10u);
        size_t i;

        for (i = 0; i < n && outcome == 0; i++) {
            uint8_t ack_cls = 0;
            uint8_t ack_id = 0;
            feb_casic_rx_event_t ev = feb_casic_rx_byte(rx, rd[i], &ack_cls, &ack_id);

            if (ev != FEB_CASIC_RX_NONE && ack_cls == cls && ack_id == id) {
                outcome = ev == FEB_CASIC_RX_ACK ? 1 : 2;
            }
        }
    }
    *waited_ms = (uint32_t)(io->now_ms(io->ctx) - start);
    return outcome;
}

void feb_agps_feed(const feb_agps_io_t *io, const uint8_t *ini, const uint8_t *blob, size_t blob_len,
                   feb_agps_result_t *result)
{
    feb_casic_rx_t rx;
    size_t pos = 0;
    unsigned consecutive_timeouts = 0;
    uint32_t feed_start = io->now_ms(io->ctx);
    uint32_t waited = 0;
    int r;

    memset(result, 0, sizeof(*result));
    for (;;) {
        const uint8_t *frame;
        size_t frame_len;

        if ((uint32_t)(io->now_ms(io->ctx) - feed_start) >= FEB_AGPS_FEED_BUDGET_MS) {
            break;
        }
        if (ini != NULL) {
            frame = ini;
            frame_len = FEB_AGPS_INI_LEN;
            ini = NULL;
        } else {
            if (pos + FEB_CASIC_OVERHEAD > blob_len) {
                break;
            }
            frame = blob + pos;
            frame_len = (size_t)rd_u16(frame + 2) + FEB_CASIC_OVERHEAD;
            if (frame[0] != 0xBAu || frame[1] != 0xCEu || pos + frame_len > blob_len) {
                break;
            }
            pos += frame_len;
        }
        r = send_one(io, &rx, frame, frame_len, result, &waited);
        result->sent++;
        if (io->frame_log != NULL) {
            io->frame_log(io->ctx, frame, r, waited);
        }
        if (r == 1) {
            result->acked++;
            consecutive_timeouts = 0;
        } else if (r == 2) {
            result->nacked++;
            consecutive_timeouts = 0;
        } else {
            result->timeout++;
            if (++consecutive_timeouts >= FEB_AGPS_MAX_CONSECUTIVE_TIMEOUTS) {
                break;
            }
        }
    }
    result->elapsed_ms = (uint32_t)(io->now_ms(io->ctx) - feed_start);
}

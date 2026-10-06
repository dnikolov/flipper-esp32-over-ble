/* Host-native tests for the `agps` capability's pure modules (components/feb_app_core/
   feb_agps_casic.c, feb_agps_blob.c, feb_agps_cmd.c): CASIC frame validation, AID-INI layout,
   ACK/NACK receive parsing, the frame feeder against a mock UART, the chunk-reassembly state
   machine, and the command-argument codec -- including malformed/hostile inputs. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "feb_agps_blob.h"
#include "feb_agps_casic.h"
#include "feb_agps_cmd.h"

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

static size_t make_frame(uint8_t *out, uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
    uint32_t ck = feb_casic_checksum(cls, id, payload, len);

    out[0] = 0xBA;
    out[1] = 0xCE;
    out[2] = (uint8_t)len;
    out[3] = (uint8_t)(len >> 8);
    out[4] = cls;
    out[5] = id;
    if (len != 0) {
        memcpy(out + 6, payload, len);
    }
    out[6 + len] = (uint8_t)ck;
    out[7 + len] = (uint8_t)(ck >> 8);
    out[8 + len] = (uint8_t)(ck >> 16);
    out[9 + len] = (uint8_t)(ck >> 24);
    return (size_t)len + 10u;
}

/* A realistic-shaped window: n * GPSEPH (08/07, 72 B) followed by one GPSION (08/06, 16 B). */
static size_t make_window(uint8_t *out, unsigned eph_count)
{
    uint8_t payload[72];
    size_t pos = 0;
    unsigned i;

    for (i = 0; i < eph_count; i++) {
        memset(payload, (int)(i + 1), sizeof(payload));
        pos += make_frame(out + pos, 0x08, 0x07, payload, 72);
    }
    memset(payload, 0x55, 16);
    pos += make_frame(out + pos, 0x08, 0x06, payload, 16);
    return pos;
}

static void test_validator(void)
{
    static uint8_t blob[FEB_AGPS_BLOB_MAX + 64];
    uint32_t count = 99;
    size_t len = make_window(blob, 32);
    uint8_t copy[FEB_AGPS_BLOB_MAX];

    check(len == 32u * 82u + 26u, "window is 32 EPH + 1 ION = 2650 bytes");
    check(feb_casic_validate_frames(blob, len, &count) == FEB_CASIC_OK && count == 33u, "valid 33-frame window");

    check(feb_casic_validate_frames(blob, 0, &count) == FEB_CASIC_ERR_EMPTY && count == 0, "empty blob rejected");
    check(feb_casic_validate_frames(NULL, 10, &count) == FEB_CASIC_ERR_EMPTY, "NULL blob rejected");
    check(feb_casic_validate_frames(blob, FEB_AGPS_BLOB_MAX + 1, &count) == FEB_CASIC_ERR_TOO_LARGE,
          "blob over 4096 rejected");

    memcpy(copy, blob, len);
    copy[0] = 0xBB;
    check(feb_casic_validate_frames(copy, len, &count) == FEB_CASIC_ERR_SYNC, "bad first sync byte");
    memcpy(copy, blob, len);
    copy[1] = 0x00;
    check(feb_casic_validate_frames(copy, len, &count) == FEB_CASIC_ERR_SYNC, "bad second sync byte");

    memcpy(copy, blob, len);
    copy[6 + 3] ^= 0x01; /* flip a payload bit of frame 0 */
    check(feb_casic_validate_frames(copy, len, &count) == FEB_CASIC_ERR_CHECKSUM, "payload bit flip -> checksum");
    memcpy(copy, blob, len);
    copy[6 + 72] ^= 0x80; /* checksum byte of frame 0 */
    check(feb_casic_validate_frames(copy, len, &count) == FEB_CASIC_ERR_CHECKSUM, "checksum byte flip");
    memcpy(copy, blob, len);
    copy[len - 1] ^= 0x01; /* last frame's checksum */
    check(feb_casic_validate_frames(copy, len, &count) == FEB_CASIC_ERR_CHECKSUM, "bad checksum on last frame");
    check(count == 0, "frame_count stays 0 on failure");

    check(feb_casic_validate_frames(blob, len - 1, &count) == FEB_CASIC_ERR_TRUNCATED, "last frame truncated by 1");
    check(feb_casic_validate_frames(blob, 9, &count) == FEB_CASIC_ERR_TRUNCATED, "shorter than minimal frame");
    check(feb_casic_validate_frames(blob, 82u + 5u, &count) == FEB_CASIC_ERR_TRUNCATED, "partial header after frame 0");

    memcpy(copy, blob, len);
    copy[2] = 0xFF;
    copy[3] = 0xFF; /* length 65535 -> not a multiple of 4 */
    check(feb_casic_validate_frames(copy, len, &count) == FEB_CASIC_ERR_LENGTH, "length 0xFFFF (not x4) rejected");
    memcpy(copy, blob, len);
    copy[2] = 0xFC;
    copy[3] = 0xFF; /* 65532: multiple of 4 but overruns */
    check(feb_casic_validate_frames(copy, len, &count) == FEB_CASIC_ERR_TRUNCATED, "length overrun rejected");
    memcpy(copy, blob, len);
    copy[2] = 0x4C; /* 76: frame 0 would swallow 4 bytes of frame 1 */
    check(feb_casic_validate_frames(copy, len, &count) != FEB_CASIC_OK, "length longer than real payload rejected");
    memcpy(copy, blob, len);
    copy[2] = 0x44; /* 68: shorter than real payload -> next frame start is mid-payload */
    check(feb_casic_validate_frames(copy, len, &count) != FEB_CASIC_OK, "length shorter than real payload rejected");
    memcpy(copy, blob, len);
    copy[2] = 0x49; /* 73: odd length */
    check(feb_casic_validate_frames(copy, len, &count) == FEB_CASIC_ERR_LENGTH, "odd length rejected");

    memcpy(copy, blob, len);
    copy[len] = 0x00;
    check(feb_casic_validate_frames(copy, len + 1, &count) == FEB_CASIC_ERR_TRUNCATED, "1 trailing garbage byte");
    {
        uint8_t big[FEB_AGPS_BLOB_MAX];
        size_t n = make_window(big, 32);

        big[n] = 'x';
        check(feb_casic_validate_frames(big, n + 12, &count) != FEB_CASIC_OK, "trailing garbage block rejected");
    }

    {
        uint8_t f[10];
        uint8_t p[4] = {0, 0, 0, 0};

        make_frame(f, 0x06, 0x00, p, 0);
        check(feb_casic_validate_frames(f, 10, &count) == FEB_CASIC_ERR_CLASS, "CFG class frame rejected");
        make_frame(f, 0x05, 0x01, p, 0);
        check(feb_casic_validate_frames(f, 10, &count) == FEB_CASIC_ERR_CLASS, "ACK class frame rejected");
        make_frame(f, 0x0B, 0x01, p, 0);
        check(feb_casic_validate_frames(f, 10, &count) == FEB_CASIC_ERR_CLASS, "AID class frame in blob rejected");
        make_frame(f, 0x08, 0x06, p, 0);
        check(feb_casic_validate_frames(f, 10, &count) == FEB_CASIC_OK && count == 1u, "empty-payload MSG frame ok");
    }

    {
        static uint8_t many[FEB_AGPS_BLOB_MAX];
        size_t pos = 0;
        unsigned i;

        for (i = 0; i < FEB_AGPS_MAX_FRAMES; i++) {
            pos += make_frame(many + pos, 0x08, 0x07, NULL, 0);
        }
        check(feb_casic_validate_frames(many, pos, &count) == FEB_CASIC_OK && count == FEB_AGPS_MAX_FRAMES,
              "exactly 64 frames ok");
        pos += make_frame(many + pos, 0x08, 0x07, NULL, 0);
        check(feb_casic_validate_frames(many, pos, &count) == FEB_CASIC_ERR_TOO_MANY, "65 frames rejected");
    }

    {
        /* A CASIC blob must not smuggle an NMEA command: '$PCAS' text is not a BA CE frame. */
        static const char nmea[] = "$PCAS10,2*1E\r\n";

        check(feb_casic_validate_frames((const uint8_t *)nmea, sizeof(nmea) - 1, &count) == FEB_CASIC_ERR_SYNC,
              "NMEA $PCAS text rejected as a blob");
    }
}

static void test_ini(void)
{
    uint8_t ini[FEB_AGPS_INI_LEN];
    uint8_t *pl = ini + 6;
    double tow;
    double x;
    float t_acc;
    float p_acc;
    uint32_t ck;

    /* 2026-10-06 12:00:00 UTC = 1791288000 (checked: 1790000000 is 2026-09-21 12:53:20). */
    feb_agps_build_ini(ini, 1791288000ull * 1000u + 250u, NULL);
    check(ini[0] == 0xBA && ini[1] == 0xCE && ini[2] == 56 && ini[3] == 0 && ini[4] == 0x0B && ini[5] == 0x01,
          "AID-INI header: BA CE len=56 class 0B id 01");
    check(pl[55] == 0x02, "time-only flags = 0x02");
    memcpy(&tow, pl + 24, 8);
    {
        long long gps_ms = 1791288000250LL - (315964800LL - 18LL) * 1000LL;
        double want = (double)(gps_ms % 604800000LL) / 1000.0;
        unsigned wn = (unsigned)(gps_ms / 604800000LL);

        check(fabs(tow - want) < 1e-6, "time of week matches GPS time (UTC + 18 s leap)");
        check((unsigned)(pl[52] | (pl[53] << 8)) == wn, "week number matches");
    }
    memcpy(&t_acc, pl + 40, 4);
    check(fabs((double)t_acc - (5.0 * 299792458.0) * (5.0 * 299792458.0)) < 1e12, "tAcc = (5 s * c)^2");
    memcpy(&p_acc, pl + 36, 4);
    check(p_acc > 0.0f, "pAcc set");
    memcpy(&x, pl + 0, 8);
    check(x == 0.0, "no position -> ECEF zero");
    check(pl[54] == 0, "time source GPS");
    ck = (uint32_t)ini[6 + 56] | ((uint32_t)ini[7 + 56] << 8) | ((uint32_t)ini[8 + 56] << 16) |
         ((uint32_t)ini[9 + 56] << 24);
    check(ck == feb_casic_checksum(0x0B, 0x01, pl, 56), "AID-INI checksum valid");

    {
        feb_agps_position_t pos = {true, 426628000, 232887000, 5500}; /* Sofia, 550 m */
        double e[3];
        double r;

        feb_agps_build_ini(ini, 1791288000ull * 1000u, &pos);
        check(pl[55] == 0x03, "time+position flags = 0x03");
        memcpy(e, pl, 24);
        r = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        check(r > 6.35e6 && r < 6.40e6, "ECEF radius is Earth-sized");
        check(fabs(e[0] - 4.2e6) < 0.2e6 && fabs(e[1] - 1.7e6) < 0.2e6 && fabs(e[2] - 4.5e6) < 0.2e6,
              "ECEF for Sofia is plausible");
        ck = (uint32_t)ini[62] | ((uint32_t)ini[63] << 8) | ((uint32_t)ini[64] << 16) | ((uint32_t)ini[65] << 24);
        check(ck == feb_casic_checksum(0x0B, 0x01, pl, 56), "AID-INI w/ position checksum valid");
    }
}

static void test_rx(void)
{
    feb_casic_rx_t rx;
    uint8_t frame[32];
    uint8_t payload[4] = {0x08, 0x07, 0, 0};
    size_t n;
    size_t i;
    uint8_t cls = 0;
    uint8_t id = 0;
    feb_casic_rx_event_t ev = FEB_CASIC_RX_NONE;
    feb_casic_rx_event_t last;
    static const char nmea[] = "$GNGGA,091010.000,,,,,0,00,,,M,,M,,*6F\r\n";

    feb_casic_rx_init(&rx);
    n = make_frame(frame, 0x05, 0x01, payload, 4);
    for (i = 0; i < n; i++) {
        last = feb_casic_rx_byte(&rx, frame[i], &cls, &id);
        if (last != FEB_CASIC_RX_NONE) {
            ev = last;
        }
    }
    check(ev == FEB_CASIC_RX_ACK && cls == 0x08 && id == 0x07, "ACK frame parsed with acked class/id");

    ev = FEB_CASIC_RX_NONE;
    n = make_frame(frame, 0x05, 0x00, payload, 4);
    for (i = 0; i < n; i++) {
        last = feb_casic_rx_byte(&rx, frame[i], &cls, &id);
        if (last != FEB_CASIC_RX_NONE) {
            ev = last;
        }
    }
    check(ev == FEB_CASIC_RX_NACK, "NACK frame parsed");

    ev = FEB_CASIC_RX_NONE;
    for (i = 0; i < sizeof(nmea) - 1; i++) {
        last = feb_casic_rx_byte(&rx, (uint8_t)nmea[i], &cls, &id);
        if (last != FEB_CASIC_RX_NONE) {
            ev = last;
        }
    }
    n = make_frame(frame, 0x05, 0x01, payload, 4);
    for (i = 0; i < n; i++) {
        last = feb_casic_rx_byte(&rx, frame[i], &cls, &id);
        if (last != FEB_CASIC_RX_NONE) {
            ev = last;
        }
    }
    check(ev == FEB_CASIC_RX_ACK, "ACK after an NMEA line");

    ev = FEB_CASIC_RX_NONE;
    n = make_frame(frame, 0x05, 0x01, payload, 4);
    frame[n - 1] ^= 1;
    for (i = 0; i < n; i++) {
        last = feb_casic_rx_byte(&rx, frame[i], &cls, &id);
        if (last != FEB_CASIC_RX_NONE) {
            ev = last;
        }
    }
    check(ev == FEB_CASIC_RX_NONE, "ACK with bad checksum ignored");

    ev = FEB_CASIC_RX_NONE;
    n = make_frame(frame, 0x05, 0x01, payload, 4);
    frame[2] = 0xFF; /* oversized declared length: dropped, no overflow */
    frame[3] = 0x7F;
    for (i = 0; i < n; i++) {
        last = feb_casic_rx_byte(&rx, frame[i], &cls, &id);
        if (last != FEB_CASIC_RX_NONE) {
            ev = last;
        }
    }
    check(ev == FEB_CASIC_RX_NONE, "oversized RX length dropped");
    {
        /* Then a clean ACK still parses (resync). */
        n = make_frame(frame, 0x05, 0x01, payload, 4);
        for (i = 0; i < n; i++) {
            last = feb_casic_rx_byte(&rx, frame[i], &cls, &id);
            if (last != FEB_CASIC_RX_NONE) {
                ev = last;
            }
        }
        check(ev == FEB_CASIC_RX_ACK, "parser resyncs after dropped frame");
    }
}

typedef enum { MOCK_ACK, MOCK_NACK, MOCK_SILENT, MOCK_WRONG_ID, MOCK_BAD_CK } mock_mode_t;

typedef struct {
    uint32_t now;
    uint8_t writes[FEB_AGPS_BLOB_MAX + 128];
    size_t write_total;
    size_t write_calls;
    size_t frame_starts[80];
    uint8_t rx[256];
    size_t rx_len;
    size_t rx_pos;
    mock_mode_t default_mode;
    int mode_at_call;
    mock_mode_t mode_for_call;
    int preface_nmea;
    int split_ack;
} mock_t;

static void mock_write(void *ctx, const uint8_t *data, size_t len)
{
    mock_t *m = ctx;
    uint8_t payload[4] = {0, 0, 0, 0};
    mock_mode_t mode = m->default_mode;

    if ((int)m->write_calls == m->mode_at_call) {
        mode = m->mode_for_call;
    }
    if (m->write_calls < 80) {
        m->frame_starts[m->write_calls] = m->write_total;
    }
    memcpy(m->writes + m->write_total, data, len);
    m->write_total += len;
    m->write_calls++;
    m->rx_len = 0;
    m->rx_pos = 0;
    if (mode == MOCK_SILENT) {
        return;
    }
    if (m->preface_nmea) {
        static const char nmea[] = "$GNRMC,091010.000,V,,,,,0.00,0.00,120926,,,N*54\r\n";

        memcpy(m->rx, nmea, sizeof(nmea) - 1);
        m->rx_len = sizeof(nmea) - 1;
    }
    payload[0] = (uint8_t)(mode == MOCK_WRONG_ID ? data[4] ^ 0x10 : data[4]);
    payload[1] = data[5];
    m->rx_len += make_frame(m->rx + m->rx_len, 0x05, mode == MOCK_NACK ? 0x00 : 0x01, payload, 4);
    if (mode == MOCK_BAD_CK) {
        m->rx[m->rx_len - 1] ^= 0x01;
    }
}

static size_t mock_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    mock_t *m = ctx;
    size_t n = m->rx_len - m->rx_pos;

    if (n == 0) {
        m->now += timeout_ms;
        return 0;
    }
    if (m->split_ack && n > 3) {
        n = 3;
    }
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, m->rx + m->rx_pos, n);
    m->rx_pos += n;
    m->now += 1;
    return n;
}

static uint32_t mock_now(void *ctx)
{
    return ((mock_t *)ctx)->now;
}

static void test_feed(void)
{
    static mock_t m;
    static uint8_t blob[FEB_AGPS_BLOB_MAX];
    uint8_t ini[FEB_AGPS_INI_LEN];
    feb_agps_result_t r;
    feb_agps_io_t io;
    size_t len = make_window(blob, 32);
    size_t i;
    int clean = 1;

    feb_agps_build_ini(ini, 1791288000ull * 1000u, NULL);
    io.ctx = &m;
    io.write = mock_write;
    io.read = mock_read;
    io.now_ms = mock_now;

    memset(&m, 0, sizeof(m));
    m.default_mode = MOCK_ACK;
    m.mode_at_call = -1;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.sent == 34 && r.acked == 34 && r.nacked == 0 && r.timeout == 0, "all 34 frames ACKed (INI + 33)");
    check(m.write_calls == 34, "one write call per whole frame");
    check(memcmp(m.writes, ini, FEB_AGPS_INI_LEN) == 0, "first write is exactly the AID-INI");
    check(memcmp(m.writes + FEB_AGPS_INI_LEN, blob, len) == 0 && m.write_total == FEB_AGPS_INI_LEN + len,
          "remaining bytes are exactly the blob, in order, nothing else");
    for (i = 0; i < m.write_calls; i++) {
        if (m.writes[m.frame_starts[i]] != 0xBA || m.writes[m.frame_starts[i] + 1] != 0xCE) {
            clean = 0;
        }
    }
    check(clean, "every write starts with BA CE (no NMEA/$PCAS ever)");

    memset(&m, 0, sizeof(m));
    m.mode_at_call = 5;
    m.mode_for_call = MOCK_NACK;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.sent == 34 && r.acked == 33 && r.nacked == 1 && r.timeout == 0, "one NACK counted, feed continues");

    memset(&m, 0, sizeof(m));
    m.mode_at_call = 7;
    m.mode_for_call = MOCK_SILENT;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.sent == 34 && r.acked == 33 && r.timeout == 1, "single timeout counted, feed continues");
    check(m.now >= FEB_CASIC_ACK_TIMEOUT_MS, "timeout waited >= 500 ms");

    memset(&m, 0, sizeof(m));
    m.mode_at_call = -1;
    m.default_mode = MOCK_SILENT;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.sent == FEB_AGPS_MAX_CONSECUTIVE_TIMEOUTS && r.timeout == FEB_AGPS_MAX_CONSECUTIVE_TIMEOUTS &&
              r.acked == 0,
          "silent module: aborts after 3 consecutive timeouts");

    memset(&m, 0, sizeof(m));
    m.mode_at_call = -1;
    m.default_mode = MOCK_WRONG_ID;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.acked == 0 && r.timeout == FEB_AGPS_MAX_CONSECUTIVE_TIMEOUTS, "ACK for a different class is not a match");

    memset(&m, 0, sizeof(m));
    m.mode_at_call = -1;
    m.default_mode = MOCK_BAD_CK;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.acked == 0 && r.timeout == FEB_AGPS_MAX_CONSECUTIVE_TIMEOUTS, "ACK with bad checksum is not a match");

    memset(&m, 0, sizeof(m));
    m.mode_at_call = -1;
    m.preface_nmea = 1;
    m.split_ack = 1;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.sent == 34 && r.acked == 34, "ACKs found among NMEA lines and split across reads");

    memset(&m, 0, sizeof(m));
    m.mode_at_call = 1;
    m.mode_for_call = MOCK_SILENT;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.timeout == 1 && r.acked == 33, "timeouts must be consecutive to abort");

    /* Defensive: a malformed blob handed to the feeder directly is cut short, not overrun. */
    memset(&m, 0, sizeof(m));
    m.mode_at_call = -1;
    blob[2] = 0xFC;
    blob[3] = 0xFF;
    feb_agps_feed(&io, ini, blob, len, &r);
    check(r.sent == 1 && m.write_total == FEB_AGPS_INI_LEN, "feeder stops at a corrupt frame, sends nothing past it");
}

static void test_blob(void)
{
    static feb_agps_blob_t b;
    static uint8_t data[FEB_AGPS_BLOB_MAX + 8];
    uint32_t t = 1000;
    size_t i;

    for (i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }

    feb_agps_blob_reset(&b);
    check(feb_agps_blob_chunk(&b, 600, 0, data, 256, t) == FEB_AGPS_BLOB_MORE, "chunk 0 accepted");
    check(feb_agps_blob_chunk(&b, 600, 256, data + 256, 256, t + 10) == FEB_AGPS_BLOB_MORE, "chunk 1 accepted");
    check(feb_agps_blob_chunk(&b, 600, 512, data + 512, 88, t + 20) == FEB_AGPS_BLOB_COMPLETE, "final chunk completes");
    check(b.complete && !b.active && memcmp(b.data, data, 600) == 0, "reassembled bytes match");
    check(feb_agps_blob_chunk(&b, 600, 512, data + 512, 88, t + 30) == FEB_AGPS_BLOB_ERR_COMPLETE,
          "duplicate final chunk after complete rejected");
    check(b.complete, "ERR_COMPLETE leaves completed state for the owner");
    feb_agps_blob_reset(&b);
    check(!b.complete && !b.active && b.received == 0, "reset clears state");

    /* single-chunk blob */
    check(feb_agps_blob_chunk(&b, 100, 0, data, 100, t) == FEB_AGPS_BLOB_COMPLETE, "single-chunk blob completes");
    feb_agps_blob_reset(&b);

    /* exactly 4096 in 16 chunks */
    for (i = 0; i < 16; i++) {
        feb_agps_blob_status_t s = feb_agps_blob_chunk(&b, 4096, (uint32_t)(i * 256), data + i * 256, 256, t + (uint32_t)i);

        if (i < 15) {
            check(s == FEB_AGPS_BLOB_MORE, "4096 B blob chunk accepted");
        } else {
            check(s == FEB_AGPS_BLOB_COMPLETE && memcmp(b.data, data, 4096) == 0, "4096 B blob completes intact");
        }
    }
    feb_agps_blob_reset(&b);

    /* oversize / zero total */
    check(feb_agps_blob_chunk(&b, 4097, 0, data, 100, t) == FEB_AGPS_BLOB_ERR_TOTAL, "total 4097 rejected");
    check(feb_agps_blob_chunk(&b, 0, 0, data, 100, t) == FEB_AGPS_BLOB_ERR_TOTAL, "total 0 rejected");
    check(feb_agps_blob_chunk(&b, 0xFFFFFFFFu, 0, data, 100, t) == FEB_AGPS_BLOB_ERR_TOTAL, "total 0xFFFFFFFF rejected");
    check(!b.active, "rejected oversize start leaves no state");

    /* chunk length */
    check(feb_agps_blob_chunk(&b, 1000, 0, data, 0, t) == FEB_AGPS_BLOB_ERR_LEN, "empty chunk rejected");
    check(feb_agps_blob_chunk(&b, 1000, 0, data, 257, t) == FEB_AGPS_BLOB_ERR_LEN, "257-byte chunk rejected");
    check(feb_agps_blob_chunk(&b, 1000, 0, NULL, 10, t) == FEB_AGPS_BLOB_ERR_LEN, "NULL data rejected");

    /* bad start */
    check(feb_agps_blob_chunk(&b, 1000, 256, data, 100, t) == FEB_AGPS_BLOB_ERR_ORDER, "transfer must start at off 0");
    check(!b.active, "bad start leaves no state");

    /* gap */
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, t);
    check(feb_agps_blob_chunk(&b, 1000, 512, data, 256, t) == FEB_AGPS_BLOB_ERR_ORDER, "gap (skipped chunk) rejected");
    check(!b.active && b.received == 0, "gap resets state");

    /* duplicate (retransmit of previous chunk) */
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, t);
    feb_agps_blob_chunk(&b, 1000, 256, data, 256, t);
    check(feb_agps_blob_chunk(&b, 1000, 256, data, 256, t) == FEB_AGPS_BLOB_ERR_ORDER, "duplicate chunk rejected");
    check(!b.active, "duplicate resets state");

    /* overlap */
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, t);
    check(feb_agps_blob_chunk(&b, 1000, 128, data, 256, t) == FEB_AGPS_BLOB_ERR_ORDER, "overlapping chunk rejected");
    check(!b.active, "overlap resets state");

    /* restart at 0 mid-transfer is an order error, then a clean start works */
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, t);
    check(feb_agps_blob_chunk(&b, 1000, 0, data, 256, t) == FEB_AGPS_BLOB_ERR_ORDER, "restart at 0 mid-transfer rejected");
    check(feb_agps_blob_chunk(&b, 1000, 0, data, 256, t) == FEB_AGPS_BLOB_MORE, "clean restart after reset accepted");
    feb_agps_blob_reset(&b);

    /* overrun */
    check(feb_agps_blob_chunk(&b, 200, 0, data, 256, t) == FEB_AGPS_BLOB_ERR_OVERRUN, "first chunk larger than total");
    feb_agps_blob_chunk(&b, 300, 0, data, 256, t);
    check(feb_agps_blob_chunk(&b, 300, 256, data, 100, t) == FEB_AGPS_BLOB_ERR_OVERRUN, "last chunk past total rejected");
    check(!b.active && !b.complete, "overrun resets state");
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, t);
    check(feb_agps_blob_chunk(&b, 1000, 0xFFFFFF00u, data, 256, t) == FEB_AGPS_BLOB_ERR_ORDER,
          "huge offset rejected without wrap");

    /* total change */
    feb_agps_blob_reset(&b);
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, t);
    check(feb_agps_blob_chunk(&b, 999, 256, data, 256, t) == FEB_AGPS_BLOB_ERR_MISMATCH, "total changed mid-transfer");
    check(!b.active, "mismatch resets state");

    /* timeout */
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, 1000);
    check(feb_agps_blob_chunk(&b, 1000, 256, data, 256, 1000 + FEB_AGPS_CHUNK_TIMEOUT_MS) == FEB_AGPS_BLOB_MORE,
          "chunk exactly at the timeout limit accepted");
    check(feb_agps_blob_chunk(&b, 1000, 512, data, 256, 1000 + FEB_AGPS_CHUNK_TIMEOUT_MS * 2 + 1) ==
              FEB_AGPS_BLOB_ERR_TIMEOUT,
          "stale transfer rejected");
    check(!b.active, "timeout resets state");
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, 2000);
    check(!feb_agps_blob_expire(&b, 2000 + FEB_AGPS_CHUNK_TIMEOUT_MS), "expire: not yet");
    check(feb_agps_blob_expire(&b, 2000 + FEB_AGPS_CHUNK_TIMEOUT_MS + 1) && !b.active, "expire drops stalled blob");
    check(!feb_agps_blob_expire(&b, 99999), "expire on idle is a no-op");
    /* uptime wrap-around */
    feb_agps_blob_chunk(&b, 1000, 0, data, 256, 0xFFFFFFF0u);
    check(feb_agps_blob_chunk(&b, 1000, 256, data, 256, 0x00000010u) == FEB_AGPS_BLOB_MORE, "timer wrap handled");
    feb_agps_blob_reset(&b);
}

static size_t put_text(uint8_t *out, size_t pos, const char *s)
{
    return pos + feb_cbor_encode_text(out + pos, 512 - pos, s, strlen(s));
}

static size_t put_uint(uint8_t *out, size_t pos, uint64_t v)
{
    return pos + feb_cbor_encode_uint(out + pos, 512 - pos, v);
}

static size_t build_args(uint8_t *out, int with_pos, uint64_t utc, uint64_t total, uint64_t off,
                         const uint8_t *data, size_t data_len)
{
    size_t pos = feb_cbor_encode_map_header(out, 512, with_pos ? 8 : 5);

    pos = put_text(out, pos, "action");
    pos = put_text(out, pos, "load");
    pos = put_text(out, pos, "utc");
    pos = put_uint(out, pos, utc);
    pos = put_text(out, pos, "total");
    pos = put_uint(out, pos, total);
    pos = put_text(out, pos, "off");
    pos = put_uint(out, pos, off);
    pos = put_text(out, pos, "data");
    pos += feb_cbor_encode_bytes(out + pos, 512 - pos, data, data_len);
    if (with_pos) {
        pos = put_text(out, pos, "lat_e7_offset");
        pos = put_uint(out, pos, 900000000ull + 426628000ull);
        pos = put_text(out, pos, "lon_e7_offset");
        pos = put_uint(out, pos, 1800000000ull + 232887000ull);
        pos = put_text(out, pos, "altitude_dm_offset");
        pos = put_uint(out, pos, 1000000ull + 5500ull);
    }
    return pos;
}

static void test_cmd(void)
{
    uint8_t buf[512];
    uint8_t data[200];
    feb_agps_load_args_t a;
    size_t n;

    memset(data, 0xA5, sizeof(data));

    n = build_args(buf, 0, 1791288000ull, 2650, 256, data, 200);
    check(feb_agps_decode_load_args(buf, n, &a) == FEB_CBOR_OK && a.utc == 1791288000ull && a.total == 2650 &&
              a.off == 256 && a.data_len == 200 && !a.position.valid && a.data == buf + (n - 200),
          "decode 5-field load args");
    n = build_args(buf, 1, 1791288000ull, 2650, 0, data, 200);
    check(feb_agps_decode_load_args(buf, n, &a) == FEB_CBOR_OK && a.position.valid &&
              a.position.lat_e7 == 426628000 && a.position.lon_e7 == 232887000 && a.position.alt_dm == 5500,
          "decode 8-field args with position");

    check(feb_agps_decode_load_args(buf, n - 1, &a) != FEB_CBOR_OK, "truncated args rejected");
    n = build_args(buf, 0, 1791288000ull, 2650, 0, data, 200);
    buf[n] = 0;
    check(feb_agps_decode_load_args(buf, n + 1, &a) != FEB_CBOR_OK, "trailing byte rejected");
    n = build_args(buf, 0, 1000ull, 2650, 0, data, 200);
    check(feb_agps_decode_load_args(buf, n, &a) != FEB_CBOR_OK, "utc far in the past (unset RTC) rejected");
    n = build_args(buf, 0, 5000000000ull, 2650, 0, data, 200);
    check(feb_agps_decode_load_args(buf, n, &a) != FEB_CBOR_OK, "utc far in the future rejected");
    n = build_args(buf, 0, 1791288000ull, 70000, 0, data, 200);
    check(feb_agps_decode_load_args(buf, n, &a) != FEB_CBOR_OK, "total over 16 bits rejected");
    n = build_args(buf, 0, 1791288000ull, 2650, 70000, data, 200);
    check(feb_agps_decode_load_args(buf, n, &a) != FEB_CBOR_OK, "off over 16 bits rejected");
    n = build_args(buf, 0, 1791288000ull, 2650, 0, data, 200);
    buf[1 + 7 + 5 - 1] = 'x'; /* corrupt the action text ("load" -> "loax") */
    check(feb_agps_decode_load_args(buf, n, &a) != FEB_CBOR_OK, "wrong action rejected");

    {
        /* wrong key order (off before total) */
        size_t pos = feb_cbor_encode_map_header(buf, 512, 5);

        pos = put_text(buf, pos, "action");
        pos = put_text(buf, pos, "load");
        pos = put_text(buf, pos, "utc");
        pos = put_uint(buf, pos, 1791288000ull);
        pos = put_text(buf, pos, "off");
        pos = put_uint(buf, pos, 0);
        pos = put_text(buf, pos, "total");
        pos = put_uint(buf, pos, 100);
        pos = put_text(buf, pos, "data");
        pos += feb_cbor_encode_bytes(buf + pos, 512 - pos, data, 10);
        check(feb_agps_decode_load_args(buf, pos, &a) != FEB_CBOR_OK, "out-of-order keys rejected");
    }
    {
        /* 6-entry map (partial position) */
        size_t pos = feb_cbor_encode_map_header(buf, 512, 6);

        pos = put_text(buf, pos, "action");
        pos = put_text(buf, pos, "load");
        pos = put_text(buf, pos, "utc");
        pos = put_uint(buf, pos, 1791288000ull);
        pos = put_text(buf, pos, "total");
        pos = put_uint(buf, pos, 100);
        pos = put_text(buf, pos, "off");
        pos = put_uint(buf, pos, 0);
        pos = put_text(buf, pos, "data");
        pos += feb_cbor_encode_bytes(buf + pos, 512 - pos, data, 10);
        pos = put_text(buf, pos, "lat_e7_offset");
        pos = put_uint(buf, pos, 900000000ull);
        check(feb_agps_decode_load_args(buf, pos, &a) != FEB_CBOR_OK, "partial position rejected");
    }
    {
        /* position out of range */
        size_t pos = build_args(buf, 1, 1791288000ull, 100, 0, data, 10);
        uint8_t tmp[512];

        memcpy(tmp, buf, pos);
        /* rebuild with lat offset > 1.8e9 */
        pos = feb_cbor_encode_map_header(buf, 512, 8);
        pos = put_text(buf, pos, "action");
        pos = put_text(buf, pos, "load");
        pos = put_text(buf, pos, "utc");
        pos = put_uint(buf, pos, 1791288000ull);
        pos = put_text(buf, pos, "total");
        pos = put_uint(buf, pos, 100);
        pos = put_text(buf, pos, "off");
        pos = put_uint(buf, pos, 0);
        pos = put_text(buf, pos, "data");
        pos += feb_cbor_encode_bytes(buf + pos, 512 - pos, data, 10);
        pos = put_text(buf, pos, "lat_e7_offset");
        pos = put_uint(buf, pos, 1800000001ull);
        pos = put_text(buf, pos, "lon_e7_offset");
        pos = put_uint(buf, pos, 1800000000ull);
        pos = put_text(buf, pos, "altitude_dm_offset");
        pos = put_uint(buf, pos, 1000000ull);
        check(feb_agps_decode_load_args(buf, pos, &a) != FEB_CBOR_OK, "latitude beyond 90 degrees rejected");
    }

    {
        feb_agps_result_t r = {34, 33, 1, 0};
        uint8_t out[64];
        size_t len = feb_agps_encode_result(out, sizeof(out), &r);
        static const uint8_t want[] = {0xA4, 0x64, 's', 'e', 'n', 't', 0x18, 34, 0x65, 'a', 'c', 'k', 'e', 'd', 0x18,
                                       33, 0x66, 'n', 'a', 'c', 'k', 'e', 'd', 0x01, 0x67, 't', 'i', 'm', 'e', 'o',
                                       'u', 't', 0x00};

        check(len == sizeof(want) && memcmp(out, want, len) == 0, "result map encodes canonically in field order");
        check(feb_agps_encode_result(out, 10, &r) == 0, "result encode fails cleanly on small buffer");
    }
}

int main(void)
{
    test_validator();
    test_ini();
    test_rx();
    test_feed();
    test_blob();
    test_cmd();

    if (g_failures != 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}

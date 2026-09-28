#include "cluster_link.h"

#include <string.h>

#include "crc16.h"

/* ---- Encoding ----
   Writes directly into the caller's `out` buffer (no internal staging buffer needed:
   unlike framing.c's feb_fragment_record(), nothing here needs to compute a size before
   it knows where to write, so there is no reentrancy/stack-size tradeoff to make). */

static size_t build_frame(uint8_t *out, size_t out_cap, uint8_t msg_type,
                           const uint8_t *payload, size_t payload_len)
{
    size_t frame_len;
    uint16_t crc;
    size_t pos;

    if (out == NULL || (payload == NULL && payload_len != 0)) {
        return 0;
    }
    if (payload_len > FEB_CLUSTER_MAX_PAYLOAD) {
        return 0;
    }
    frame_len = 2u + 1u + 2u + payload_len + 2u;
    if (frame_len > out_cap) {
        return 0;
    }

    pos = 0;
    out[pos++] = FEB_CLUSTER_SOF0;
    out[pos++] = FEB_CLUSTER_SOF1;
    out[pos++] = msg_type;
    out[pos++] = (uint8_t)(payload_len & 0xFFu);
    out[pos++] = (uint8_t)((payload_len >> 8) & 0xFFu);
    if (payload_len > 0) {
        memcpy(out + pos, payload, payload_len);
        pos += payload_len;
    }

    crc = feb_cluster_crc16(out + 2, 3u + payload_len);
    out[pos++] = (uint8_t)(crc & 0xFFu);
    out[pos++] = (uint8_t)((crc >> 8) & 0xFFu);

    return pos;
}

size_t feb_cluster_encode_worker_hello(uint8_t *out, size_t out_cap,
                                        const feb_cluster_worker_hello_t *msg)
{
    uint8_t payload[1];

    if (msg == NULL) {
        return 0;
    }
    payload[0] = msg->band;
    return build_frame(out, out_cap, (uint8_t)FEB_CLUSTER_MSG_WORKER_HELLO, payload, sizeof(payload));
}

size_t feb_cluster_encode_scan_config_set(uint8_t *out, size_t out_cap,
                                           const feb_cluster_scan_config_t *msg)
{
    uint8_t payload[5];

    if (msg == NULL) {
        return 0;
    }
    payload[0] = msg->mode;
    payload[1] = msg->dwell_mode;
    payload[2] = msg->band_filter;
    payload[3] = (uint8_t)(msg->interval_ms & 0xFFu);
    payload[4] = (uint8_t)((msg->interval_ms >> 8) & 0xFFu);
    return build_frame(out, out_cap, (uint8_t)FEB_CLUSTER_MSG_SCAN_CONFIG_SET, payload, sizeof(payload));
}

size_t feb_cluster_encode_scan_result(uint8_t *out, size_t out_cap,
                                       const feb_cluster_scan_result_t *msg)
{
    uint8_t payload[1 + FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN + FEB_CLUSTER_SCAN_RESULT_BSSID_LEN + 1 + 1 + 1 + 1];
    size_t pos;

    if (msg == NULL || msg->ssid_len > FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN) {
        return 0;
    }

    pos = 0;
    payload[pos++] = msg->ssid_len;
    memcpy(payload + pos, msg->ssid, msg->ssid_len);
    pos += msg->ssid_len;
    memcpy(payload + pos, msg->bssid, FEB_CLUSTER_SCAN_RESULT_BSSID_LEN);
    pos += FEB_CLUSTER_SCAN_RESULT_BSSID_LEN;
    payload[pos++] = (uint8_t)msg->rssi;
    payload[pos++] = msg->channel;
    payload[pos++] = msg->phy;
    payload[pos++] = msg->auth;

    return build_frame(out, out_cap, (uint8_t)FEB_CLUSTER_MSG_SCAN_RESULT, payload, pos);
}

size_t feb_cluster_encode_scan_batch_done(uint8_t *out, size_t out_cap,
                                           const feb_cluster_scan_batch_done_t *msg)
{
    uint8_t payload[2];

    if (msg == NULL) {
        return 0;
    }
    payload[0] = (uint8_t)(msg->count & 0xFFu);
    payload[1] = (uint8_t)((msg->count >> 8) & 0xFFu);
    return build_frame(out, out_cap, (uint8_t)FEB_CLUSTER_MSG_SCAN_BATCH_DONE, payload, sizeof(payload));
}

/* ---- Streaming decoder ----
   dec->buf[start..len) holds the not-yet-consumed bytes. process() either discards them up
   to the next SOF0, waits on an incomplete frame, or validates a complete one. A rejected
   candidate drops only its SOF0 byte, so every byte after it is re-scanned. Invariant after
   each process() that is not FRAME_READY: the pending bytes are empty or an incomplete
   frame prefix (< FEB_CLUSTER_MAX_FRAME_SIZE bytes); after FRAME_READY at most
   FEB_CLUSTER_MAX_FRAME_SIZE - 7 bytes remain. Either way one more byte always fits once
   buf is compacted. */

void feb_cluster_decoder_init(feb_cluster_decoder_t *dec)
{
    if (dec == NULL) {
        return;
    }
    dec->start = 0;
    dec->len = 0;
}

static feb_cluster_decode_result_t decoder_process(feb_cluster_decoder_t *dec,
                                                   feb_cluster_frame_t *out_frame)
{
    int rejected = 0;

    for (;;) {
        const uint8_t *p = dec->buf + dec->start;
        size_t avail = (size_t)dec->len - dec->start;
        size_t payload_len;
        uint16_t received_crc;

        if (avail == 0) {
            dec->start = 0;
            dec->len = 0;
            break;
        }
        if (p[0] != FEB_CLUSTER_SOF0) {
            dec->start++;
            continue;
        }
        if (avail < 2) {
            break;
        }
        if (p[1] != FEB_CLUSTER_SOF1) {
            dec->start++;
            continue;
        }
        if (avail < 5) {
            break;
        }
        payload_len = (size_t)p[3] | ((size_t)p[4] << 8);
        if (payload_len > FEB_CLUSTER_MAX_PAYLOAD) {
            dec->start++;
            rejected = 1;
            continue;
        }
        if (avail < 7u + payload_len) {
            break;
        }
        received_crc = (uint16_t)(p[5 + payload_len] | ((uint16_t)p[6 + payload_len] << 8));
        if (received_crc != feb_cluster_crc16(p + 2, 3u + payload_len)) {
            dec->start++;
            rejected = 1;
            continue;
        }
        if (out_frame != NULL) {
            out_frame->msg_type = p[2];
            out_frame->payload_len = (uint16_t)payload_len;
            if (payload_len > 0) {
                memcpy(out_frame->payload, p + 5, payload_len);
            }
        }
        dec->start = (uint16_t)(dec->start + 7u + payload_len);
        if (dec->start == dec->len) {
            dec->start = 0;
            dec->len = 0;
        }
        return FEB_CLUSTER_DECODE_FRAME_READY;
    }
    return rejected ? FEB_CLUSTER_DECODE_RESYNC : FEB_CLUSTER_DECODE_NEED_MORE;
}

feb_cluster_decode_result_t feb_cluster_decoder_feed_byte(feb_cluster_decoder_t *dec,
                                                           uint8_t byte,
                                                           feb_cluster_frame_t *out_frame)
{
    if (dec == NULL) {
        return FEB_CLUSTER_DECODE_NEED_MORE;
    }
    if (dec->len >= sizeof(dec->buf)) {
        if (dec->start == 0) {
            dec->start = 1;
        }
        memmove(dec->buf, dec->buf + dec->start, (size_t)dec->len - dec->start);
        dec->len = (uint16_t)(dec->len - dec->start);
        dec->start = 0;
    }
    dec->buf[dec->len++] = byte;
    return decoder_process(dec, out_frame);
}

feb_cluster_decode_result_t feb_cluster_decoder_poll(feb_cluster_decoder_t *dec,
                                                      feb_cluster_frame_t *out_frame)
{
    if (dec == NULL) {
        return FEB_CLUSTER_DECODE_NEED_MORE;
    }
    return decoder_process(dec, out_frame);
}

size_t feb_cluster_decoder_feed(feb_cluster_decoder_t *dec, const uint8_t *data, size_t len,
                                 feb_cluster_frame_t *frames, size_t max_frames)
{
    size_t produced = 0;
    size_t i;

    if (dec == NULL || data == NULL) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        feb_cluster_frame_t *slot = (produced < max_frames) ? &frames[produced] : NULL;
        feb_cluster_decode_result_t result = feb_cluster_decoder_feed_byte(dec, data[i], slot);

        while (result == FEB_CLUSTER_DECODE_FRAME_READY) {
            if (slot != NULL) {
                produced++;
            }
            slot = (produced < max_frames) ? &frames[produced] : NULL;
            result = feb_cluster_decoder_poll(dec, slot);
        }
    }
    return produced;
}

/* ---- Per-message decode helpers ---- */

int feb_cluster_decode_worker_hello(const feb_cluster_frame_t *frame,
                                     feb_cluster_worker_hello_t *out)
{
    if (frame == NULL || out == NULL) {
        return 0;
    }
    if (frame->msg_type != (uint8_t)FEB_CLUSTER_MSG_WORKER_HELLO || frame->payload_len != 1u) {
        return 0;
    }
    out->band = frame->payload[0];
    return 1;
}

int feb_cluster_decode_scan_config_set(const feb_cluster_frame_t *frame,
                                        feb_cluster_scan_config_t *out)
{
    if (frame == NULL || out == NULL) {
        return 0;
    }
    if (frame->msg_type != (uint8_t)FEB_CLUSTER_MSG_SCAN_CONFIG_SET || frame->payload_len != 5u) {
        return 0;
    }
    out->mode = frame->payload[0];
    out->dwell_mode = frame->payload[1];
    out->band_filter = frame->payload[2];
    out->interval_ms = (uint16_t)(frame->payload[3] | ((uint16_t)frame->payload[4] << 8));
    return 1;
}

int feb_cluster_decode_scan_result(const feb_cluster_frame_t *frame,
                                    feb_cluster_scan_result_t *out)
{
    size_t pos;
    uint8_t ssid_len;

    if (frame == NULL || out == NULL) {
        return 0;
    }
    if (frame->msg_type != (uint8_t)FEB_CLUSTER_MSG_SCAN_RESULT) {
        return 0;
    }
    if (frame->payload_len < 1u) {
        return 0;
    }
    ssid_len = frame->payload[0];
    if (ssid_len > FEB_CLUSTER_SCAN_RESULT_SSID_MAX_LEN) {
        return 0;
    }
    /* ssid_len bytes of ssid + 6 bssid + 1 rssi + 1 channel + 1 phy + 1 auth */
    if (frame->payload_len != (size_t)1u + ssid_len + FEB_CLUSTER_SCAN_RESULT_BSSID_LEN + 1u + 1u + 1u + 1u) {
        return 0;
    }

    pos = 1;
    memset(out->ssid, 0, sizeof(out->ssid));
    memcpy(out->ssid, frame->payload + pos, ssid_len);
    out->ssid_len = ssid_len;
    pos += ssid_len;
    memcpy(out->bssid, frame->payload + pos, FEB_CLUSTER_SCAN_RESULT_BSSID_LEN);
    pos += FEB_CLUSTER_SCAN_RESULT_BSSID_LEN;
    out->rssi = (int8_t)frame->payload[pos++];
    out->channel = frame->payload[pos++];
    out->phy = frame->payload[pos++];
    out->auth = frame->payload[pos++];
    return 1;
}

int feb_cluster_decode_scan_batch_done(const feb_cluster_frame_t *frame,
                                        feb_cluster_scan_batch_done_t *out)
{
    if (frame == NULL || out == NULL) {
        return 0;
    }
    if (frame->msg_type != (uint8_t)FEB_CLUSTER_MSG_SCAN_BATCH_DONE || frame->payload_len != 2u) {
        return 0;
    }
    out->count = (uint16_t)(frame->payload[0] | ((uint16_t)frame->payload[1] << 8));
    return 1;
}

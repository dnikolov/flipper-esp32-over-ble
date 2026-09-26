/* CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, xorout 0x0000). Used by
   cluster_link.c to cover [msg_type, payload_len, payload] per docs/CLUSTER.md's
   "Inter-board protocol" frame layout. Hand-rolled, no third-party library, matching this
   project's convention for wire-format code (CLAUDE.md's conventions). Verified against
   the standard CRC16/CCITT-FALSE check value for ASCII "123456789" (0x29B1) in
   tests/esp32/test_cluster_link.c. */
#ifndef FEB_CLUSTER_CRC16_H
#define FEB_CLUSTER_CRC16_H

#include <stddef.h>
#include <stdint.h>

#define FEB_CLUSTER_CRC16_INIT 0xFFFFu

/* Folds one more byte into a running CRC. Callers streaming a frame byte-at-a-time
   (cluster_link.c's decoder) start from FEB_CLUSTER_CRC16_INIT and call this once per
   covered byte. */
uint16_t feb_cluster_crc16_update(uint16_t crc, uint8_t byte);

/* Convenience one-shot form over a contiguous buffer; equivalent to seeding
   FEB_CLUSTER_CRC16_INIT and calling feb_cluster_crc16_update() for each byte in order. */
uint16_t feb_cluster_crc16(const uint8_t *data, size_t len);

#endif /* FEB_CLUSTER_CRC16_H */

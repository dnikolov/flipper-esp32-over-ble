#include "crc16.h"

uint16_t feb_cluster_crc16_update(uint16_t crc, uint8_t byte)
{
    int bit;

    crc = (uint16_t)(crc ^ ((uint16_t)byte << 8));
    for (bit = 0; bit < 8; bit++) {
        if ((crc & 0x8000u) != 0u) {
            crc = (uint16_t)((crc << 1) ^ 0x1021u);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t feb_cluster_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = FEB_CLUSTER_CRC16_INIT;
    size_t i;

    for (i = 0; i < len; i++) {
        crc = feb_cluster_crc16_update(crc, data[i]);
    }
    return crc;
}

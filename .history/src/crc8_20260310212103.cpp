#include "crc8.h"

uint8_t crc8_dvb_s2(const uint8_t *data, uint32_t length) {
    uint8_t crc = 0;
    const uint8_t poly = 0xD5;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t a = data[i];
        crc ^= a;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ poly);
            else crc <<= 1;
        }
    }
    return crc;
}

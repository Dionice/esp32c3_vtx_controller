#pragma once
#include <stdint.h>

// CRC8 DVB-S2 (poly 0xD5)
uint8_t crc8_dvb_s2(const uint8_t *data, uint32_t length);

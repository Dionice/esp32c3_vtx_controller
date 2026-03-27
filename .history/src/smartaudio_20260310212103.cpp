#include <Arduino.h>
#include "smartaudio.h"
#include "crc8.h"

// SmartAudio framing: 0xAA 0x55 <len> <cmd> <payload...> <crc8>
// CRC8 is computed over: <len> <cmd> <payload...>

static void smartaudioSendFrame(uint8_t command, const uint8_t *payload, uint8_t payloadLen) {
    uint8_t header[2] = { 0xAA, 0x55 };
    Serial1.write(header, 2);

    uint8_t len = payloadLen;
    Serial1.write(len);
    Serial1.write(command);

    if (payloadLen) Serial1.write(payload, payloadLen);

    // compute CRC8 over len, command and payload
    uint8_t crcBuf[2 + 32];
    uint8_t crcCount = 0;
    crcBuf[crcCount++] = len;
    crcBuf[crcCount++] = command;
    for (uint8_t i = 0; i < payloadLen && crcCount < sizeof(crcBuf); i++) crcBuf[crcCount++] = payload[i];
    uint8_t crc = crc8_dvb_s2(crcBuf, crcCount);
    Serial1.write(crc);
}

void smartaudioSendBandChannel(uint8_t band, uint8_t channel) {
    uint8_t payload[2] = { band, channel };
    // SMARTAUDIO_CMD_SET_CHANNEL is 0x07 per Betaflight
    smartaudioSendFrame(0x07, payload, 2);
}

void smartaudioSendPower(uint8_t power) {
    uint8_t payload[1] = { power };
    // SMARTAUDIO_CMD_SET_POWER is 0x05
    smartaudioSendFrame(0x05, payload, 1);
}

void smartaudioSendFrequency(uint16_t frequency, bool pitmodeFrequency) {
    uint16_t val = frequency & 0x3FFF;
    if (pitmodeFrequency) val |= (1 << 15);
    uint8_t payload[2] = { (uint8_t)((val >> 8) & 0xFF), (uint8_t)(val & 0xFF) };
    // SMARTAUDIO_CMD_SET_FREQUENCY is 0x09
    smartaudioSendFrame(0x09, payload, 2);
}


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

// Parse a SmartAudio response buffer (simple parser matching Get Settings response)
static bool smartaudioParseSettingsResponse(const uint8_t *buf, size_t len, uint8_t *channel, uint8_t *power, uint8_t *opmode, uint16_t *frequency) {
    if (len < 2 + 1 + 5 + 1) return false; // header(2) + len + payload(5) + crc
    // find start code 0xAA 0x55
    size_t i = 0;
    for (; i + 4 < len; i++) {
        if (buf[i] == 0xAA && buf[i+1] == 0x55) break;
    }
    if (i + 4 >= len) return false;
    const uint8_t plen = buf[i+2];
    const uint8_t cmd = buf[i+3];
    if (plen < 5) return false;
    if (i + 3 + plen + 1 >= len) return false;
    const uint8_t *payload = &buf[i+4];
    const uint8_t crc = buf[i+4 + plen];

    // compute crc over len, cmd and payload
    uint8_t crcBuf[2 + 32];
    uint8_t crcCount = 0;
    crcBuf[crcCount++] = plen;
    crcBuf[crcCount++] = cmd;
    for (uint8_t j = 0; j < plen && crcCount < sizeof(crcBuf); j++) crcBuf[crcCount++] = payload[j];
    uint8_t calc = crc8_dvb_s2(crcBuf, crcCount);
    if (calc != crc) return false;

    // payload layout expected: channel (1), power (1), operationMode (1), frequency (2)
    *channel = payload[0];
    *power = payload[1];
    *opmode = payload[2];
    *frequency = (uint16_t)payload[3] << 8 | payload[4];
    return true;
}

bool smartaudioProbe(smartaudio_probe_result_t *result, uint32_t timeout_ms) {
    if (!result) return false;
    // send Get Settings command (0x03)
    smartaudioSendFrame(0x03, NULL, 0);

    uint32_t start = millis();
    uint8_t buf[64];
    size_t pos = 0;
    while (millis() - start < timeout_ms) {
        while (Serial1.available() && pos < sizeof(buf)) {
            buf[pos++] = (uint8_t)Serial1.read();
        }
        if (pos >= 8) {
            uint8_t channel, power, opmode; uint16_t freq;
            if (smartaudioParseSettingsResponse(buf, pos, &channel, &power, &opmode, &freq)) {
                result->channel = channel;
                result->power = power;
                result->operationMode = opmode;
                result->frequency = freq;
                return true;
            }
        }
    }
    return false;
}

// --- State machine implementation ---

smartaudio_device_state_t sa_state = {0,0,0,0,false};

static uint32_t sa_lastTransmissionMs = 0;
static uint8_t sa_outstanding = 0; // outstanding command code
static uint8_t sa_osbuf[32];
static int sa_oslen = 0;

void smartaudioRequestGetSettings(void) {
    smartaudioSendFrame(0x03, NULL, 0);
    sa_outstanding = 0x03;
    sa_lastTransmissionMs = millis();
}

void smartaudioRequestSetChannel(uint8_t band, uint8_t channel) {
    uint8_t payload[2] = { band, channel };
    smartaudioSendFrame(0x07, payload, 2);
    sa_outstanding = 0x07;
    sa_lastTransmissionMs = millis();
}

void smartaudioRequestSetPower(uint8_t power) {
    uint8_t payload[1] = { power };
    smartaudioSendFrame(0x05, payload, 1);
    sa_outstanding = 0x05;
    sa_lastTransmissionMs = millis();
}

void smartaudioRequestSetFrequency(uint16_t frequency, bool pitmodeFrequency) {
    uint16_t val = frequency & 0x3FFF;
    if (pitmodeFrequency) val |= (1 << 15);
    uint8_t payload[2] = { (uint8_t)((val >> 8) & 0xFF), (uint8_t)(val & 0xFF) };
    smartaudioSendFrame(0x09, payload, 2);
    sa_outstanding = 0x09;
    sa_lastTransmissionMs = millis();
}

// Incoming byte buffer
static uint8_t sa_rbuf[64];
static size_t sa_rpos = 0;

static void sa_handle_incoming(void) {
    while (Serial1.available() && sa_rpos < sizeof(sa_rbuf)) {
        sa_rbuf[sa_rpos++] = (uint8_t)Serial1.read();
    }
    if (sa_rpos >= 6) {
        // try parse; support v1/v2 get settings responses and simpler ACKs
        uint8_t channel, power, opmode; uint16_t freq;
        if (smartaudioParseSettingsResponse(sa_rbuf, sa_rpos, &channel, &power, &opmode, &freq)) {
            sa_state.channel = channel;
            sa_state.power = power;
            sa_state.opMode = opmode;
            sa_state.frequency = freq;
            sa_state.valid = true;
            sa_outstanding = 0;
            // clear buffer
            sa_rpos = 0;
        } else {
            // not parsed yet; if buffer full or no valid header, shift/drop
            if (sa_rpos == sizeof(sa_rbuf)) sa_rpos = 0;
        }
    }
}

void smartaudioProcess(void) {
    sa_handle_incoming();
    // timeout handling for outstanding commands
    if (sa_outstanding && millis() - sa_lastTransmissionMs > 200) {
        // retransmit once
        if (sa_oslen > 0) {
            Serial1.write(sa_osbuf, sa_oslen);
            sa_lastTransmissionMs = millis();
            // clear stored outstanding to avoid infinite retransmit here
            sa_outstanding = 0;
            sa_oslen = 0;
        } else {
            sa_outstanding = 0; // give up
        }
    }
}


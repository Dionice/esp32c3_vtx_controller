#include <Arduino.h>
#include <string.h>
#include "tramp.h"

// TRAMP frame layout matches Betaflight tramp_protocol: header, payload, footer
// We'll build a 16-byte frame (as in Betaflight TRAMP_FRAME_LENGTH == 16)

typedef struct __attribute__((packed)) {
    uint8_t syncStart;
    uint8_t command;
    uint8_t payload[12];
    uint8_t crc;
    uint8_t syncStop;
} trampFrame_t;

static uint8_t trampCrc(const trampFrame_t *frame) {
    const uint8_t *p = (const uint8_t *)frame;
    uint8_t crc = 0;
    // sum header + payload (exclude footer.crc and footer.syncStop)
    for (size_t i = 0; i < sizeof(trampFrame_t) - 2; i++) crc += p[i];
    return crc;
}

static void trampSendFrame(uint8_t cmd, const uint8_t *payload, size_t payloadLen) {
    trampFrame_t frame;
    frame.syncStart = 0x0F;
    frame.command = cmd;
    memset(frame.payload, 0, sizeof(frame.payload));
    if (payload && payloadLen) memcpy(frame.payload, payload, payloadLen > sizeof(frame.payload) ? sizeof(frame.payload) : payloadLen);
    frame.crc = trampCrc(&frame);
    frame.syncStop = 0x00;
    Serial1.write((const uint8_t *)&frame, sizeof(frame));
}

void trampSendFrequency(uint16_t freq) {
    uint8_t payload[2] = { (uint8_t)(freq & 0xFF), (uint8_t)((freq >> 8) & 0xFF) };
    trampSendFrame('F', payload, 2);
}

void trampSendPower(uint16_t power) {
    uint8_t payload[2] = { (uint8_t)(power & 0xFF), (uint8_t)((power >> 8) & 0xFF) };
    trampSendFrame('P', payload, 2);
}

void trampSendActiveState(bool active) {
    uint8_t payload[1] = { (uint8_t)active };
    trampSendFrame('I', payload, 1);
}


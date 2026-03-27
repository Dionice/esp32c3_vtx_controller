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

// --- TRAMP receive/parse/state ---
tramp_device_state_t tramp_state = {0,0,false};

// Simple parser: read from serial and look for full 16-byte frame
static uint8_t tramp_rbuf[32];
static size_t tramp_rpos = 0;

// TRAMP queue
typedef struct {
    uint8_t buf[16];
    uint8_t len;
    uint8_t cmd;
    uint8_t retries;
    uint32_t lastSendMs;
} tramp_cmd_t;

static tramp_cmd_t tramp_queue[TRAMP_QUEUE_DEPTH];
static int tramp_q_head = 0;
static int tramp_q_tail = 0;
static bool tramp_debug = false;

static bool tramp_queue_empty(void) { return tramp_q_head == tramp_q_tail; }
static bool tramp_queue_full(void) { return ((tramp_q_tail + 1) % TRAMP_QUEUE_DEPTH) == tramp_q_head; }

static bool tramp_enqueue(const uint8_t *buf, uint8_t len, uint8_t cmd) {
    if (tramp_queue_full()) return false;
    tramp_cmd_t *e = &tramp_queue[tramp_q_tail];
    memset(e,0,sizeof(*e));
    memcpy(e->buf, buf, len);
    e->len = len; e->cmd = cmd; e->retries = 0; e->lastSendMs = 0;
    tramp_q_tail = (tramp_q_tail + 1) % TRAMP_QUEUE_DEPTH;
    return true;
}

bool trampEnqueueGetSettings(void) {
    // enqueue an empty payload with command 'v' (get config)
    uint8_t payload[1] = {0};
    return tramp_enqueue(payload, 1, 'v');
}

bool trampEnqueueSetFrequency(uint16_t freq) {
    uint8_t payload[2] = { (uint8_t)(freq & 0xFF), (uint8_t)((freq>>8)&0xFF) };
    return tramp_enqueue(payload, 2, 'F');
}

bool trampEnqueueSetPower(uint16_t power) {
    uint8_t payload[2] = { (uint8_t)(power & 0xFF), (uint8_t)((power>>8)&0xFF) };
    return tramp_enqueue(payload, 2, 'P');
}

void trampEnableDebug(bool enable) { tramp_debug = enable; }

static void tramp_send_head(void) {
    if (tramp_queue_empty()) return;
    tramp_cmd_t *e = &tramp_queue[tramp_q_head];
    // build frame and send
    trampSendFrame(e->cmd, e->buf, e->len);
    e->retries++;
    e->lastSendMs = millis();
}

void trampProcess(void) {
    tramp_handle_incoming();
    // send next if any
    if (!tramp_queue_empty()) {
        tramp_cmd_t *e = &tramp_queue[tramp_q_head];
        if (e->lastSendMs == 0) {
            if (tramp_debug) Serial.println("TRAMP: sending queued");
            tramp_send_head();
        } else if (millis() - e->lastSendMs > 500) {
            if (e->retries >= TRAMP_MAX_RETRIES) {
                // drop
                tramp_q_head = (tramp_q_head + 1) % TRAMP_QUEUE_DEPTH;
            } else {
                tramp_send_head();
            }
        }
    }
}

static void tramp_handle_incoming(void) {
    while (Serial1.available() && tramp_rpos < sizeof(tramp_rbuf)) {
        tramp_rbuf[tramp_rpos++] = (uint8_t)Serial1.read();
    }
    // search for valid frame
    for (size_t i = 0; i + sizeof(trampFrame_t) <= tramp_rpos; i++) {
        trampFrame_t *frame = (trampFrame_t *)&tramp_rbuf[i];
        // check sync start/stop
        if (frame->syncStart != 0x0F || frame->syncStop != 0x00) continue;
        uint8_t crc = trampCrc(frame);
        if (crc != frame->crc) continue;
        // parse frequency and power if response contains settings
        // assume settings payload at start of payload buffer
        uint16_t freq = (uint16_t)frame->payload[0] | ((uint16_t)frame->payload[1] << 8);
        uint16_t power = (uint16_t)frame->payload[2] | ((uint16_t)frame->payload[3] << 8);
        tramp_state.frequency = freq;
        tramp_state.power = power;
        tramp_state.valid = true;
        // compact buffer
        size_t remain = tramp_rpos - (i + sizeof(trampFrame_t));
        if (remain) memmove(tramp_rbuf, &tramp_rbuf[i + sizeof(trampFrame_t)], remain);
        tramp_rpos = remain;
        return;
    }
    // drop if buffer nearly full
    if (tramp_rpos == sizeof(tramp_rbuf)) tramp_rpos = 0;
}

// trampProcess defined above


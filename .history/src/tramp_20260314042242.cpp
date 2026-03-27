#include <Arduino.h>
#include <string.h>

#include "tramp.h"

namespace {

constexpr uint8_t kTrampSyncStart = 0x0F;
constexpr uint8_t kTrampSyncStop = 0x00;
constexpr uint8_t kTrampPayloadLength = 12;
constexpr uint8_t kTrampFrameLength = 16;
constexpr uint8_t kTrampCommandSetFrequency = 'F';
constexpr uint8_t kTrampCommandSetPower = 'P';
constexpr uint8_t kTrampCommandSetActiveState = 'I';
constexpr uint8_t kTrampCommandGetSettings = 'v';
constexpr uint8_t kTrampCommandGetLimits = 'r';
constexpr uint8_t kTrampCommandGetTemperature = 's';
constexpr uint32_t kTrampMinRequestPeriodMs = 200;

typedef struct __attribute__((packed)) {
    uint8_t syncStart;
    uint8_t command;
    uint8_t payload[kTrampPayloadLength];
    uint8_t crc;
    uint8_t syncStop;
} trampFrame_t;

typedef struct {
    uint8_t payload[kTrampPayloadLength];
    uint8_t payloadLength;
    uint8_t command;
    uint8_t attempts;
    uint32_t lastSendMs;
} tramp_cmd_t;

static_assert(sizeof(trampFrame_t) == kTrampFrameLength, "Unexpected TRAMP frame size");

static tramp_cmd_t tramp_queue[TRAMP_QUEUE_DEPTH];
static int tramp_q_head = 0;
static int tramp_q_tail = 0;
static bool tramp_debug = false;
static uint8_t tramp_rbuf[32];
static size_t tramp_rpos = 0;
static bool tramp_initialized = false;

static bool tramp_queue_empty(void) {
    return tramp_q_head == tramp_q_tail;
}

static bool tramp_queue_full(void) {
    return ((tramp_q_tail + 1) % TRAMP_QUEUE_DEPTH) == tramp_q_head;
}

static bool trampCommandExpectsResponse(uint8_t command) {
    return command == kTrampCommandGetSettings || command == kTrampCommandGetLimits || command == kTrampCommandGetTemperature;
}

static uint8_t trampCrc(const trampFrame_t *frame) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(frame);
    uint8_t crc = 0;
    for (size_t index = 1; index < kTrampFrameLength - 2; index++) {
        crc += bytes[index];
    }
    return crc;
}

static void trampBuildFrame(uint8_t command, const uint8_t *payload, size_t payloadLength, trampFrame_t *frame) {
    memset(frame, 0, sizeof(*frame));
    frame->syncStart = kTrampSyncStart;
    frame->command = command;
    if (payload && payloadLength > 0) {
        memcpy(frame->payload,
               payload,
               payloadLength > sizeof(frame->payload) ? sizeof(frame->payload) : payloadLength);
    }
    frame->crc = trampCrc(frame);
    frame->syncStop = kTrampSyncStop;
}

static void trampSendFrame(uint8_t command, const uint8_t *payload, size_t payloadLength) {
    trampFrame_t frame;
    trampBuildFrame(command, payload, payloadLength, &frame);
    Serial1.write(reinterpret_cast<const uint8_t *>(&frame), sizeof(frame));
    Serial1.flush();

    if (tramp_debug) {
        Serial.printf("TRAMP tx cmd=%c crc=%02X\n", command, frame.crc);
    }
}

static void trampPopQueueHead(void) {
    if (!tramp_queue_empty()) {
        tramp_q_head = (tramp_q_head + 1) % TRAMP_QUEUE_DEPTH;
    }
}

static bool tramp_enqueue(const uint8_t *payload, uint8_t payloadLength, uint8_t command) {
    if (tramp_queue_full()) {
        return false;
    }

    tramp_cmd_t *entry = &tramp_queue[tramp_q_tail];
    memset(entry, 0, sizeof(*entry));
    if (payload && payloadLength > 0) {
        memcpy(entry->payload,
               payload,
               payloadLength > sizeof(entry->payload) ? sizeof(entry->payload) : payloadLength);
    }
    entry->payloadLength = payloadLength;
    entry->command = command;
    tramp_q_tail = (tramp_q_tail + 1) % TRAMP_QUEUE_DEPTH;
    return true;
}

static void trampHandleFrame(const trampFrame_t *frame) {
    switch (frame->command) {
        case kTrampCommandGetLimits:
            tramp_initialized = true;
            break;
        case kTrampCommandGetSettings:
            tramp_state.frequency = static_cast<uint16_t>(frame->payload[0]) |
                                    (static_cast<uint16_t>(frame->payload[1]) << 8);
            tramp_state.power = static_cast<uint16_t>(frame->payload[2]) |
                                (static_cast<uint16_t>(frame->payload[3]) << 8);
            tramp_state.valid = tramp_state.frequency != 0;
            break;
        default:
            break;
    }

    if (!tramp_queue_empty()) {
        tramp_cmd_t *entry = &tramp_queue[tramp_q_head];
        if (trampCommandExpectsResponse(entry->command) && entry->command == frame->command) {
            trampPopQueueHead();
        }
    }
}

static void tramp_handle_incoming(void) {
    while (Serial1.available() && tramp_rpos < sizeof(tramp_rbuf)) {
        tramp_rbuf[tramp_rpos++] = static_cast<uint8_t>(Serial1.read());
    }

    for (size_t index = 0; index + sizeof(trampFrame_t) <= tramp_rpos; index++) {
        const trampFrame_t *frame = reinterpret_cast<const trampFrame_t *>(&tramp_rbuf[index]);
        if (frame->syncStart != kTrampSyncStart || frame->syncStop != kTrampSyncStop) {
            continue;
        }
        if (trampCrc(frame) != frame->crc) {
            continue;
        }

        trampHandleFrame(frame);

        const size_t remain = tramp_rpos - (index + sizeof(trampFrame_t));
        if (remain > 0) {
            memmove(tramp_rbuf, &tramp_rbuf[index + sizeof(trampFrame_t)], remain);
        }
        tramp_rpos = remain;
        return;
    }

    if (tramp_rpos == sizeof(tramp_rbuf)) {
        tramp_rpos = 0;
    }
}

}  // namespace

tramp_device_state_t tramp_state = {0, 0, false};

void trampSendFrequency(uint16_t freq) {
    const uint8_t payload[2] = {
        static_cast<uint8_t>(freq & 0xFF),
        static_cast<uint8_t>((freq >> 8) & 0xFF)
    };
    trampSendFrame(kTrampCommandSetFrequency, payload, sizeof(payload));
}

void trampSendPower(uint16_t power) {
    const uint8_t payload[2] = {
        static_cast<uint8_t>(power & 0xFF),
        static_cast<uint8_t>((power >> 8) & 0xFF)
    };
    trampSendFrame(kTrampCommandSetPower, payload, sizeof(payload));
}

void trampSendActiveState(bool active) {
    const uint8_t payload[1] = { static_cast<uint8_t>(active ? 1 : 0) };
    trampSendFrame(kTrampCommandSetActiveState, payload, sizeof(payload));
}

bool trampEnqueueGetSettings(void) {
    return tramp_enqueue(nullptr, 0, kTrampCommandGetSettings);
}

bool trampEnqueueInit(void) {
    return tramp_enqueue(nullptr, 0, kTrampCommandGetLimits);
}

bool trampEnqueueSetFrequency(uint16_t freq) {
    const uint8_t payload[2] = {
        static_cast<uint8_t>(freq & 0xFF),
        static_cast<uint8_t>((freq >> 8) & 0xFF)
    };
    return tramp_enqueue(payload, sizeof(payload), kTrampCommandSetFrequency);
}

bool trampEnqueueSetPower(uint16_t power) {
    const uint8_t payload[2] = {
        static_cast<uint8_t>(power & 0xFF),
        static_cast<uint8_t>((power >> 8) & 0xFF)
    };
    return tramp_enqueue(payload, sizeof(payload), kTrampCommandSetPower);
}

bool trampEnqueueSetActiveState(bool active) {
    const uint8_t payload[1] = { static_cast<uint8_t>(active ? 1 : 0) };
    return tramp_enqueue(payload, sizeof(payload), kTrampCommandSetActiveState);
}

bool trampIsBusy(void) {
    return !tramp_queue_empty();
}

void trampEnableDebug(bool enable) {
    tramp_debug = enable;
}

void trampResetState(void) {
    tramp_q_head = 0;
    tramp_q_tail = 0;
    tramp_rpos = 0;
    tramp_initialized = false;
    tramp_state.valid = false;
}

bool trampInit(void) {
    if (tramp_initialized) {
        return true;
    }

    trampSendFrame(kTrampCommandGetLimits, nullptr, 0);

    const uint32_t startedAt = millis();
    while (millis() - startedAt < 600) {
        tramp_handle_incoming();
        if (tramp_initialized) {
            return true;
        }
        delay(10);
    }

    return false;
}

void trampProcess(void) {
    tramp_handle_incoming();

    if (tramp_queue_empty()) {
        return;
    }

    tramp_cmd_t *entry = &tramp_queue[tramp_q_head];
    const uint32_t now = millis();

    if (entry->command != kTrampCommandGetLimits && !tramp_initialized) {
        if (entry->attempts == 0) {
            trampSendFrame(kTrampCommandGetLimits, nullptr, 0);
            entry->attempts = 1;
            entry->lastSendMs = now;
        } else if (now - entry->lastSendMs >= kTrampMinRequestPeriodMs) {
            if (entry->attempts >= TRAMP_MAX_RETRIES) {
                trampPopQueueHead();
            } else {
                trampSendFrame(kTrampCommandGetLimits, nullptr, 0);
                entry->attempts++;
                entry->lastSendMs = now;
            }
        }
        return;
    }

    if (entry->attempts == 0) {
        trampSendFrame(entry->command, entry->payload, entry->payloadLength);
        entry->attempts = 1;
        entry->lastSendMs = now;
        return;
    }

    if (!trampCommandExpectsResponse(entry->command)) {
        if (now - entry->lastSendMs >= kTrampMinRequestPeriodMs) {
            trampPopQueueHead();
        }
        return;
    }

    if (now - entry->lastSendMs < kTrampMinRequestPeriodMs) {
        return;
    }

    if (entry->attempts >= TRAMP_MAX_RETRIES) {
        trampPopQueueHead();
        return;
    }

    trampSendFrame(entry->command, entry->payload, entry->payloadLength);
    entry->attempts++;
    entry->lastSendMs = now;
}


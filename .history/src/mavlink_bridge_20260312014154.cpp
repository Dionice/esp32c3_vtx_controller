#include "mavlink_bridge.h"

#include <math.h>
#include <string.h>

namespace {

static const uint8_t kMavlinkV1Magic = 0xFE;
static const uint8_t kMavlinkV2Magic = 0xFD;
static const uint8_t kMavlinkV1HeaderLength = 6;
static const uint8_t kMavlinkV2HeaderLength = 10;
static const uint8_t kMavlinkV2SignatureFlag = 0x01;
static const uint32_t kHeartbeatMessageId = 0;
static const uint32_t kCommandLongMessageId = 76;
static const uint32_t kCommandAckMessageId = 77;
static const uint8_t kHeartbeatCrcExtra = 50;
static const uint8_t kCommandLongCrcExtra = 152;
static const uint8_t kCommandAckCrcExtra = 143;

static uint16_t mavlinkX25Accumulate(uint8_t data, uint16_t crc) {
    uint8_t tmp = data ^ static_cast<uint8_t>(crc & 0xFF);
    tmp ^= static_cast<uint8_t>(tmp << 4);
    return static_cast<uint16_t>((crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8) ^
                                 (static_cast<uint16_t>(tmp) << 3) ^
                                 (static_cast<uint16_t>(tmp) >> 4));
}

static uint16_t mavlinkComputeCrc(const uint8_t* data, size_t length, uint8_t crcExtra) {
    uint16_t crc = 0xFFFF;
    for (size_t index = 0; index < length; index++) {
        crc = mavlinkX25Accumulate(data[index], crc);
    }
    return mavlinkX25Accumulate(crcExtra, crc);
}

static float readFloatLe(const uint8_t* data) {
    float value = 0.0f;
    memcpy(&value, data, sizeof(value));
    return value;
}

static uint8_t clampParamToByte(float value, uint8_t fallback) {
    if (!isfinite(value)) {
        return fallback;
    }
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return static_cast<uint8_t>(lroundf(value));
}

static bool getMessageCrcExtra(uint32_t messageId, uint8_t* crcExtra) {
    if (!crcExtra) {
        return false;
    }
    switch (messageId) {
        case kHeartbeatMessageId:
            *crcExtra = kHeartbeatCrcExtra;
            return true;
        case kCommandLongMessageId:
            *crcExtra = kCommandLongCrcExtra;
            return true;
        case kCommandAckMessageId:
            *crcExtra = kCommandAckCrcExtra;
            return true;
        default:
            return false;
    }
}

}  // namespace

MavlinkCommandParser::MavlinkCommandParser()
    : length_(0), expectedLength_(0), mavlink2_(false) {}

void MavlinkCommandParser::reset() {
    length_ = 0;
    expectedLength_ = 0;
    mavlink2_ = false;
}

bool MavlinkCommandParser::ingest(uint8_t byte, MavlinkVtxCommand* command) {
    return ingest(byte, nullptr, command);
}

bool MavlinkCommandParser::ingest(uint8_t byte, MavlinkFrameInfo* frameInfo, MavlinkVtxCommand* command) {
    if (length_ == 0) {
        if (byte != kMavlinkV1Magic && byte != kMavlinkV2Magic) {
            return false;
        }
        mavlink2_ = (byte == kMavlinkV2Magic);
        buffer_[length_++] = byte;
        return false;
    }

    if (length_ >= sizeof(buffer_)) {
        reset();
        return false;
    }

    buffer_[length_++] = byte;

    const size_t headerLength = mavlink2_ ? kMavlinkV2HeaderLength : kMavlinkV1HeaderLength;
    if (expectedLength_ == 0 && length_ >= headerLength) {
        const size_t payloadLength = buffer_[1];
        const size_t signatureLength = (mavlink2_ && (buffer_[2] & kMavlinkV2SignatureFlag)) ? 13 : 0;
        expectedLength_ = headerLength + payloadLength + 2 + signatureLength;
        if (expectedLength_ > sizeof(buffer_)) {
            reset();
            return false;
        }
    }

    if (expectedLength_ == 0 || length_ < expectedLength_) {
        return false;
    }

    const bool parsed = parseFrame(frameInfo, command);
    reset();
    return parsed;
}

bool MavlinkCommandParser::parseFrame(MavlinkFrameInfo* frameInfo, MavlinkVtxCommand* command) const {
    const size_t headerLength = mavlink2_ ? kMavlinkV2HeaderLength : kMavlinkV1HeaderLength;
    const size_t payloadLength = buffer_[1];
    if (length_ < headerLength + payloadLength + 2) {
        return false;
    }

    uint32_t messageId = 0;
    if (mavlink2_) {
        messageId = static_cast<uint32_t>(buffer_[7]) |
                    (static_cast<uint32_t>(buffer_[8]) << 8) |
                    (static_cast<uint32_t>(buffer_[9]) << 16);
    } else {
        messageId = buffer_[5];
    }

    const uint8_t sourceSystem = mavlink2_ ? buffer_[5] : buffer_[3];
    const uint8_t sourceComponent = mavlink2_ ? buffer_[6] : buffer_[4];
    const uint8_t* payload = buffer_ + headerLength;

    uint8_t crcExtra = 0;
    if (!getMessageCrcExtra(messageId, &crcExtra)) {
        return false;
    }

    const uint16_t expectedCrc = mavlinkComputeCrc(buffer_ + 1, headerLength - 1 + payloadLength, crcExtra);
    const size_t crcOffset = headerLength + payloadLength;
    const uint16_t actualCrc = static_cast<uint16_t>(buffer_[crcOffset]) |
                               (static_cast<uint16_t>(buffer_[crcOffset + 1]) << 8);
    if (expectedCrc != actualCrc) {
        return false;
    }

    if (frameInfo) {
        frameInfo->complete = true;
        frameInfo->mavlink2 = mavlink2_;
        frameInfo->payloadLength = static_cast<uint8_t>(payloadLength);
        frameInfo->messageId = messageId;
        frameInfo->sourceSystem = sourceSystem;
        frameInfo->sourceComponent = sourceComponent;
        frameInfo->targetSystem = 0;
        frameInfo->targetComponent = 0;
        frameInfo->commandId = 0;
        if (messageId == kCommandLongMessageId && payloadLength >= 33) {
            frameInfo->commandId = static_cast<uint16_t>(payload[28]) |
                                   (static_cast<uint16_t>(payload[29]) << 8);
            frameInfo->targetSystem = payload[30];
            frameInfo->targetComponent = payload[31];
        }
    }

    if (messageId != kCommandLongMessageId) {
        return false;
    }

    if (!command) {
        return false;
    }

    if (payloadLength < 33) {
        return false;
    }

    const uint16_t commandId = static_cast<uint16_t>(payload[28]) |
                               (static_cast<uint16_t>(payload[29]) << 8);
    if (commandId != MAVLINK_VTX_COMMAND_ID) {
        return false;
    }

    command->commandId = commandId;
    command->sourceSystem = sourceSystem;
    command->sourceComponent = sourceComponent;
    command->targetSystem = payload[30];
    command->targetComponent = payload[31];
    command->nodeId = clampParamToByte(readFloatLe(payload + 0), 0);
    command->deviceId = clampParamToByte(readFloatLe(payload + 4), 0);
    command->band = clampParamToByte(readFloatLe(payload + 8), 0);
    command->channel = clampParamToByte(readFloatLe(payload + 12), 0);
    command->powerIndex = clampParamToByte(readFloatLe(payload + 16), MAVLINK_VTX_KEEP_VALUE);
    command->flags = clampParamToByte(readFloatLe(payload + 20), 0);
    return true;
}
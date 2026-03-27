#include "mavlink_bridge.h"

#include <math.h>
#include <string.h>

extern "C" {
#include <common/mavlink.h>
}

namespace {

static const uint32_t kHeartbeatMessageId = MAVLINK_MSG_ID_HEARTBEAT;
static const uint32_t kCommandLongMessageId = MAVLINK_MSG_ID_COMMAND_LONG;

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

}  // namespace

struct MavlinkCommandParser::OpaqueState {
    mavlink_message_t message;
    mavlink_status_t status;
};

MavlinkCommandParser::MavlinkCommandParser()
    : hasParsedFrame_(false), lastFrameInfo_(), lastCommand_(), state_(new OpaqueState()) {
    reset();
}

void MavlinkCommandParser::reset() {
    hasParsedFrame_ = false;
    memset(&lastFrameInfo_, 0, sizeof(lastFrameInfo_));
    memset(&lastCommand_, 0, sizeof(lastCommand_));
    if (state_) {
        memset(&state_->message, 0, sizeof(state_->message));
        memset(&state_->status, 0, sizeof(state_->status));
    }
}

bool MavlinkCommandParser::ingest(uint8_t byte, MavlinkVtxCommand* command) {
    return ingest(byte, nullptr, command);
}

bool MavlinkCommandParser::ingest(uint8_t byte, MavlinkFrameInfo* frameInfo, MavlinkVtxCommand* command) {
    if (!state_) {
        return false;
    }

    if (!mavlink_parse_char(MAVLINK_COMM_0, byte, &state_->message, &state_->status)) {
        return false;
    }

    hasParsedFrame_ = parseFrame(&lastFrameInfo_, &lastCommand_);
    if (frameInfo) {
        *frameInfo = lastFrameInfo_;
    }
    if (!hasParsedFrame_) {
        return false;
    }
    if (command) {
        *command = lastCommand_;
    }
    return lastCommand_.commandId == MAVLINK_VTX_COMMAND_ID;
}

bool MavlinkCommandParser::parseFrame(MavlinkFrameInfo* frameInfo, MavlinkVtxCommand* command) const {
    if (!state_ || !frameInfo) {
        return false;
    }

    memset(frameInfo, 0, sizeof(*frameInfo));
    frameInfo->complete = true;
    frameInfo->mavlink2 = (state_->message.magic == MAVLINK_STX);
    frameInfo->payloadLength = state_->message.len;
    frameInfo->messageId = state_->message.msgid;
    frameInfo->sourceSystem = state_->message.sysid;
    frameInfo->sourceComponent = state_->message.compid;

    if (state_->message.msgid != kCommandLongMessageId) {
        return false;
    }

    mavlink_command_long_t payload = {};
    mavlink_msg_command_long_decode(&state_->message, &payload);
    frameInfo->commandId = payload.command;
    frameInfo->targetSystem = payload.target_system;
    frameInfo->targetComponent = payload.target_component;

    if (!command || payload.command != MAVLINK_VTX_COMMAND_ID) {
        return false;
    }

    memset(command, 0, sizeof(*command));
    command->commandId = payload.command;
    command->sourceSystem = state_->message.sysid;
    command->sourceComponent = state_->message.compid;
    command->targetSystem = payload.target_system;
    command->targetComponent = payload.target_component;
    command->nodeId = clampParamToByte(payload.param1, 0);
    command->deviceId = clampParamToByte(payload.param2, 0);
    command->band = clampParamToByte(payload.param3, 0);
    command->channel = clampParamToByte(payload.param4, 0);
    command->powerIndex = clampParamToByte(payload.param5, MAVLINK_VTX_KEEP_VALUE);
    command->flags = clampParamToByte(payload.param6, 0);
    return true;
}

size_t mavlinkBuildHeartbeatFrame(uint8_t systemId, uint8_t componentId, uint8_t* buffer, size_t bufferSize) {
    if (!buffer || bufferSize < MAVLINK_MAX_PACKET_LEN) {
        return 0;
    }

    mavlink_message_t message = {};
    mavlink_msg_heartbeat_pack_chan(systemId,
                                    componentId,
                                    MAVLINK_COMM_0,
                                    &message,
                                    MAV_TYPE_ONBOARD_CONTROLLER,
                                    MAV_AUTOPILOT_INVALID,
                                    0,
                                    0,
                                    MAV_STATE_ACTIVE);
    return mavlink_msg_to_send_buffer(buffer, &message);
}

size_t mavlinkBuildCommandAckFrame(const MavlinkVtxCommand& command,
                                   uint8_t localSystemId,
                                   uint8_t localComponentId,
                                   uint8_t result,
                                   uint8_t* buffer,
                                   size_t bufferSize) {
    if (!buffer || bufferSize < MAVLINK_MAX_PACKET_LEN) {
        return 0;
    }

    mavlink_message_t message = {};
    mavlink_msg_command_ack_pack_chan(localSystemId,
                                      localComponentId,
                                      MAVLINK_COMM_0,
                                      &message,
                                      command.commandId,
                                      result,
                                      0,
                                      0,
                                      command.sourceSystem,
                                      command.sourceComponent);
    return mavlink_msg_to_send_buffer(buffer, &message);
}
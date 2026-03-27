#pragma once

#include <Arduino.h>

static const uint16_t MAVLINK_VTX_COMMAND_ID = 31001;
static const uint8_t MAVLINK_VTX_BROADCAST_NODE_ID = 255;
static const uint8_t MAVLINK_VTX_ALL_DEVICES_ID = 255;
static const uint16_t MAVLINK_VTX_KEEP_VALUE = 255;

struct MavlinkVtxCommand {
    uint16_t commandId;
    uint8_t sourceSystem;
    uint8_t sourceComponent;
    uint8_t targetSystem;
    uint8_t targetComponent;
    uint8_t nodeId;
    uint8_t deviceId;
    uint8_t band;
    uint8_t channel;
    uint16_t powerValue;
    uint8_t flags;
};

struct MavlinkFrameInfo {
    bool complete;
    bool mavlink2;
    uint8_t payloadLength;
    uint32_t messageId;
    uint8_t sourceSystem;
    uint8_t sourceComponent;
    uint8_t targetSystem;
    uint8_t targetComponent;
    uint16_t commandId;
};

class MavlinkCommandParser {
public:
    MavlinkCommandParser();

    bool ingest(uint8_t byte, MavlinkVtxCommand* command);
    bool ingest(uint8_t byte, MavlinkFrameInfo* frameInfo, MavlinkVtxCommand* command);
    void reset();

private:
    bool parseFrame(MavlinkFrameInfo* frameInfo, MavlinkVtxCommand* command) const;

    mutable bool hasParsedFrame_;
    mutable MavlinkFrameInfo lastFrameInfo_;
    mutable MavlinkVtxCommand lastCommand_;
    struct OpaqueState;
    OpaqueState* state_;
};

size_t mavlinkBuildHeartbeatFrame(uint8_t systemId, uint8_t componentId, uint8_t* buffer, size_t bufferSize);
size_t mavlinkBuildCommandAckFrame(const MavlinkVtxCommand& command,
                                   uint8_t localSystemId,
                                   uint8_t localComponentId,
                                   uint8_t result,
                                   uint8_t* buffer,
                                   size_t bufferSize);
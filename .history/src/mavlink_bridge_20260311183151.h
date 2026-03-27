#pragma once

#include <Arduino.h>

static const uint16_t MAVLINK_VTX_COMMAND_ID = 31001;
static const uint8_t MAVLINK_VTX_BROADCAST_NODE_ID = 255;
static const uint8_t MAVLINK_VTX_ALL_DEVICES_ID = 255;
static const uint8_t MAVLINK_VTX_KEEP_VALUE = 255;

struct MavlinkVtxCommand {
    uint16_t commandId;
    uint8_t nodeId;
    uint8_t deviceId;
    uint8_t band;
    uint8_t channel;
    uint8_t powerIndex;
    uint8_t flags;
};

class MavlinkCommandParser {
public:
    MavlinkCommandParser();

    bool ingest(uint8_t byte, MavlinkVtxCommand* command);
    void reset();

private:
    bool parseFrame(MavlinkVtxCommand* command) const;

    uint8_t buffer_[300];
    size_t length_;
    size_t expectedLength_;
    bool mavlink2_;
};
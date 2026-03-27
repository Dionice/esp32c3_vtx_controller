#pragma once

#include <Arduino.h>

static const uint8_t MAX_VTX_DEVICES = 6;
static const char DEFAULT_VTX_TABLE_PATH[] = "/peak_thor_t35.json";

enum VtxProtocol : uint8_t {
    VTX_PROTOCOL_SMARTAUDIO = 0,
    VTX_PROTOCOL_TRAMP = 1,
};

enum VtxControlMode : uint8_t {
    VTX_CONTROL_MODE_PWM = 0,
    VTX_CONTROL_MODE_SERIAL = 1,
    VTX_CONTROL_MODE_MAVLINK = 2,
};

enum BoardRole : uint8_t {
    BOARD_ROLE_STANDALONE = 0,
    BOARD_ROLE_FC_BRIDGE = 1,
    BOARD_ROLE_VTX_NODE = 2,
};

struct VtxDeviceConfig {
    char name[24];
    uint8_t pwmInputPin;
    uint8_t vtxControlPin;
    uint8_t protocol;
    uint8_t controlMode;
    bool enabled;
    uint8_t manualBand;
    uint8_t manualChannel;
    uint8_t manualPowerIndex;
    uint8_t mavlinkNodeId;
    uint8_t mavlinkDeviceId;
    char vtxTablePath[64];
};

struct AppConfig {
    uint8_t wifiChannel;
    bool espNowEnabled;
    uint8_t boardRole;
    uint8_t localNodeId;
    int8_t mavlinkRxPin;
    int8_t mavlinkTxPin;
    uint32_t mavlinkBaud;
    uint8_t deviceCount;
    VtxDeviceConfig devices[MAX_VTX_DEVICES];
};

static inline void appConfigSetDefaultDevice(VtxDeviceConfig& device, uint8_t index) {
    snprintf(device.name, sizeof(device.name), "VTX %u", static_cast<unsigned>(index + 1));
    device.pwmInputPin = static_cast<uint8_t>(3 + index);
    device.vtxControlPin = static_cast<uint8_t>(4 + index);
    device.protocol = VTX_PROTOCOL_SMARTAUDIO;
    device.controlMode = VTX_CONTROL_MODE_PWM;
    device.enabled = (index == 0);
    device.manualBand = 1;
    device.manualChannel = 1;
    device.manualPowerIndex = 0;
    device.mavlinkNodeId = 1;
    device.mavlinkDeviceId = static_cast<uint8_t>(index + 1);
    strncpy(device.vtxTablePath, DEFAULT_VTX_TABLE_PATH, sizeof(device.vtxTablePath) - 1);
    device.vtxTablePath[sizeof(device.vtxTablePath) - 1] = '\0';
}

static inline void appConfigSetDefaults(AppConfig& config) {
    config.wifiChannel = 1;
    config.espNowEnabled = false;
    config.boardRole = BOARD_ROLE_STANDALONE;
    config.localNodeId = 1;
    config.mavlinkRxPin = 5;
    config.mavlinkTxPin = 6;
    config.mavlinkBaud = 57600;
    config.deviceCount = 1;
    for (uint8_t index = 0; index < MAX_VTX_DEVICES; index++) {
        appConfigSetDefaultDevice(config.devices[index], index);
    }
}

static inline uint8_t appConfigClampDeviceCount(uint8_t count) {
    if (count == 0) {
        return 1;
    }
    return count > MAX_VTX_DEVICES ? MAX_VTX_DEVICES : count;
}

static inline const char* appConfigProtocolToString(uint8_t protocol) {
    switch (protocol) {
        case VTX_PROTOCOL_TRAMP:
            return "tramp";
        case VTX_PROTOCOL_SMARTAUDIO:
        default:
            return "smartaudio";
    }
}

static inline bool appConfigProtocolFromString(const String& input, uint8_t& protocol) {
    String normalized = input;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized == "smartaudio") {
        protocol = VTX_PROTOCOL_SMARTAUDIO;
        return true;
    }
    if (normalized == "tramp") {
        protocol = VTX_PROTOCOL_TRAMP;
        return true;
    }
    return false;
}

static inline const char* appConfigControlModeToString(uint8_t controlMode) {
    switch (controlMode) {
        case VTX_CONTROL_MODE_MAVLINK:
            return "mavlink";
        case VTX_CONTROL_MODE_SERIAL:
            return "serial";
        case VTX_CONTROL_MODE_PWM:
        default:
            return "pwm";
    }
}

static inline bool appConfigControlModeFromString(const String& input, uint8_t& controlMode) {
    String normalized = input;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized == "pwm") {
        controlMode = VTX_CONTROL_MODE_PWM;
        return true;
    }
    if (normalized == "serial") {
        controlMode = VTX_CONTROL_MODE_SERIAL;
        return true;
    }
    if (normalized == "mavlink") {
        controlMode = VTX_CONTROL_MODE_MAVLINK;
        return true;
    }
    return false;
}

static inline const char* appConfigBoardRoleToString(uint8_t boardRole) {
    switch (boardRole) {
        case BOARD_ROLE_FC_BRIDGE:
            return "fc_bridge";
        case BOARD_ROLE_VTX_NODE:
            return "vtx_node";
        case BOARD_ROLE_STANDALONE:
        default:
            return "standalone";
    }
}

static inline bool appConfigBoardRoleFromString(const String& input, uint8_t& boardRole) {
    String normalized = input;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized == "standalone") {
        boardRole = BOARD_ROLE_STANDALONE;
        return true;
    }
    if (normalized == "fc_bridge") {
        boardRole = BOARD_ROLE_FC_BRIDGE;
        return true;
    }
    if (normalized == "vtx_node") {
        boardRole = BOARD_ROLE_VTX_NODE;
        return true;
    }
    return false;
}
